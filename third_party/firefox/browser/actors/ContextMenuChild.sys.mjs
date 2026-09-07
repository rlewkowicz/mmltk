/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ContentDOMReference: "resource://gre/modules/ContentDOMReference.sys.mjs",
  E10SUtils: "resource://gre/modules/E10SUtils.sys.mjs",
  SelectionUtils: "resource://gre/modules/SelectionUtils.sys.mjs",
});

const EditFlags = {
  EDITABLE: 0x1,
  INPUT: 0x2,
  TEXTAREA: 0x4,
  TEXTINPUT: 0x8,
  SEARCHENGINE: 0x10,
  CONTENTEDITABLE: 0x20,
  NUMERIC: 0x40,
  PASSWORD: 0x80,

  isTargetASearchEngineField(node, window) {
    if (
      !window.HTMLInputElement.isInstance(node) ||
      (node.type != "text" && node.type != "search") ||
      node.readOnly ||
      !node.name ||
      !node.form
    ) {
      return false;
    }
    const method = node.form.method.toUpperCase();
    return (
      node.form.hasAttribute("action") &&
      (method == "GET" || (method == "POST" && node.form.role == "search")) &&
      node.form.enctype == "application/x-www-form-urlencoded" &&
      new FormData(node.form).values().every(value => typeof value == "string")
    );
  },

  isEditable(element, window) {
    let flags = 0;
    if (window.HTMLInputElement.isInstance(element)) {
      flags |= this.INPUT;
      if (element.mozIsTextField(false) || element.type == "number") {
        flags |= this.TEXTINPUT;
        if (!element.readOnly) {
          flags |= this.EDITABLE;
        }
        if (element.type == "number") {
          flags |= this.NUMERIC;
        }
        if (this.isTargetASearchEngineField(element, window)) {
          flags |= this.SEARCHENGINE;
        }
        if (element.type == "password") {
          flags |= this.PASSWORD;
        }
      }
    } else if (window.HTMLTextAreaElement.isInstance(element)) {
      flags |= this.TEXTINPUT | this.TEXTAREA;
      if (!element.readOnly) {
        flags |= this.EDITABLE;
      }
    }

    const contentWindow = element.documentGlobal;
    if (contentWindow) {
      try {
        if (
          contentWindow.docShell.editingSession.windowIsEditable(contentWindow) &&
          element.matches(":read-write")
        ) {
          flags |= this.CONTENTEDITABLE | this.EDITABLE;
        }
      } catch (error) {}
    }
    return flags;
  },
};

let contextMenus = new WeakMap();

export class ContextMenuChild extends JSWindowActorChild {
  constructor() {
    super();

    this.target = null;
    this.context = null;
  }

  static getTarget(browsingContext, message, key) {
    let actor = contextMenus.get(browsingContext);
    if (!actor) {
      throw new Error(
        "Can't find ContextMenu actor for browsing context with " +
          "ID: " +
          browsingContext.id
      );
    }
    return actor.getTarget(message, key);
  }

