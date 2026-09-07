/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  E10SUtils: "resource://gre/modules/E10SUtils.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  setTimeout: "resource://gre/modules/Timer.sys.mjs",
});

export class PageInfoChild extends JSWindowActorChild {
  async receiveMessage(message) {
    let window = this.contentWindow;
    let document = window.document;

    switch (message.name) {
      case "PageInfo:getData": {
        return Promise.resolve({
          metaViewRows: this.getMetaInfo(document),
          docInfo: this.getDocumentInfo(document),
          windowInfo: this.getWindowInfo(window),
        });
      }
      case "PageInfo:getMediaData": {
        return Promise.resolve({
          mediaItems: await this.getDocumentMedia(document),
        });
      }
      case "PageInfo:getPartitionKey": {
        return Promise.resolve({
          partitionKey: await this.getPartitionKey(document),
        });
      }
    }

    return undefined;
  }

  getPartitionKey(document) {
    let partitionKey = document.cookieJarSettings.partitionKey;
    return partitionKey;
  }

  getMetaInfo(document) {
    let metaViewRows = [];

    let metaNodes = document.getElementsByTagName("meta");

    for (let metaNode of metaNodes) {
      metaViewRows.push([
        metaNode.name ||
          metaNode.httpEquiv ||
          metaNode.getAttribute("property"),
        metaNode.content,
      ]);
    }

    return metaViewRows;
  }

  getWindowInfo(window) {
    let windowInfo = {};
    windowInfo.isTopWindow = window == window.top;

    let hostName = null;
    try {
      hostName = Services.io.newURI(window.location.href).displayHost;
    } catch (exception) {}

    windowInfo.hostName = hostName;
    return windowInfo;
  }

  getDocumentInfo(document) {
    let docInfo = {};
    docInfo.title = document.title;
    docInfo.location = document.location.toString();
    try {
      docInfo.location = Services.io.newURI(
        document.location.toString()
      ).displaySpec;
    } catch (exception) {}
    docInfo.referrer = document.referrer;
    try {
      if (document.referrer) {
        docInfo.referrer = Services.io.newURI(document.referrer).displaySpec;
      }
    } catch (exception) {}
    docInfo.compatMode = document.compatMode;
    docInfo.contentType = document.contentType;
    docInfo.characterSet = document.characterSet;
    docInfo.lastModified = document.lastModified;
    docInfo.principal = document.nodePrincipal;
    docInfo.cookieJarSettings = lazy.E10SUtils.serializeCookieJarSettings(
      document.cookieJarSettings
    );

    let documentURIObject = {};
    documentURIObject.spec = document.documentURIObject.spec;
    docInfo.documentURIObject = documentURIObject;

    docInfo.isContentWindowPrivate =
      lazy.PrivateBrowsingUtils.isContentWindowPrivate(document.documentGlobal);

    return docInfo;
  }

  async getDocumentMedia(document) {
    let nodeCount = 0;
    let content = document.documentGlobal;
    let iterator = document.createTreeWalker(
      document,
      content.NodeFilter.SHOW_ELEMENT
    );

    let totalMediaItems = [];

    while (iterator.nextNode()) {
      let mediaItems = this.getMediaItems(document, iterator.currentNode);

      if (++nodeCount % 500 == 0) {
        await new Promise(resolve => lazy.setTimeout(resolve, 10));
      }
      totalMediaItems.push(...mediaItems);
    }

    return totalMediaItems;
  }

  getMediaItems(document, elem) {
    let computedStyle = elem.documentGlobal.getComputedStyle(elem);
    let mediaItems = [];
    let content = document.documentGlobal;

    let addMedia = (url, type, alt, el, isBg, altNotProvided = false) => {
      let element = this.serializeElementInfo(document, url, el, isBg);
      mediaItems.push({
        url,
        type,
        alt,
        altNotProvided,
        element,
        isBg,
      });
    };

    if (computedStyle) {
      let addImgFunc = (type, urls) => {
        for (let url of urls) {
          addMedia(url, type, "", elem, true, true);
        }
      };
      addImgFunc("bg-img", computedStyle.getCSSImageURLs("background-image"));
      addImgFunc(
        "border-img",
        computedStyle.getCSSImageURLs("border-image-source")
      );
      addImgFunc("list-img", computedStyle.getCSSImageURLs("list-style-image"));
      addImgFunc("cursor", computedStyle.getCSSImageURLs("cursor"));
    }

    if (content.HTMLImageElement.isInstance(elem)) {
      addMedia(
        elem.currentSrc,
        "img",
        elem.getAttribute("alt"),
        elem,
        false,
        !elem.hasAttribute("alt")
      );
    } else if (content.SVGImageElement.isInstance(elem)) {
      if (elem.href.baseVal) {
        let href = URL.parse(elem.href.baseVal, elem.baseURI)?.href;
        if (href) {
          addMedia(href, "img", "", elem, false);
        }
      }
    } else if (content.HTMLVideoElement.isInstance(elem)) {
      addMedia(elem.currentSrc, "video", "", elem, false);
    } else if (content.HTMLAudioElement.isInstance(elem)) {
      addMedia(elem.currentSrc, "audio", "", elem, false);
    } else if (content.HTMLLinkElement.isInstance(elem)) {
      if (elem.rel && /\bicon\b/i.test(elem.rel)) {
        addMedia(elem.href, "link", "", elem, false);
      }
    } else if (
      content.HTMLInputElement.isInstance(elem) ||
      content.HTMLButtonElement.isInstance(elem)
    ) {
      if (elem.type.toLowerCase() == "image") {
        addMedia(
          elem.src,
          "input",
          elem.getAttribute("alt"),
          elem,
          false,
          !elem.hasAttribute("alt")
        );
      }
    } else if (content.HTMLObjectElement.isInstance(elem)) {
      addMedia(elem.data, "object", this.getValueText(elem), elem, false);
    } else if (content.HTMLEmbedElement.isInstance(elem)) {
      addMedia(elem.src, "embed", "", elem, false);
    }

    return mediaItems;
  }


