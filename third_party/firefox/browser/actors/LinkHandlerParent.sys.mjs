/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import {
  TYPE_ICO,
  SVG_DATA_URI_PREFIX,
  TRUSTED_FAVICON_SCHEMES,
  blobAsDataURL,
} from "moz-src:///toolkit/modules/FaviconUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  OpenSearchManager:
    "moz-src:///browser/components/search/OpenSearchManager.sys.mjs",
});

let gTestListeners = new Set();

async function drawImageOnCanvas(canvas, image) {
  let data = await image.blob.bytes();
  let frame = new VideoFrame(data, {
    timestamp: 0,
    format: image.format,
    codedWidth: image.displayWidth,
    codedHeight: image.displayHeight,
  });

  canvas.width = frame.displayWidth;
  canvas.height = frame.displayHeight;
  let ctx = canvas.getContext("2d");
  ctx.drawImage(frame, 0, 0);
}

function createICO(images) {
  const ICO_HEADER_SIZE = 6;
  const ICO_DIR_ENTRY_SIZE = 16;

  const metadataSize = ICO_HEADER_SIZE + ICO_DIR_ENTRY_SIZE * images.length;
  const size =
    metadataSize + images.reduce((acc, image) => acc + image.byteLength, 0);

  let buffer = new ArrayBuffer(size);
  let u8 = new Uint8Array(buffer);
  let view = new DataView(buffer);

  view.setUint16(0, 0, true); 
  view.setUint16(2, 1, true); 
  view.setUint16(4, images.length, true); 

  let dataOffset = metadataSize; 
  for (let i = 0; i < images.length; i++) {
    const off = ICO_HEADER_SIZE + ICO_DIR_ENTRY_SIZE * i;

    view.setUint8(off, 0); 
    view.setUint8(off + 1, 0); 
    view.setUint8(off + 2, 0); 
    view.setUint8(off + 3, 0); 
    view.setUint16(off + 4, 1, true); 
    view.setUint16(off + 6, 32, true); 
    view.setUint32(off + 8, images[i].byteLength, true); 
    view.setUint32(off + 12, dataOffset, true); 

    u8.set(images[i], dataOffset);

    dataOffset += images[i].byteLength;
  }

  return buffer;
}

export class LinkHandlerParent extends JSWindowActorParent {
  static addListenerForTests(listener) {
    gTestListeners.add(listener);
  }

  static removeListenerForTests(listener) {
    gTestListeners.delete(listener);
  }

  receiveMessage(aMsg) {
    let browser = this.browsingContext.top.embedderElement;
    if (!browser) {
      return;
    }

    let win = browser.documentGlobal;

    let gBrowser = win.gBrowser;

    switch (aMsg.name) {
      case "Link:LoadingIcon":
        if (!gBrowser) {
          return;
        }

        if (!aMsg.data.isRichIcon) {
          let tab = gBrowser.getTabForBrowser(browser);
          if (tab.hasAttribute("busy")) {
            tab.setAttribute("pendingicon", "true");
          }
        }

        this.notifyTestListeners("LoadingIcon", aMsg.data);
        break;

      case "Link:SetIcon":
        if (!gBrowser) {
          return;
        }

        this.setIconFromLink(gBrowser, browser, aMsg.data);

        this.notifyTestListeners("SetIcon", aMsg.data);
        break;

      case "Link:SetFailedIcon":
        if (!gBrowser) {
          return;
        }

        if (!aMsg.data.isRichIcon) {
          this.clearPendingIcon(gBrowser, browser);
        }

        this.notifyTestListeners("SetFailedIcon", aMsg.data);
        break;

      case "Link:AddSearch": {
        if (!gBrowser) {
          return;
        }

        let tab = gBrowser.getTabForBrowser(browser);
        if (!tab) {
          break;
        }

        lazy.OpenSearchManager.addEngine(browser, aMsg.data.engine);
        break;
      }
    }
  }

  notifyTestListeners(name, data) {
    for (let listener of gTestListeners) {
      listener(name, data);
    }
  }

  clearPendingIcon(gBrowser, aBrowser) {
    let tab = gBrowser.getTabForBrowser(aBrowser);
    tab.removeAttribute("pendingicon");
  }

  async setIconFromLink(
    gBrowser,
    browser,
    {
      pageURL,
      originalURL,
      expiration,
      iconURL,
      images,
      canStoreIcon,
      beforePageShow,
      isRichIcon,
    }
  ) {
    let tab = gBrowser.getTabForBrowser(browser);
    if (!tab) {
      return;
    }

    if (images) {
      let canvas = tab.ownerDocument.createElement("canvas");

      if (images.length > 1) {
        let blobs = [];
        for (let image of images) {
          await drawImageOnCanvas(canvas, image);
          blobs.push(await new Promise(resolve => canvas.toBlob(resolve)));
        }
        let buffers = await Promise.all(blobs.map(blob => blob.bytes()));

        let ico = createICO(buffers);

        iconURL = await blobAsDataURL(new Blob([ico], { type: TYPE_ICO }));
      } else {
        await drawImageOnCanvas(canvas, images[0]);
        iconURL = canvas.toDataURL();
      }
    }

    if (!gBrowser.getBrowserForTab(tab)) {
      return;
    }

    if (!isRichIcon) {
      this.clearPendingIcon(gBrowser, browser);
    }

    let iconURI;
    try {
      iconURI = Services.io.newURI(iconURL);
    } catch (ex) {
      console.error(ex);
      return;
    }

    if (
      !images &&
      !TRUSTED_FAVICON_SCHEMES.includes(iconURI.scheme) &&
      !iconURL.startsWith(SVG_DATA_URI_PREFIX)
    ) {
      console.error(
        `Not allowed to set favicon "${iconURL}" with that scheme!`
      );
      return;
    }

    if (!iconURI.schemeIs("data")) {
      try {
        Services.scriptSecurityManager.checkLoadURIWithPrincipal(
          browser.contentPrincipal,
          iconURI,
          Services.scriptSecurityManager.ALLOW_CHROME
        );
      } catch (ex) {
        return;
      }
    }
    if (canStoreIcon && AppConstants.MOZ_PLACES) {
      try {
        lazy.PlacesUtils.favicons
          .setFaviconForPage(
            Services.io.newURI(pageURL),
            Services.io.newURI(originalURL),
            iconURI,
            expiration && lazy.PlacesUtils.toPRTime(expiration),
            isRichIcon
          )
          .catch(console.error);
      } catch (ex) {
        console.error(ex);
      }
    }

    if (!isRichIcon) {
      gBrowser.setIcon(tab, iconURL, originalURL, beforePageShow);
    }
  }
}