  receiveMessage(message) {
    switch (message.name) {
      case "ContextMenu:GetFrameTitle": {
        let target = lazy.ContentDOMReference.resolve(
          message.data.targetIdentifier
        );
        return Promise.resolve(target.ownerDocument.title);
      }

      case "ContextMenu:Canvas:ToBlobURL": {
        let target = lazy.ContentDOMReference.resolve(
          message.data.targetIdentifier
        );
        return new Promise(resolve => {
          target.toBlob(blob => {
            let blobURL = URL.createObjectURL(blob);
            resolve(blobURL);
          });
        });
      }

      case "ContextMenu:Canvas:CopyImage": {
        let target = lazy.ContentDOMReference.resolve(
          message.data.targetIdentifier
        );
        return new Promise(resolve => {
          target.toBlob(blob => resolve(blob.arrayBuffer()));
        });
      }

      case "ContextMenu:Hiding": {
        this.context = null;
        this.target = null;
        break;
      }

      case "ContextMenu:MediaCommand": {
        lazy.E10SUtils.wrapHandlingUserInput(
          this.contentWindow,
          message.data.handlingUserInput,
          () => {
            let media = lazy.ContentDOMReference.resolve(
              message.data.targetIdentifier
            );

            switch (message.data.command) {
              case "play":
                media.play();
                break;
              case "pause":
                media.pause();
                break;
              case "loop":
                media.loop = !media.loop;
                break;
              case "mute":
                media.muted = true;
                break;
              case "unmute":
                media.muted = false;
                break;
              case "playbackRate":
                media.playbackRate = message.data.data;
                break;
              case "hidecontrols":
                media.removeAttribute("controls");
                break;
              case "showcontrols":
                media.setAttribute("controls", "true");
                break;
              case "fullscreen":
                if (this.document.fullscreenEnabled) {
                  media.requestFullscreen();
                }
                break;
              case "pictureinpicture": {
                let event = new this.contentWindow.CustomEvent(
                  "MozTogglePictureInPicture",
                  {
                    bubbles: true,
                    detail: { reason: "ContextMenu" },
                  },
                  this.contentWindow
                );
                this.contentWindow.windowUtils.dispatchEventToChromeOnly(
                  media,
                  event
                );
                break;
              }
            }
          }
        );
        break;
      }

      case "ContextMenu:ReloadFrame": {
        let target = lazy.ContentDOMReference.resolve(
          message.data.targetIdentifier
        );
        target.ownerDocument.location.reload(message.data.forceReload);
        break;
      }

      case "ContextMenu:GetImageText": {
        let img = lazy.ContentDOMReference.resolve(
          message.data.targetIdentifier
        );
        const { direction } = this.contentWindow.getComputedStyle(img);

        return img.recognizeCurrentImageText().then(results => {
          return { results, direction };
        });
      }

      case "ContextMenu:ReloadImage": {
        let image = lazy.ContentDOMReference.resolve(
          message.data.targetIdentifier
        );

        if (image instanceof Ci.nsIImageLoadingContent) {
          image.forceReload();
        }
        break;
      }

      case "ContextMenu:SearchFieldEngineData": {
        let node = lazy.ContentDOMReference.resolve(
          message.data.targetIdentifier
        );
        let charset = node.ownerDocument.characterSet;
        let formBaseURI = Services.io.newURI(node.form.baseURI, charset);
        let method = node.form.method.toUpperCase();

        let formData = new FormData(node.form);
        formData.set(node.name, "{searchTerms}");

        let url = Services.io.newURI(
          node.form.getAttribute("action"),
          charset,
          formBaseURI
        ).spec;

        if (
          !node.name ||
          (method != "POST" && method != "GET") ||
          node.form.enctype != "application/x-www-form-urlencoded" ||
          formData.values().some(v => typeof v != "string")
        ) {
          return Promise.reject("Cannot create search engine from this form.");
        }

        return Promise.resolve({ url, formData, charset, method });
      }

      case "ContextMenu:SaveVideoFrameAsImage": {
        let video = lazy.ContentDOMReference.resolve(
          message.data.targetIdentifier
        );
        let canvas = this.document.createElementNS(
          "http://www.w3.org/1999/xhtml",
          "canvas"
        );
        canvas.width = video.videoWidth;
        canvas.height = video.videoHeight;

        let ctxDraw = canvas.getContext("2d");
        ctxDraw.drawImage(video, 0, 0);

        return Promise.resolve(canvas.toDataURL("image/jpeg", ""));
      }

      case "ContextMenu:SetAsDesktopBackground": {
        let target = lazy.ContentDOMReference.resolve(
          message.data.targetIdentifier
        );

        let disable = this._disableSetDesktopBackground(target);

        if (!disable) {
          try {
            let canvas;
            if (this.contentWindow.HTMLCanvasElement.isInstance(target)) {
              canvas = target;
            } else {
              Services.scriptSecurityManager.checkLoadURIWithPrincipal(
                target.ownerDocument.nodePrincipal,
                target.currentURI
              );
              canvas = this.document.createElement("canvas");
              canvas.width = target.naturalWidth;
              canvas.height = target.naturalHeight;
              let ctx = canvas.getContext("2d");
              ctx.drawImage(target, 0, 0);
            }
            let dataURL = canvas.toDataURL();
            let url = target.ownerDocument.location;
            let imageName = url.pathname.substr(
              url.pathname.lastIndexOf("/") + 1
            );
            return Promise.resolve({ failed: false, dataURL, imageName });
          } catch (e) {
            console.error(e);
          }
        }

        return Promise.resolve({
          failed: true,
          dataURL: null,
          imageName: null,
        });
      }

      case "ContextMenu:GetTextDirective": {
        const sel = this.contentWindow.getSelection();
        const ranges = !sel.isCollapsed
          ? Array.from({ length: sel.rangeCount }, (_, i) => sel.getRangeAt(i))
          : this.document.fragmentDirective.getTextDirectiveRanges();
        return ranges
          ? this.document.fragmentDirective
              .createTextDirectiveForRanges(ranges)
              .then(textFragment => {
                if (textFragment) {
                  let url = URL.fromURI(this.document?.documentURIObject);
                  url.hash += `:~:${textFragment}`;
                  return url.href;
                }
                return null;
              })
          : null;
      }
      case "ContextMenu:RemoveAllTextFragments": {
        this.document.fragmentDirective.removeAllTextDirectives();
        this.contentWindow.history.replaceState(
          this.contentWindow.history.state,
          "",
          this.contentWindow.location.href
        );
      }
    }

    return undefined;
  }

