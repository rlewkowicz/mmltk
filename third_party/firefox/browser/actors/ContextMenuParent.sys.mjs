/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  E10SUtils: "resource://gre/modules/E10SUtils.sys.mjs",
  WebNavigationFrames: "resource://gre/modules/WebNavigationFrames.sys.mjs",
});

XPCOMUtils.defineLazyServiceGetters(lazy, {
  BrowserHandler: ["@mozilla.org/browser/clh;1", Ci.nsIBrowserHandler],
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "TEXT_FRAGMENTS_ENABLED",
  "dom.text_fragments.enabled",
  false
);

export class ContextMenuParent extends JSWindowActorParent {
  receiveMessage(message) {
    let browser = this.manager.rootFrameLoader.ownerElement;
    if (browser.hasAttribute("disablecontextmenu")) {
      return;
    }

    let win = browser.documentGlobal;
    if (!win.nsContextMenu) {
      let topBrowser = browser.documentGlobal.docShell.chromeEventHandler;
      win = topBrowser.documentGlobal;
    }

    this.#openContextMenu(message.data, win, browser);
  }

  hiding() {
    try {
      this.sendAsyncMessage("ContextMenu:Hiding", {});
    } catch (e) {
    }
  }

  reloadFrame(targetIdentifier, forceReload) {
    this.sendAsyncMessage("ContextMenu:ReloadFrame", {
      targetIdentifier,
      forceReload,
    });
  }

  getImageText(targetIdentifier) {
    return this.sendQuery("ContextMenu:GetImageText", {
      targetIdentifier,
    });
  }

  reloadImage(targetIdentifier) {
    this.sendAsyncMessage("ContextMenu:ReloadImage", { targetIdentifier });
  }

  getFrameTitle(targetIdentifier) {
    return this.sendQuery("ContextMenu:GetFrameTitle", { targetIdentifier });
  }

  mediaCommand(targetIdentifier, command, data) {
    let windowGlobal = this.manager.browsingContext.currentWindowGlobal;
    let browser = windowGlobal.rootFrameLoader.ownerElement;
    let win = browser.documentGlobal;
    let windowUtils = win.windowUtils;
    this.sendAsyncMessage("ContextMenu:MediaCommand", {
      targetIdentifier,
      command,
      data,
      handlingUserInput: windowUtils.isHandlingUserInput,
    });
  }

  canvasToBlobURL(targetIdentifier) {
    return this.sendQuery("ContextMenu:Canvas:ToBlobURL", { targetIdentifier });
  }

  copyCanvasImage(targetIdentifier) {
    return this.sendQuery("ContextMenu:Canvas:CopyImage", {
      targetIdentifier,
    });
  }

  saveVideoFrameAsImage(targetIdentifier) {
    return this.sendQuery("ContextMenu:SaveVideoFrameAsImage", {
      targetIdentifier,
    });
  }

  setAsDesktopBackground(targetIdentifier) {
    return this.sendQuery("ContextMenu:SetAsDesktopBackground", {
      targetIdentifier,
    });
  }

  getSearchFieldEngineData(targetIdentifier) {
    return this.sendQuery("ContextMenu:SearchFieldEngineData", {
      targetIdentifier,
    });
  }

  getTextDirective() {
    return lazy.TEXT_FRAGMENTS_ENABLED
      ? this.sendQuery("ContextMenu:GetTextDirective")
      : null;
  }

  removeAllTextFragments() {
    return this.sendQuery("ContextMenu:RemoveAllTextFragments");
  }

  #openContextMenu(data, win, browser) {
    if (lazy.BrowserHandler.kiosk) {
      return;
    }
    let wgp = this.manager;

    if (!wgp.isCurrentGlobal) {
      return;
    }

    let documentURIObject = wgp.browsingContext.currentURI;

    let frameReferrerInfo = data.frameReferrerInfo;
    if (frameReferrerInfo) {
      frameReferrerInfo =
        lazy.E10SUtils.deserializeReferrerInfo(frameReferrerInfo);
    }

    let linkReferrerInfo = data.linkReferrerInfo;
    if (linkReferrerInfo) {
      linkReferrerInfo =
        lazy.E10SUtils.deserializeReferrerInfo(linkReferrerInfo);
    }

    let frameID = lazy.WebNavigationFrames.getFrameId(wgp.browsingContext);

    win.nsContextMenu.contentData = {
      context: data.context,
      browser,
      actor: this,
      editFlags: data.editFlags,
      spellInfo: data.spellInfo,
      principal: wgp.documentPrincipal,
      storagePrincipal: wgp.documentStoragePrincipal,
      documentURIObject,
      docLocation: documentURIObject.spec,
      charSet: data.charSet,
      referrerInfo: lazy.E10SUtils.deserializeReferrerInfo(data.referrerInfo),
      frameReferrerInfo,
      linkReferrerInfo,
      contentType: data.contentType,
      contentDisposition: data.contentDisposition,
      frameID,
      frameOuterWindowID: frameID,
      frameBrowsingContext: wgp.browsingContext,
      selectionInfo: data.selectionInfo,
      disableSetDesktopBackground: data.disableSetDesktopBackground,
      userContextId: wgp.browsingContext.originAttributes.userContextId,
      cookieJarSettings: wgp.cookieJarSettings,
    };

    let popup = win.document.getElementById("contentAreaContextMenu");
    let context = win.nsContextMenu.contentData.context;

    context.principal = wgp.documentPrincipal;
    context.storagePrincipal = wgp.documentStoragePrincipal;
    context.frameID = frameID;
    context.frameOuterWindowID = wgp.outerWindowId;
    context.frameBrowsingContextID = wgp.browsingContext.id;

    let newEvent = new PointerEvent("contextmenu", {
      bubbles: true,
      cancelable: true,
      screenX: context.screenXDevPx / win.devicePixelRatio,
      screenY: context.screenYDevPx / win.devicePixelRatio,
      button: 2,
      pointerType: (() => {
        switch (context.inputSource) {
          case MouseEvent.MOZ_SOURCE_MOUSE:
            return "mouse";
          case MouseEvent.MOZ_SOURCE_PEN:
            return "pen";
          case MouseEvent.MOZ_SOURCE_ERASER:
            return "eraser";
          case MouseEvent.MOZ_SOURCE_CURSOR:
            return "cursor";
          case MouseEvent.MOZ_SOURCE_TOUCH:
            return "touch";
          case MouseEvent.MOZ_SOURCE_KEYBOARD:
            return "keyboard";
          default:
            return "";
        }
      })(),
    });
    popup.openPopupAtScreen(newEvent.screenX, newEvent.screenY, true, newEvent);
  }
}