  serializeElementInfo(document, url, item, isBG) {
    let result = {};
    let content = document.documentGlobal;

    let imageText;
    if (
      !isBG &&
      !content.SVGImageElement.isInstance(item) &&
      !content.ImageDocument.isInstance(document)
    ) {
      imageText = item.title || item.alt;

      if (!imageText && !content.HTMLImageElement.isInstance(item)) {
        imageText = this.getValueText(item);
      }
    }

    result.imageText = imageText;
    result.longDesc = item.longDesc;
    result.numFrames = 1;

    if (
      content.HTMLObjectElement.isInstance(item) ||
      content.HTMLEmbedElement.isInstance(item) ||
      content.HTMLLinkElement.isInstance(item)
    ) {
      result.mimeType = item.type;
    }

    if (
      !result.mimeType &&
      !isBG &&
      item instanceof Ci.nsIImageLoadingContent
    ) {
      let imageRequest = item.getRequest(
        Ci.nsIImageLoadingContent.CURRENT_REQUEST
      );
      if (imageRequest) {
        result.mimeType = imageRequest.mimeType;
        let image =
          !(imageRequest.imageStatus & imageRequest.STATUS_ERROR) &&
          imageRequest.image;
        if (image) {
          result.numFrames = image.numFrames;
        }
      }
    }

    if (!result.mimeType && url.startsWith("data:")) {
      let dataMimeType = /^data:(image\/[^;,]+)/i.exec(url);
      if (dataMimeType) {
        result.mimeType = dataMimeType[1].toLowerCase();
      }
    }

    result.HTMLLinkElement = content.HTMLLinkElement.isInstance(item);
    result.HTMLInputElement = content.HTMLInputElement.isInstance(item);
    result.HTMLImageElement = content.HTMLImageElement.isInstance(item);
    result.HTMLObjectElement = content.HTMLObjectElement.isInstance(item);
    result.SVGImageElement = content.SVGImageElement.isInstance(item);
    result.HTMLVideoElement = content.HTMLVideoElement.isInstance(item);
    result.HTMLAudioElement = content.HTMLAudioElement.isInstance(item);

    if (isBG) {
      let img = content.document.createElement("img");
      img.src = url;
      result.naturalWidth = img.naturalWidth;
      result.naturalHeight = img.naturalHeight;
    } else if (!content.SVGImageElement.isInstance(item)) {

      result.width = item.width;
      result.height = item.height;
    }

    if (content.SVGImageElement.isInstance(item)) {
      result.SVGImageElementWidth = item.width.baseVal.value;
      result.SVGImageElementHeight = item.height.baseVal.value;
    }

    result.baseURI = item.baseURI;

    return result;
  }

  getValueText(node) {
    let valueText = "";
    let content = node.documentGlobal;

    if (
      content.HTMLInputElement.isInstance(node) ||
      content.HTMLSelectElement.isInstance(node) ||
      content.HTMLTextAreaElement.isInstance(node)
    ) {
      return valueText;
    }

    let length = node.childNodes.length;

    for (let i = 0; i < length; i++) {
      let childNode = node.childNodes[i];
      let nodeType = childNode.nodeType;

      if (nodeType == content.Node.TEXT_NODE) {
        valueText += " " + childNode.nodeValue;
      } else if (nodeType == content.Node.ELEMENT_NODE) {
        if (content.HTMLImageElement.isInstance(childNode)) {
          valueText += " " + this.getAltText(childNode);
        } else {
          valueText += " " + this.getValueText(childNode);
        }
      }
    }

    return this.stripWS(valueText);
  }

  getAltText(node) {
    let altText = "";

    if (node.alt) {
      return node.alt;
    }
    let length = node.childNodes.length;
    for (let i = 0; i < length; i++) {
      if ((altText = this.getAltText(node.childNodes[i]) != undefined)) {
        return altText;
      }
    }
    return "";
  }

  stripWS(text) {
    let middleRE = /\s+/g;
    let endRE = /(^\s+)|(\s+$)/g;

    text = text.replace(middleRE, " ");
    return text.replace(endRE, "");
  }
}