  getTarget(aMessage, aKey = "target") {
    return this.target || (aMessage.objects && aMessage.objects[aKey]);
  }

  _isXULTextLinkLabel(aNode) {
    const XUL_NS =
      "http://www.mozilla.org/keymaster/gatekeeper/there.is.only.xul";
    return (
      aNode.namespaceURI == XUL_NS &&
      aNode.tagName == "label" &&
      aNode.classList.contains("text-link") &&
      aNode.href
    );
  }

  _getLinkURL() {
    let href = this.context.link.href;

    if (href) {
      if (typeof href == "object" && href.animVal) {
        return new URL(href.animVal, this.context.link.baseURI).href;
      }

      return href;
    }

    href =
      this.context.link.getAttribute("href") ||
      this.context.link.getAttributeNS("http://www.w3.org/1999/xlink", "href");

    if (!href || !href.match(/\S/)) {
      throw new Error("Empty href");
    }

    return new URL(href, this.context.link.baseURI).href;
  }

  _getLinkURI() {
    try {
      return Services.io.newURI(this.context.linkURL);
    } catch (ex) {
    }

    return null;
  }

  _getLinkText() {
    let text = this._gatherTextUnder(this.context.link);

    if (!text || !text.match(/\S/)) {
      text = this.context.link.getAttribute("title");
      if (!text || !text.match(/\S/)) {
        text = this.context.link.getAttribute("alt");
        if (!text || !text.match(/\S/)) {
          text = this.context.linkURL;
        }
      }
    }

    return text;
  }

  _getLinkProtocol() {
    if (this.context.linkURI) {
      return this.context.linkURI.scheme; 
    }

    return null;
  }

  _isLinkSaveable() {
    return (
      this.context.linkProtocol &&
      !(
        this.context.linkProtocol == "mailto" ||
        this.context.linkProtocol == "tel" ||
        this.context.linkProtocol == "javascript" ||
        this.context.linkProtocol == "news" ||
        this.context.linkProtocol == "snews"
      )
    );
  }

  _gatherTextUnder(root) {
    const encoder = Cu.createDocumentEncoder("text/plain");
    encoder.init(root.ownerDocument, "text/plain", 0);
    encoder.setContainerNode(root);
    return encoder.encodeToString().trim();
  }

  _getComputedURL(aElem, aProp) {
    let urls = aElem.documentGlobal
      .getComputedStyle(aElem)
      .getCSSImageURLs(aProp);

    if (!urls.length) {
      return null;
    }

    if (urls.length != 1) {
      throw new Error("found multiple URLs");
    }

    return urls[0];
  }

  _isProprietaryDRM() {
    return (
      this.context.target.isEncrypted &&
      this.context.target.mediaKeys &&
      this.context.target.mediaKeys.keySystem != "org.w3.clearkey"
    );
  }

  _isMediaURLReusable(aURL) {
    if (aURL.startsWith("blob:")) {
      return URL.isBoundToBlob(aURL);
    }

    return true;
  }

  _maybeGetVideoElementAtPoint(clientX, clientY) {
    if (
      !Services.prefs.getBoolPref(
        "media.contextmenu.video-overlay-detection",
        false
      )
    ) {
      return null;
    }

    let elements = this.contentWindow.windowUtils.nodesFromRect(
      clientX,
      clientY,
      1,
      1,
      1,
      1,
      true,
      false,
      true,
      0
    );
    for (let el of elements) {
      if (this.contentWindow.HTMLVideoElement.isInstance(el)) {
        return el;
      }
    }
    return null;
  }

  _isTargetATextBox(node) {
    if (this.contentWindow.HTMLInputElement.isInstance(node)) {
      return node.mozIsTextField(false);
    }

    return this.contentWindow.HTMLTextAreaElement.isInstance(node);
  }

  _disableSetDesktopBackground(aTarget) {
    if (this.contentWindow.HTMLCanvasElement.isInstance(aTarget)) {
      return false;
    }

    if (!(aTarget instanceof Ci.nsIImageLoadingContent)) {
      return true;
    }

    if ("complete" in aTarget && !aTarget.complete) {
      return true;
    }

    if (aTarget.currentURI.schemeIs("javascript")) {
      return true;
    }

    let request = aTarget.getRequest(Ci.nsIImageLoadingContent.CURRENT_REQUEST);

    if (!request) {
      return true;
    }

    return false;
  }

  async handleEvent(aEvent) {
    contextMenus.set(this.browsingContext, this);

    let defaultPrevented = aEvent.defaultPrevented;

    if (
      !aEvent.composedTarget.nodePrincipal.isSystemPrincipal &&
      !Services.prefs.getBoolPref("dom.event.contextmenu.enabled")
    ) {
      defaultPrevented = false;
    }

    if (defaultPrevented) {
      return;
    }

    let doc = aEvent.composedTarget.ownerDocument;
    if (!doc && false) {
      dump(
        `doc is unexpectedly null (bug 1478596), composedTarget=${aEvent.composedTarget}\n`
      );
      for (let k of ["target", "originalTarget", "explicitOriginalTarget"]) {
        dump(
          ` Alternative: ${k}=${aEvent[k]} and its doc=${aEvent[k]?.ownerDocument}\n`
        );
      }
    }
    let {
      mozDocumentURIIfNotForErrorPages: docLocation,
      characterSet: charSet,
      baseURI,
    } = doc;
    docLocation = docLocation && docLocation.spec;
    let disableSetDesktopBackground = null;

    let contentType = null;
    let contentDisposition = null;
    let composedTarget = aEvent.composedTarget;
    if (composedTarget.nodeType == composedTarget.ELEMENT_NODE) {
      let isImage =
        composedTarget instanceof Ci.nsIImageLoadingContent &&
        composedTarget.currentURI;
      if (
        isImage ||
        this.contentWindow.HTMLCanvasElement.isInstance(composedTarget)
      ) {
        disableSetDesktopBackground =
          this._disableSetDesktopBackground(composedTarget);
      }
      if (isImage) {
        try {
          let imageCache = Cc["@mozilla.org/image/tools;1"]
            .getService(Ci.imgITools)
            .getImgCacheForDocument(doc);
          let props = imageCache.findEntryProperties(
            aEvent.composedTarget.currentURI,
            doc
          );

          try {
            contentType = props.get("type", Ci.nsISupportsCString).data;
          } catch (e) {}

          try {
            contentDisposition = props.get(
              "content-disposition",
              Ci.nsISupportsCString
            ).data;
          } catch (e) {}
        } catch (e) {}
      }
    }

    let selectionInfo = lazy.SelectionUtils.getSelectionDetails(
      this.contentWindow
    );

    this._setContext(aEvent);
    let context = this.context;
    this.target = context.target;

    let editFlags = null;

    let referrerInfo = Cc["@mozilla.org/referrer-info;1"].createInstance(
      Ci.nsIReferrerInfo
    );
    referrerInfo.initWithElement(aEvent.composedTarget);
    referrerInfo = lazy.E10SUtils.serializeReferrerInfo(referrerInfo);

    let linkReferrerInfo = null;
    if (context.onLink) {
      linkReferrerInfo = Cc["@mozilla.org/referrer-info;1"].createInstance(
        Ci.nsIReferrerInfo
      );
      linkReferrerInfo.initWithElement(context.link);
    }

    let target = context.target;
    if (target) {
      this._cleanContext();
    }

    editFlags = EditFlags.isEditable(
      aEvent.composedTarget,
      this.contentWindow
    );

    this.docShell.docViewer
      .QueryInterface(Ci.nsIDocumentViewerEdit)
      .setCommandNode(aEvent.composedTarget);
    aEvent.composedTarget.documentGlobal.updateCommands("contentcontextmenu");

    let data = {
      context,
      charSet,
      baseURI,
      referrerInfo,
      editFlags,
      contentType,
      docLocation,
      selectionInfo,
      contentDisposition,
      disableSetDesktopBackground,
    };

    if (context.inFrame && !context.inSrcdocFrame) {
      data.frameReferrerInfo = lazy.E10SUtils.serializeReferrerInfo(
        doc.referrerInfo
      );
    }

    if (linkReferrerInfo) {
      data.linkReferrerInfo =
        lazy.E10SUtils.serializeReferrerInfo(linkReferrerInfo);
    }

    aEvent.stopPropagation();

    this.sendAsyncMessage("contextmenu", data);
  }

  _cleanContext() {
    const context = this.context;
    const cleanTarget = Object.create(null);

    cleanTarget.ownerDocument = {
      fullscreen: context.target.ownerDocument.fullscreen,

      contentType: context.target.ownerDocument.contentType,
    };

    Object.assign(cleanTarget, {
      ended: context.target.ended,
      muted: context.target.muted,
      paused: context.target.paused,
      controls: context.target.controls,
      duration: context.target.duration,
    });

    const onMedia = context.onVideo || context.onAudio;

    if (onMedia) {
      Object.assign(cleanTarget, {
        loop: context.target.loop,
        error: context.target.error,
        networkState: context.target.networkState,
        playbackRate: context.target.playbackRate,
        NETWORK_NO_SOURCE: context.target.NETWORK_NO_SOURCE,
      });

      if (context.onVideo) {
        Object.assign(cleanTarget, {
          readyState: context.target.readyState,
          HAVE_CURRENT_DATA: context.target.HAVE_CURRENT_DATA,
        });
      }
    }

    context.target = cleanTarget;

    if (context.link) {
      context.link = { href: context.linkURL };
    }

    delete context.linkURI;
  }

  _setContext(aEvent) {
    this.context = Object.create(null);
    const context = this.context;

    context.timeStamp = aEvent.timeStamp;
    context.screenXDevPx = aEvent.screenX * this.contentWindow.devicePixelRatio;
    context.screenYDevPx = aEvent.screenY * this.contentWindow.devicePixelRatio;
    context.inputSource = aEvent.inputSource;
    context.clientX = aEvent.clientX;
    context.clientY = aEvent.clientY;

    let node = aEvent.composedTarget;

    if (node.containingShadowRoot?.isUAWidget()) {
      const host = node.containingShadowRoot.host;
      if (
        this.contentWindow.HTMLMediaElement.isInstance(host) ||
        this.contentWindow.HTMLEmbedElement.isInstance(host) ||
        this.contentWindow.HTMLObjectElement.isInstance(host)
      ) {
        node = host;
      }
    }

    const XUL_NS =
      "http://www.mozilla.org/keymaster/gatekeeper/there.is.only.xul";

    context.shouldDisplay = true;

    if (
      node.nodeType == node.DOCUMENT_NODE ||
      (node.namespaceURI == XUL_NS && !this._isXULTextLinkLabel(node))
    ) {
      context.shouldDisplay = false;
      return;
    }

    const editFlags = EditFlags.isEditable(node, this.contentWindow);

    context.bgImageURL = "";
    context.imageDescURL = "";
    context.imageInfo = null;
    context.mediaURL = "";

    context.hasBGImage = false;
    context.hasMultipleBGImages = false;
    context.isDesignMode = false;
    context.inFrame = false;
    context.inSrcdocFrame = false;
    context.inSyntheticDoc = false;
    context.inTabBrowser = true;

    context.link = null;
    context.linkDownload = "";
    context.linkProtocol = "";
    context.linkTextStr = "";
    context.linkURL = "";
    context.linkURI = null;

    context.onAudio = false;
    context.onCanvas = false;
    context.onCompletedImage = false;
    context.onDRMMedia = false;
    context.onPiPVideo = false;
    context.onEditable = false;
    context.onImage = false;
    context.onLink = false;
    context.onLoadedImage = false;
    context.onMailtoLink = false;
    context.onTelLink = false;
    context.onNumeric = false;
    context.onSaveableLink = false;
    context.onTextInput = false;
    context.onVideo = false;

    const textDirectiveRanges =
      this.document.fragmentDirective?.getTextDirectiveRanges?.() || [];
    context.hasTextFragments = !!textDirectiveRanges.length;

    context.target = node;
    context.targetIdentifier = lazy.ContentDOMReference.get(node);

    context.policyContainer = lazy.E10SUtils.serializePolicyContainer(
      context.target.ownerDocument.policyContainer
    );

    context.inSyntheticDoc = context.target.ownerDocument.mozSyntheticDocument;

    this._setContextForNodesNoChildren(editFlags);
    this._setContextForNodesWithChildren(editFlags);

  }

  _setContextForNodesNoChildren(editFlags) {
    const context = this.context;

    if (context.target.nodeType == context.target.TEXT_NODE) {
      return;
    }

    if (context.target.nodeType != context.target.ELEMENT_NODE) {
      return;
    }

    let videoElement;

    if (
      context.target instanceof Ci.nsIImageLoadingContent &&
      (context.target.currentRequestFinalURI || context.target.currentURI)
    ) {
      context.onImage = true;

      context.imageInfo = {
        currentSrc: context.target.currentSrc,
        width: context.target.width,
        height: context.target.height,
        imageText: this.contentWindow.ImageDocument.isInstance(
          context.target.ownerDocument
        )
          ? undefined
          : context.target.title || context.target.alt,
      };
      if (SVGAnimatedLength.isInstance(context.imageInfo.height)) {
        context.imageInfo.height = context.imageInfo.height.animVal.value;
      }
      if (SVGAnimatedLength.isInstance(context.imageInfo.width)) {
        context.imageInfo.width = context.imageInfo.width.animVal.value;
      }

      const request = context.target.getRequest(
        Ci.nsIImageLoadingContent.CURRENT_REQUEST
      );

      if (request && request.imageStatus & request.STATUS_SIZE_AVAILABLE) {
        context.onLoadedImage = true;
      }

      if (
        request &&
        request.imageStatus & request.STATUS_LOAD_COMPLETE &&
        !(request.imageStatus & request.STATUS_ERROR)
      ) {
        context.onCompletedImage = true;
      }

      context.originalMediaURL = (() => {
        let currentURI = context.target.currentURI?.spec;
        if (currentURI && this._isMediaURLReusable(currentURI)) {
          return currentURI;
        }
        return "";
      })();

      context.mediaURL = (() => {
        let finalURI = context.target.currentRequestFinalURI?.spec;
        if (finalURI && this._isMediaURLReusable(finalURI)) {
          return finalURI;
        }
        let currentURI = context.target.currentURI?.spec;
        if (currentURI && this._isMediaURLReusable(currentURI)) {
          return currentURI;
        }
        return "";
      })();

      const descURL = context.target.getAttribute("longdesc");

      if (descURL) {
        context.imageDescURL = new URL(
          descURL,
          context.target.ownerDocument.body.baseURI
        ).href;
      }
    } else if (
      this.contentWindow.HTMLCanvasElement.isInstance(context.target)
    ) {
      context.onCanvas = true;
    } else if (
      (videoElement = this.contentWindow.HTMLVideoElement.isInstance(
        context.target
      )
        ? context.target
        : this._maybeGetVideoElementAtPoint(context.clientX, context.clientY))
    ) {
      context.target = videoElement;
      context.targetIdentifier = lazy.ContentDOMReference.get(videoElement);
      const mediaURL = context.target.currentSrc || context.target.src;

      if (this._isMediaURLReusable(mediaURL)) {
        context.mediaURL = mediaURL;
      }

      if (this._isProprietaryDRM()) {
        context.onDRMMedia = true;
      }

      if (context.target.isCloningElementVisually) {
        context.onPiPVideo = true;
      }

      if (
        context.target.readyState >= context.target.HAVE_METADATA &&
        (context.target.videoWidth == 0 || context.target.videoHeight == 0)
      ) {
        context.onAudio = true;
      } else {
        context.onVideo = true;
      }
    } else if (this.contentWindow.HTMLAudioElement.isInstance(context.target)) {
      context.onAudio = true;
      const mediaURL = context.target.currentSrc || context.target.src;

      if (this._isMediaURLReusable(mediaURL)) {
        context.mediaURL = mediaURL;
      }

      if (this._isProprietaryDRM()) {
        context.onDRMMedia = true;
      }
    } else if (
      editFlags &
      (EditFlags.INPUT | EditFlags.TEXTAREA)
    ) {
      context.onTextInput = (editFlags & EditFlags.TEXTINPUT) !== 0;
      context.onNumeric = (editFlags & EditFlags.NUMERIC) !== 0;
      context.onEditable = (editFlags & EditFlags.EDITABLE) !== 0;
      context.isDesignMode =
        (editFlags & EditFlags.CONTENTEDITABLE) !== 0;
      context.onSearchField = editFlags & EditFlags.SEARCHENGINE;
    } else if (this.contentWindow.HTMLHtmlElement.isInstance(context.target)) {
      const bodyElt = context.target.ownerDocument.body;

      if (bodyElt) {
        let computedURL;

        try {
          computedURL = this._getComputedURL(bodyElt, "background-image");
          context.hasMultipleBGImages = false;
        } catch (e) {
          context.hasMultipleBGImages = true;
        }

        if (computedURL) {
          context.hasBGImage = true;
          context.bgImageURL = new URL(computedURL, bodyElt.baseURI).href;
        }
      }
    }

  }

  _setContextForNodesWithChildren(editFlags) {
    const context = this.context;

    let elem = context.target;

    while (elem) {
      if (elem.nodeType == elem.ELEMENT_NODE) {
        const XLINK_NS = "http://www.w3.org/1999/xlink";

        if (
          !context.onLink &&
          ((this.contentWindow.HTMLAnchorElement.isInstance(elem) &&
            elem.href) ||
            (this.contentWindow.HTMLAreaElement.isInstance(elem) &&
              elem.href) ||
            this.contentWindow.HTMLLinkElement.isInstance(elem) ||
            (this.contentWindow.SVGAElement.isInstance(elem) &&
              (elem.href || elem.hasAttributeNS(XLINK_NS, "href"))) ||
            (this.contentWindow.MathMLElement.isInstance(elem) &&
              (elem.localName == "a" ||
                !Services.prefs.getBoolPref(
                  "mathml.href_link_on_non_anchor_element.disabled"
                )) &&
              elem.hasAttribute("href")) ||
            elem.getAttributeNS(XLINK_NS, "type") == "simple" ||
            this._isXULTextLinkLabel(elem))
        ) {
          context.onLink = true;

          context.link = elem;
          context.linkURL = this._getLinkURL();
          context.linkURI = this._getLinkURI();
          context.linkTextStr = this._getLinkText();
          context.linkProtocol = this._getLinkProtocol();
          context.onMailtoLink = context.linkProtocol == "mailto";
          context.onTelLink = context.linkProtocol == "tel";
          context.onSaveableLink = this._isLinkSaveable(context.link);

          context.isSponsoredLink =
            (elem.ownerDocument.URL === "about:newtab" ||
              elem.ownerDocument.URL === "about:home") &&
            elem.dataset.isSponsoredLink === "true";

          try {
            if (elem.download) {
              context.target.ownerDocument.nodePrincipal.checkMayLoad(
                context.linkURI,
                true
              );
              context.linkDownload = elem.download;
            }
          } catch (ex) {}
        }

        if (!context.hasBGImage && !context.hasMultipleBGImages) {
          let bgImgUrl = null;

          try {
            bgImgUrl = this._getComputedURL(elem, "background-image");
            context.hasMultipleBGImages = false;
          } catch (e) {
            context.hasMultipleBGImages = true;
          }

          if (bgImgUrl) {
            context.hasBGImage = true;
            context.bgImageURL = new URL(bgImgUrl, elem.baseURI).href;
          }
        }
      }

      elem = elem.flattenedTreeParentNode;
    }

    const docDefaultView = context.target.documentGlobal;

    if (docDefaultView != docDefaultView.top) {
      context.inFrame = true;

      if (context.target.ownerDocument.isSrcdocDocument) {
        context.inSrcdocFrame = true;
      }
    }

    if (!context.onEditable) {
      if (editFlags & EditFlags.CONTENTEDITABLE) {
        context.onTextInput = true;
        context.onImage = false;
        context.onLoadedImage = false;
        context.onCompletedImage = false;
        context.inFrame = false;
        context.inSrcdocFrame = false;
        context.hasBGImage = false;
        context.isDesignMode = true;
        context.onEditable = true;
      }
    }
  }

  _destructionObservers = new Set();
  registerDestructionObserver(obj) {
    this._destructionObservers.add(obj);
  }

  unregisterDestructionObserver(obj) {
    this._destructionObservers.delete(obj);
  }

  didDestroy() {
    for (let obs of this._destructionObservers) {
      obs.actorDestroyed(this);
    }
    this._destructionObservers = null;
  }
}
