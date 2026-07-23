/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  ContextualIdentityService:
    "moz-src:///toolkit/components/contextualidentity/ContextualIdentityService.sys.mjs",
  E10SUtils: "resource://gre/modules/E10SUtils.sys.mjs",
  NetUtil: "resource://gre/modules/NetUtil.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  SearchUIUtils: "moz-src:///browser/components/search/SearchUIUtils.sys.mjs",
  SearchUtils: "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  ShortcutUtils: "resource://gre/modules/ShortcutUtils.sys.mjs",
});

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

ChromeUtils.defineLazyGetter(lazy, "ReferrerInfo", () =>
  Components.Constructor(
    "@mozilla.org/referrer-info;1",
    "nsIReferrerInfo",
    "init"
  )
);


XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "STRIP_ON_SHARE_ENABLED",
  "privacy.query_stripping.strip_on_share.enabled",
  false
);


XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "QueryStringStripper",
  "@mozilla.org/url-query-string-stripper;1",
  Ci.nsIURLQueryStringStripper
);

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "clipboard",
  "@mozilla.org/widget/clipboardhelper;1",
  Ci.nsIClipboardHelper
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "TEXT_FRAGMENTS_ENABLED",
  "dom.text_fragments.enabled",
  false
);


const ALLOWED_CHROME_IMAGE_URLS = new Set([
  "chrome://global/skin/illustrations/security-error.svg",
  "chrome://global/skin/illustrations/no-connection.svg",
]);

const IMAGE_ONLY_PROTOCOLS = [
  "cached-favicon:",
  "moz-icon:",
  "moz-newtab-wallpaper:",
  "moz-page-thumb:",
  "moz-remote-image:",
  "page-icon:",
];

export class nsContextMenu {
  constructor(aXulMenu, _aIsShift) {
    this.window = aXulMenu.documentGlobal;
    this.document = aXulMenu.ownerDocument;

    this.setContext();

    if (!this.shouldDisplay) {
      return;
    }

    this.isContentSelected = !this.selectionInfo.docSelectionIsCollapsed;

    this.viewFrameSourceElement = this.document.getElementById(
      "context-viewframesource"
    );

    this.isContentSelected = !this.selectionInfo.docSelectionIsCollapsed;
    this.onPlainTextLink = false;

    this.initItems(aXulMenu);
  }

  setContext() {
    let context = Object.create(null);

    if (nsContextMenu.contentData) {
      this.contentData = nsContextMenu.contentData;
      context = this.contentData.context;
      nsContextMenu.contentData = null;
    }

    this.shouldDisplay = context.shouldDisplay;
    this.timeStamp = context.timeStamp;

    this.imageDescURL = context.imageDescURL;
    this.imageInfo = context.imageInfo;
    this.mediaURL = context.mediaURL || context.bgImageURL;
    this.originalMediaURL = context.originalMediaURL || this.mediaURL;

    this.hasBGImage = context.hasBGImage;
    this.hasMultipleBGImages = context.hasMultipleBGImages;
    this.isDesignMode = context.isDesignMode;
    this.inFrame = context.inFrame;
    this.inSrcdocFrame = context.inSrcdocFrame;
    this.inSyntheticDoc = context.inSyntheticDoc;
    this.inTabBrowser = context.inTabBrowser;

    this.link = context.link;
    this.linkDownload = context.linkDownload;
    this.linkProtocol = context.linkProtocol;
    this.linkTextStr = context.linkTextStr;
    this.linkURL = context.linkURL;
    this.linkURI = this.getLinkURI(); 

    this.onAudio = context.onAudio;
    this.onCanvas = context.onCanvas;
    this.onCompletedImage = context.onCompletedImage;
    this.onEditable = context.onEditable;
    this.onImage = context.onImage;
    this.onSearchField = context.onSearchField;
    this.onLink = context.onLink;
    this.onLoadedImage = context.onLoadedImage;
    this.onMailtoLink = context.onMailtoLink;
    this.onTelLink = context.onTelLink;
    this.onNumeric = context.onNumeric;
    this.onSaveableLink = context.onSaveableLink;
    this.onTextInput = context.onTextInput;
    this.onVideo = context.onVideo;

    this.target = context.target;
    this.targetIdentifier = context.targetIdentifier;

    this.principal = context.principal;
    this.storagePrincipal = context.storagePrincipal;
    this.frameID = context.frameID;
    this.frameOuterWindowID = context.frameOuterWindowID;
    this.frameBrowsingContext = BrowsingContext.get(
      context.frameBrowsingContextID
    );

    this.inSyntheticDoc = context.inSyntheticDoc;

    this.isSponsoredLink = context.isSponsoredLink;

    if (this.target) {
      this.ownerDoc = this.target.ownerDocument;
    }

    this.policyContainer = lazy.E10SUtils.deserializePolicyContainer(
      context.policyContainer
    );

    if (this.contentData) {
      this.browser = this.contentData.browser;
      this.selectionInfo = this.contentData.selectionInfo;
      this.actor = this.contentData.actor;
    } else {
      const { SelectionUtils } = ChromeUtils.importESModule(
        "resource://gre/modules/SelectionUtils.sys.mjs"
      );

      this.browser = this.ownerDoc.defaultView.docShell.chromeEventHandler;
      this.selectionInfo = SelectionUtils.getSelectionDetails(
        this.browser.documentGlobal
      );
      this.actor =
        this.browser.browsingContext.currentWindowGlobal.getActor(
          "ContextMenu"
        );
    }

    this.remoteType = this.actor.manager.domProcess.remoteType;

    this.selectedText = this.selectionInfo.text;
    this.isTextSelected = !!this.selectedText.length;
    this.inTabBrowser =
      gBrowser && gBrowser.getTabForBrowser
        ? !!gBrowser.getTabForBrowser(this.browser)
        : false;

    this.hasTextFragments = context.hasTextFragments;
    this.textFragmentURL = null;

  } 

  hiding(aXulMenu) {
    if (this.actor) {
      this.actor.hiding();
    }

    aXulMenu.showHideSeparators = null;

    this.contentData = null;
    if (this._onPopupHiding) {
      this._onPopupHiding();
    }
  }

  initItems(aXulMenu) {
    this.initOpenItems();
    this.initNavigationItems();
    this.initViewItems();
    this.initImageItems();
    this.initMiscItems();
    this.initSaveItems();
    this.initClipboardItems();
    this.initMediaPlayerItems();
    this.initLeaveDOMFullScreenItems();
    this.initViewSourceItems();
    this.initTextFragmentItems();

    this.showHideSeparators(aXulMenu);
    if (!aXulMenu.showHideSeparators) {
      aXulMenu.showHideSeparators = () => {
        this.showHideSeparators(aXulMenu);
      };
    }
  }

  initTextFragmentItems() {
    const shouldShow =
      lazy.TEXT_FRAGMENTS_ENABLED &&
      !(
        this.inFrame ||
        this.onEditable ||
        this.browser.currentURI.schemeIs("view-source")
      ) &&
      (this.hasTextFragments || this.isContentSelected);
    this.showItem("context-copy-link-to-highlight", shouldShow);
    this.showItem(
      "context-copy-clean-link-to-highlight",
      shouldShow && lazy.STRIP_ON_SHARE_ENABLED
    );

    this.setItemAttr("context-copy-link-to-highlight", "disabled", true);
    this.setItemAttr("context-copy-clean-link-to-highlight", "disabled", true);

    this.showItem("context-sep-highlights", this.hasTextFragments);
    this.showItem("context-remove-highlight", this.hasTextFragments);
  }

  async getTextDirective() {
    if (!lazy.TEXT_FRAGMENTS_ENABLED) {
      return;
    }
    this.textFragmentURL = await this.actor.getTextDirective();

    if (this.textFragmentURL) {
      this.setItemAttr("context-copy-link-to-highlight", "disabled", null);
      let link = this.getLinkURI(this.textFragmentURL);
      let disabledAttr = this.canStripParams(link) ? null : true;
      this.setItemAttr(
        "context-copy-clean-link-to-highlight",
        "disabled",
        disabledAttr
      );
    }
  }

  async removeAllTextFragments() {
    await this.actor.removeAllTextFragments();
  }

  copyLinkToHighlight(stripSiteTracking = false) {
    if (this.textFragmentURL) {
      if (stripSiteTracking) {
        const uri = this.getLinkURI(this.textFragmentURL);
        this.copyStrippedLink(uri);
      } else {
        this.copyLink(this.textFragmentURL);
      }
    }
  }

  initOpenItems() {
    var isMailtoInternal = false;
    if (this.onMailtoLink) {
      var mailtoHandler = Cc[
        "@mozilla.org/uriloader/external-protocol-service;1"
      ]
        .getService(Ci.nsIExternalProtocolService)
        .getProtocolHandlerInfo("mailto");
      isMailtoInternal =
        !mailtoHandler.alwaysAskBeforeHandling &&
        mailtoHandler.preferredAction == Ci.nsIHandlerInfo.useHelperApp &&
        mailtoHandler.preferredApplicationHandler instanceof
          Ci.nsIWebHandlerApp;
    }

    if (
      this.isTextSelected &&
      !this.onLink &&
      this.selectionInfo &&
      this.selectionInfo.linkURL
    ) {
      this.linkURL = this.selectionInfo.linkURL;
      this.linkURI = this.getLinkURI();

      this.linkTextStr = this.selectionInfo.linkText;
      this.onPlainTextLink = true;
    }

    let { window, document } = this;
    var inContainer = false;
    if (this.contentData.userContextId) {
      inContainer = true;
      var item = document.getElementById("context-openlinkincontainertab");

      item.setAttribute("data-usercontextid", this.contentData.userContextId);

      var label = lazy.ContextualIdentityService.getUserContextLabel(
        this.contentData.userContextId
      );

      document.l10n.setAttributes(
        item,
        "main-context-menu-open-link-in-container-tab",
        {
          containerName: label,
        }
      );
    }

    var shouldShow =
      this.onSaveableLink || isMailtoInternal || this.onPlainTextLink;
    var isWindowPrivate = lazy.PrivateBrowsingUtils.isWindowPrivate(window);
    let showContainers =
      Services.prefs.getBoolPref("privacy.userContext.enabled") &&
      lazy.ContextualIdentityService.getPublicIdentities().length;
    let showSplitViews = Services.prefs.getBoolPref(
      "browser.tabs.splitView.enabled"
    );
    let currentTabInSplitView = !!window.gBrowser?.selectedTab?.splitview;
    this.showItem("context-openlink", shouldShow && !isWindowPrivate);
    this.showItem(
      "context-openlinkprivate",
      shouldShow && lazy.PrivateBrowsingUtils.enabled
    );
    this.showItem("context-openlinkintab", shouldShow && !inContainer);
    this.showItem("context-openlinkincontainertab", shouldShow && inContainer);
    this.showItem(
      "context-openlinkinusercontext-menu",
      shouldShow && !isWindowPrivate && showContainers
    );
    this.showItem("context-openlinkincurrent", this.onPlainTextLink);
    let isHiddenTab = !!window.gBrowser?.getTabForBrowser(this.browser)?.hidden;
    let isPinnedTab = !!window.gBrowser?.getTabForBrowser(this.browser)?.pinned;
    this.showItem(
      "context-openlinkinsplitview",
      shouldShow &&
        showSplitViews &&
        !currentTabInSplitView &&
        !isHiddenTab &&
        !isPinnedTab
    );
  }

  initNavigationItems() {
    var shouldShow =
      !(
        this.isContentSelected ||
        this.onLink ||
        this.onImage ||
        this.onCanvas ||
        this.onVideo ||
        this.onAudio ||
        this.onTextInput
      ) && this.inTabBrowser;
    if (AppConstants.platform == "macosx") {
      for (let id of [
        "context-back",
        "context-forward",
        "context-reload",
        "context-stop",
        "context-sep-navigation",
      ]) {
        this.showItem(id, shouldShow);
      }
    } else {
      this.showItem("context-navigation", shouldShow);
    }

    let stopped =
      this.window.XULBrowserWindow.stopCommand.getAttribute("disabled") ==
      "true";

    let stopReloadItem = "";
    if (shouldShow) {
      stopReloadItem = stopped ? "reload" : "stop";
    }

    this.showItem("context-reload", stopReloadItem == "reload");
    this.showItem("context-stop", stopReloadItem == "stop");

    let { document } = this;
    let initBackForwardMenuItemTooltip = (menuItemId, l10nId, shortcutId) => {
      if (AppConstants.platform == "macosx") {
        return;
      }

      let shortcut = document.getElementById(shortcutId);
      if (shortcut) {
        shortcut = lazy.ShortcutUtils.prettifyShortcut(shortcut);
      } else {
        shortcut = "";
      }

      let menuItem = document.getElementById(menuItemId);
      document.l10n.setAttributes(menuItem, l10nId, { shortcut });
    };

    initBackForwardMenuItemTooltip(
      "context-back",
      "main-context-menu-back-2",
      "goBackKb"
    );

    initBackForwardMenuItemTooltip(
      "context-forward",
      "main-context-menu-forward-2",
      "goForwardKb"
    );
  }

  initLeaveDOMFullScreenItems() {
    var shouldShow = this.target.ownerDocument.fullscreen;
    this.showItem("context-leave-dom-fullscreen", shouldShow);
  }

  initSaveItems() {
    var shouldShow = !(
      this.onTextInput ||
      this.onLink ||
      this.isContentSelected ||
      this.onImage ||
      this.onCanvas ||
      this.onVideo ||
      this.onAudio
    );
    this.showItem("context-savepage", shouldShow);

    this.showItem(
      "context-savelink",
      this.onSaveableLink || this.onPlainTextLink
    );

    this.showItem("context-savevideo", this.onVideo);
    this.showItem("context-saveaudio", this.onAudio);
    this.showItem("context-video-saveimage", this.onVideo);
    this.setItemAttr("context-savevideo", "disabled", !this.mediaURL);
    this.setItemAttr("context-saveaudio", "disabled", !this.mediaURL);
    this.showItem("context-sendvideo", this.onVideo);
    this.showItem("context-sendaudio", this.onAudio);
    let mediaIsBlob = this.mediaURL.startsWith("blob:");
    this.setItemAttr(
      "context-sendvideo",
      "disabled",
      !this.mediaURL || mediaIsBlob
    );
    this.setItemAttr(
      "context-sendaudio",
      "disabled",
      !this.mediaURL || mediaIsBlob
    );

  }

  initImageItems() {
    this.showItem(
      "context-reloadimage",
      this.onImage && !this.onCompletedImage
    );

    const mediaURL = URL.parse(this.mediaURL);
    const isImageOnlyProtocol =
      mediaURL && IMAGE_ONLY_PROTOCOLS.includes(mediaURL.protocol);

    let showViewImage =
      (this.onImage && (!this.inSyntheticDoc || this.inFrame)) ||
      this.onCanvas;
    let showBGImage =
      this.hasBGImage &&
      !this.hasMultipleBGImages &&
      !this.inSyntheticDoc &&
      !this.isContentSelected &&
      !this.onImage &&
      !this.onCanvas &&
      !this.onVideo &&
      !this.onAudio &&
      !this.onLink &&
      !this.onTextInput;
    this.showItem(
      "context-viewimage",
      (showViewImage || showBGImage) && !isImageOnlyProtocol
    );

    this.showItem(
      "context-saveimage",
      (this.onLoadedImage && !isImageOnlyProtocol) || this.onCanvas
    );


    this.showItem("context-copyimage-contents", this.onImage || this.onCanvas);

    this.showItem("context-copyimage", this.onImage || showBGImage);

    this.showItem("context-sendimage", this.onImage || showBGImage);

    var showViewImageInfo =
      this.onImage &&
      Services.prefs.getBoolPref("browser.menu.showViewImageInfo", false);

    this.showItem("context-viewimageinfo", showViewImageInfo);
    this.showItem(
      "context-viewimagedesc",
      this.onImage && this.imageDescURL !== ""
    );


  }

  initViewItems() {
    this.showItem(
      "context-viewpartialsource-selection",
      this.isContentSelected &&
        this.selectionInfo.isDocumentLevelSelection
    );


    var showViewSource = !(
      this.isContentSelected ||
      this.onImage ||
      this.onCanvas ||
      this.onVideo ||
      this.onAudio ||
      this.onLink ||
      this.onTextInput
    );

    this.showItem("context-viewsource", showViewSource);


    this.showItem(
      "context-viewvideo",
      this.onVideo && (!this.inSyntheticDoc || this.inFrame)
    );
    this.setItemAttr("context-viewvideo", "disabled", !this.mediaURL);
  }

  initMiscItems() {
    let { window, document } = this;
    let bookmarkPage = document.getElementById("context-bookmarkpage");
    this.showItem(
      bookmarkPage,
      !(
        this.isContentSelected ||
        this.onTextInput ||
        this.onLink ||
        this.onImage ||
        this.onVideo ||
        this.onAudio ||
        this.onCanvas
      )
    );

    this.showItem(
      "context-bookmarklink",
      (this.onLink &&
        !this.onMailtoLink &&
        !this.onTelLink) ||
        this.onPlainTextLink
    );
    this.showItem("context-add-engine", this.shouldShowAddEngine());
    this.showItem("frame", this.inFrame);

    if (this.inFrame) {
      let frameOsPid =
        this.actor.manager.browsingContext.currentWindowGlobal.osPid;
      this.setItemAttr("context-frameOsPid", "label", "PID: " + frameOsPid);
    }

    this.showAndFormatSearchContextItem();

    this.showItem("context-showonlythisframe", !this.inSrcdocFrame);
    this.showItem("context-openframeintab", !this.inSrcdocFrame);
    this.showItem("context-openframe", !this.inSrcdocFrame);
    this.showItem("context-bookmarkframe", !this.inSrcdocFrame);

    if (this.inFrame) {
      this.viewFrameSourceElement.hidden =
        !lazy.BrowserUtils.mimeTypeIsTextBased(
          this.target.ownerDocument.contentType
        );
    }

    this.showItem(
      "context-bidi-text-direction-toggle",
      this.onTextInput && !this.onNumeric && window.top.gBidiUI
    );
    this.showItem(
      "context-bidi-page-direction-toggle",
      !this.onTextInput && window.top.gBidiUI
    );
  }

  initClipboardItems() {
    this.window.goUpdateGlobalEditMenuItems();

    this.showItem("context-undo", this.onTextInput);
    this.showItem("context-redo", this.onTextInput);
    this.showItem("context-cut", this.onTextInput);
    this.showItem("context-copy", this.isContentSelected || this.onTextInput);
    this.showItem("context-paste", this.onTextInput);
    this.showItem("context-paste-no-formatting", this.isDesignMode);
    this.showItem("context-delete", this.onTextInput);
    this.showItem(
      "context-selectall",
      !(
        this.onLink ||
        this.onImage ||
        this.onVideo ||
        this.onAudio ||
        this.inSyntheticDoc
      ) || this.isDesignMode
    );


    this.showItem("context-copyemail", this.onMailtoLink);

    this.showItem("context-copyphone", this.onTelLink);

    this.showItem(
      "context-copylink",
      this.onLink && !this.onMailtoLink && !this.onTelLink
    );

    this.showItem(
      "context-stripOnShareLink",
      lazy.STRIP_ON_SHARE_ENABLED &&
        (this.onLink || this.onPlainTextLink) &&
        !this.onMailtoLink &&
        !this.onTelLink &&
        !this.isSecureAboutPage()
    );

    let disabledAttr = this.canStripParams() ? null : true;
    this.setItemAttr("context-stripOnShareLink", "disabled", disabledAttr);


    this.showItem("context-copyvideourl", this.onVideo);
    this.showItem("context-copyaudiourl", this.onAudio);
    this.setItemAttr("context-copyvideourl", "disabled", !this.mediaURL);
    this.setItemAttr("context-copyaudiourl", "disabled", !this.mediaURL);
  }

  initMediaPlayerItems() {
    var onMedia = this.onVideo || this.onAudio;
    this.showItem(
      "context-media-play",
      onMedia && (this.target.paused || this.target.ended)
    );
    this.showItem(
      "context-media-pause",
      onMedia && !this.target.paused && !this.target.ended
    );
    this.showItem("context-media-mute", onMedia && !this.target.muted);
    this.showItem("context-media-unmute", onMedia && this.target.muted);
    this.showItem(
      "context-media-playbackrate",
      onMedia && this.target.duration != Number.POSITIVE_INFINITY
    );
    this.showItem("context-media-loop", onMedia);
    this.showItem(
      "context-media-showcontrols",
      onMedia && !this.target.controls
    );
    this.showItem(
      "context-media-hidecontrols",
      this.target.controls &&
        (this.onVideo || (this.onAudio && !this.inSyntheticDoc))
    );
    this.showItem(
      "context-video-fullscreen",
      this.onVideo && !this.target.ownerDocument.fullscreen
    );

    if (onMedia) {
      this.setItemAttr(
        "context-media-playbackrate-050x",
        "checked",
        this.target.playbackRate == 0.5
      );
      this.setItemAttr(
        "context-media-playbackrate-100x",
        "checked",
        this.target.playbackRate == 1.0
      );
      this.setItemAttr(
        "context-media-playbackrate-125x",
        "checked",
        this.target.playbackRate == 1.25
      );
      this.setItemAttr(
        "context-media-playbackrate-150x",
        "checked",
        this.target.playbackRate == 1.5
      );
      this.setItemAttr(
        "context-media-playbackrate-200x",
        "checked",
        this.target.playbackRate == 2.0
      );
      this.setItemAttr("context-media-loop", "checked", this.target.loop);
      var hasError =
        this.target.error != null ||
        this.target.networkState == this.target.NETWORK_NO_SOURCE;
      this.setItemAttr("context-media-play", "disabled", hasError);
      this.setItemAttr("context-media-pause", "disabled", hasError);
      this.setItemAttr("context-media-mute", "disabled", hasError);
      this.setItemAttr("context-media-unmute", "disabled", hasError);
      this.setItemAttr("context-media-playbackrate", "disabled", hasError);
      this.setItemAttr("context-media-playbackrate-050x", "disabled", hasError);
      this.setItemAttr("context-media-playbackrate-100x", "disabled", hasError);
      this.setItemAttr("context-media-playbackrate-125x", "disabled", hasError);
      this.setItemAttr("context-media-playbackrate-150x", "disabled", hasError);
      this.setItemAttr("context-media-playbackrate-200x", "disabled", hasError);
      this.setItemAttr("context-media-showcontrols", "disabled", hasError);
      this.setItemAttr("context-media-hidecontrols", "disabled", hasError);
      if (this.onVideo) {
        let canSaveSnapshot =
          this.target.readyState >= this.target.HAVE_CURRENT_DATA;
        this.setItemAttr(
          "context-video-saveimage",
          "disabled",
          !canSaveSnapshot
        );
        this.setItemAttr("context-video-fullscreen", "disabled", hasError);
      }
    }
  }

  initViewSourceItems() {
    const getString = aName => {
      const { bundle } = this.window.gViewSourceUtils.getPageActor(
        this.browser
      );
      return bundle.GetStringFromName(aName);
    };
    const showViewSourceItem = (id, check, accesskey) => {
      const fullId = `context-viewsource-${id}`;
      this.showItem(fullId, onViewSource);
      if (!onViewSource) {
        return;
      }
      this.setItemAttr(fullId, "checked", check());
      this.setItemAttr(fullId, "label", getString(`context_${id}_label`));
      if (accesskey) {
        this.setItemAttr(
          fullId,
          "accesskey",
          getString(`context_${id}_accesskey`)
        );
      }
    };

    const onViewSource =
      !!this.browser.browsingContext.currentWindowGlobal?.documentURI?.schemeIs(
        "view-source"
      );

    showViewSourceItem("goToLine", () => false, true);
    showViewSourceItem("wrapLongLines", () =>
      Services.prefs.getBoolPref("view_source.wrap_long_lines", false)
    );
    showViewSourceItem("highlightSyntax", () =>
      Services.prefs.getBoolPref("view_source.syntax_highlight", false)
    );
  }

  showHideSeparators(aPopup) {
    let lastVisibleSeparator = null;
    let count = 0;
    for (let menuItem of aPopup.children) {
      if (menuItem.hasAttribute("generateditemid")) {
        count++;
        continue;
      }

      if (menuItem.localName == "menuseparator") {
        if (!count || menuItem.hasAttribute("ensureHidden")) {
          menuItem.hidden = true;
        } else {
          menuItem.hidden = false;
          lastVisibleSeparator = menuItem;
        }

        count = 0;
      } else if (!menuItem.hidden) {
        if (menuItem.localName == "menu" && menuItem.menupopup) {
          this.showHideSeparators(menuItem.menupopup);
        } else if (menuItem.localName == "menugroup") {
          this.showHideSeparators(menuItem);
        }
        count++;
      }
    }

    if (!count && lastVisibleSeparator) {
      lastVisibleSeparator.hidden = true;
    }
  }

  _openLinkInParameters(extra) {
    let params = {
      charset: this.contentData.charSet,
      originPrincipal: this.principal,
      originStoragePrincipal: this.storagePrincipal,
      triggeringPrincipal: this.principal,
      triggeringRemoteType: this.remoteType,
      policyContainer: this.policyContainer,
      frameID: this.contentData.frameID,
      hasValidUserGestureActivation: true,
      textDirectiveUserActivation: true,
    };
    for (let p in extra) {
      params[p] = extra[p];
    }

    let referrerInfo = this.onLink
      ? this.contentData.linkReferrerInfo
      : this.contentData.referrerInfo;
    if (
      ("userContextId" in params &&
        params.userContextId != this.contentData.userContextId) ||
      this.onPlainTextLink
    ) {
      referrerInfo = new lazy.ReferrerInfo(
        referrerInfo.referrerPolicy,
        false,
        referrerInfo.originalReferrer
      );
    }

    params.referrerInfo = referrerInfo;
    return params;
  }

  _getGlobalHistoryOptions() {
    if (this.isSponsoredLink) {
      return {
        globalHistoryOptions: {
          triggeringSponsoredURL: this.linkURL,
          triggeringSource: "newtab",
        },
      };
    } else if (this.browser.hasAttribute("triggeringSponsoredURL")) {
      return {
        globalHistoryOptions: {
          triggeringSponsoredURL: this.browser.getAttribute(
            "triggeringSponsoredURL"
          ),
          triggeringSponsoredURLVisitTimeMS: this.browser.getAttribute(
            "triggeringSponsoredURLVisitTimeMS"
          ),
          triggeringSource: this.browser.getAttribute("triggeringSource"),
        },
      };
    }
    return {};
  }

  openLink() {
    const params = this._getGlobalHistoryOptions();

    this.window.openLinkIn(
      this.linkURL,
      "window",
      this._openLinkInParameters(params)
    );
  }

  openLinkInPrivateWindow() {
    this.window.openLinkIn(
      this.linkURL,
      "window",
      this._openLinkInParameters({ private: true })
    );
  }

  openLinkInTab(event) {
    let params = {
      userContextId: parseInt(event.target.getAttribute("data-usercontextid")),
      ...this._getGlobalHistoryOptions(),
    };

    this.window.openLinkIn(
      this.linkURL,
      "tab",
      this._openLinkInParameters(params)
    );
  }

  openLinkInSplitView() {
    let win = this.window;
    let currentTab = win.gBrowser.getTabForBrowser(this.browser);
    let userContextId = currentTab ? currentTab.userContextId : 0;
    let params = {
      userContextId,
      ...this._getGlobalHistoryOptions(),
      inBackground: false,
      resolveOnNewTabCreated: browser => {
        let linkTab = win.gBrowser.getTabForBrowser(browser);
        if (linkTab && currentTab) {
          win.gBrowser.addTabSplitView([currentTab, linkTab], {
            insertBefore: currentTab,
          });
          win.gBrowser.selectedTab = linkTab;
        }
      },
    };

    win.openLinkIn(this.linkURL, "tab", this._openLinkInParameters(params));
  }

  openLinkInCurrent() {
    this.window.openLinkIn(
      this.linkURL,
      "current",
      this._openLinkInParameters()
    );
  }

  openFrameInTab() {
    this.window.openLinkIn(this.contentData.docLocation, "tab", {
      charset: this.contentData.charSet,
      triggeringPrincipal: this.browser.contentPrincipal,
      policyContainer: this.browser.policyContainer,
      referrerInfo: this.contentData.frameReferrerInfo,
    });
  }

  reloadFrame(aEvent) {
    let forceReload = aEvent.shiftKey;
    this.actor.reloadFrame(this.targetIdentifier, forceReload);
  }

  openFrame() {
    this.window.openLinkIn(this.contentData.docLocation, "window", {
      charset: this.contentData.charSet,
      triggeringPrincipal: this.browser.contentPrincipal,
      policyContainer: this.browser.policyContainer,
      referrerInfo: this.contentData.frameReferrerInfo,
    });
  }

  showOnlyThisFrame() {
    this.window.urlSecurityCheck(
      this.contentData.docLocation,
      this.browser.contentPrincipal,
      Ci.nsIScriptSecurityManager.DISALLOW_SCRIPT
    );
    this.window.openWebLinkIn(this.contentData.docLocation, "current", {
      referrerInfo: this.contentData.frameReferrerInfo,
      triggeringPrincipal: this.browser.contentPrincipal,
    });
  }

  viewPartialSource() {
    let { browser } = this;
    let openSelectionFn = async () => {
      let tabBrowser = this.window.gBrowser;
      let relatedToCurrent = tabBrowser?.selectedBrowser === browser;
      const inNewWindow = !Services.prefs.getBoolPref("view_source.tab");
      if (!tabBrowser || !tabBrowser.addTab || !this.window.toolbar.visible) {
        let browserWindow =
          lazy.BrowserWindowTracker.getTopWindow() ??
          (await lazy.BrowserWindowTracker.promiseOpenWindow());
        tabBrowser = browserWindow.gBrowser;
      }

      let tab = tabBrowser.addTab("about:blank", {
        relatedToCurrent,
        inBackground: inNewWindow,
        skipAnimation: inNewWindow,
        triggeringPrincipal:
          Services.scriptSecurityManager.getSystemPrincipal(),
      });
      const viewSourceBrowser = tabBrowser.getBrowserForTab(tab);
      if (inNewWindow) {
        tabBrowser.hideTab(tab);
        tabBrowser.replaceTabsWithWindow(tab);
      }
      return viewSourceBrowser;
    };

    this.window.gViewSourceUtils.viewPartialSourceInBrowser(
      this.actor.browsingContext,
      openSelectionFn
    );
  }

  viewFrameSource() {
    this.window.BrowserCommands.viewSourceOfDocument({
      browser: this.browser,
      URL: this.contentData.docLocation,
      outerWindowID: this.frameOuterWindowID,
    });
  }

  viewInfo() {
    this.window.BrowserCommands.pageInfo(
      this.contentData.docLocation,
      null,
      null,
      null,
      this.browser
    );
  }

  viewImageInfo() {
    this.window.BrowserCommands.pageInfo(
      this.contentData.docLocation,
      "mediaTab",
      this.imageInfo,
      null,
      this.browser
    );
  }

  viewImageDesc(e) {
    this.window.urlSecurityCheck(
      this.imageDescURL,
      this.principal,
      Ci.nsIScriptSecurityManager.DISALLOW_SCRIPT
    );
    this.window.openUILink(this.imageDescURL, e, {
      referrerInfo: this.contentData.referrerInfo,
      triggeringPrincipal: this.principal,
      triggeringRemoteType: this.remoteType,
      policyContainer: this.policyContainer,
    });
  }

  viewFrameInfo() {
    this.window.BrowserCommands.pageInfo(
      this.contentData.docLocation,
      null,
      null,
      this.actor.browsingContext,
      this.browser
    );
  }

  reloadImage() {
    this.window.urlSecurityCheck(
      this.mediaURL,
      this.principal,
      Ci.nsIScriptSecurityManager.DISALLOW_SCRIPT
    );
    this.actor.reloadImage(this.targetIdentifier);
  }

  _canvasToBlobURL(targetIdentifier) {
    return this.actor.canvasToBlobURL(targetIdentifier);
  }

  copyCanvasImage() {
    this.actor.copyCanvasImage(this.targetIdentifier).then(arrayBuffer => {
      lazy.BrowserUtils.copyImageToClipboard(arrayBuffer);
    }, console.error);
  }

  viewMedia(e) {
    let where = lazy.BrowserUtils.whereToOpenLink(e, false, false);
    if (where == "current") {
      where = "tab";
    }
    let referrerInfo = this.contentData.referrerInfo;
    let systemPrincipal = Services.scriptSecurityManager.getSystemPrincipal();
    if (this.onCanvas) {
      this._canvasToBlobURL(this.targetIdentifier).then(blobURL => {
        this.window.openLinkIn(blobURL, where, {
          referrerInfo,
          triggeringPrincipal: systemPrincipal,
        });
      }, console.error);
    } else {
      const isAllowedChromeImage = ALLOWED_CHROME_IMAGE_URLS.has(this.mediaURL);
      const principal = isAllowedChromeImage ? systemPrincipal : this.principal;

      this.window.urlSecurityCheck(
        this.mediaURL,
        principal,
        Ci.nsIScriptSecurityManager.DISALLOW_SCRIPT
      );

      this.window.openLinkIn(this.mediaURL, where, {
        referrerInfo,
        forceAllowDataURI: true,
        triggeringPrincipal: principal,
        triggeringRemoteType: this.remoteType,
        policyContainer: this.policyContainer,
      });
    }
  }

  saveVideoFrameAsImage() {
    let isPrivate = lazy.PrivateBrowsingUtils.isBrowserPrivate(this.browser);

    let aName = "";
    if (this.mediaURL) {
      try {
        let uri = this.window.makeURI(this.mediaURL);
        let url = uri.QueryInterface(Ci.nsIURL);
        if (url.fileBaseName) {
          aName = decodeURI(url.fileBaseName) + ".jpg";
        }
      } catch (e) {}
    }
    if (!aName) {
      aName = "snapshot.jpg";
    }

    let referrerInfo = this.contentData.referrerInfo;
    let cookieJarSettings = this.contentData.cookieJarSettings;

    this.actor.saveVideoFrameAsImage(this.targetIdentifier).then(dataURL => {
      this.window.internalSave(
        dataURL,
        null, 
        null, 
        aName,
        null, 
        "image/jpeg", 
        true, 
        "SaveImageTitle",
        null, 
        referrerInfo,
        cookieJarSettings,
        null, 
        false, 
        null, 
        isPrivate,
        this.principal
      );
    });
  }

  leaveDOMFullScreen() {
    this.document.exitFullscreen();
  }

  viewBGImage(e) {
    this.window.urlSecurityCheck(
      this.bgImageURL,
      this.principal,
      Ci.nsIScriptSecurityManager.DISALLOW_SCRIPT
    );

    this.window.openUILink(this.bgImageURL, e, {
      referrerInfo: this.contentData.referrerInfo,
      forceAllowDataURI: true,
      triggeringPrincipal: this.principal,
      triggeringRemoteType: this.remoteType,
      policyContainer: this.policyContainer,
    });
  }

  saveFrame() {
    this.window.saveBrowser(this.browser, false, this.frameBrowsingContext);
  }

  saveHelper(
    linkURL,
    linkText,
    dialogTitle,
    bypassCache,
    doc,
    referrerInfo,
    cookieJarSettings,
    windowID,
    linkDownload,
    isContentWindowPrivate
  ) {
    function saveAsListener(principal, aWindow) {
      this._triggeringPrincipal = principal;
      this._window = aWindow;
    }
    saveAsListener.prototype = {
      extListener: null,

      onStartRequest: function saveLinkAs_onStartRequest(aRequest) {
        if (aRequest.status == Cr.NS_ERROR_SAVE_LINK_AS_TIMEOUT) {
          return;
        }

        timer.cancel();

        if (!Components.isSuccessCode(aRequest.status)) {
          try {
            const l10n = new Localization(["browser/downloads.ftl"], true);

            const msg = l10n.formatValueSync("downloads-error-generic");

            const win = Services.wm.getOuterWindowWithId(windowID);
            const title = l10n.formatValueSync("downloads-error-alert-title");
            Services.prompt.alert(win, title, msg);
          } catch (ex) {}
          return;
        }

        let extHelperAppSvc = Cc[
          "@mozilla.org/uriloader/external-helper-app-service;1"
        ].getService(Ci.nsIExternalHelperAppService);
        let channel = aRequest.QueryInterface(Ci.nsIChannel);
        this.extListener = extHelperAppSvc.doContent(
          channel.contentType,
          aRequest,
          null,
          true,
          this._window
        );
        this.extListener.onStartRequest(aRequest);
      },

      onStopRequest: function saveLinkAs_onStopRequest(aRequest, aStatusCode) {
        if (aStatusCode == Cr.NS_ERROR_SAVE_LINK_AS_TIMEOUT) {
          this._window.saveURL(
            linkURL,
            null,
            linkText,
            dialogTitle,
            bypassCache,
            false,
            referrerInfo,
            cookieJarSettings,
            doc,
            isContentWindowPrivate,
            this._triggeringPrincipal
          );
        }
        if (this.extListener) {
          this.extListener.onStopRequest(aRequest, aStatusCode);
        }
      },

      onDataAvailable: function saveLinkAs_onDataAvailable(
        aRequest,
        aInputStream,
        aOffset,
        aCount
      ) {
        this.extListener.onDataAvailable(
          aRequest,
          aInputStream,
          aOffset,
          aCount
        );
      },
    };

    function callbacks() {}
    callbacks.prototype = {
      getInterface: function sLA_callbacks_getInterface(aIID) {
        if (aIID.equals(Ci.nsIAuthPrompt) || aIID.equals(Ci.nsIAuthPrompt2)) {
          timer.cancel();
          channel.cancel(Cr.NS_ERROR_SAVE_LINK_AS_TIMEOUT);
        }
        throw Components.Exception("", Cr.NS_ERROR_NO_INTERFACE);
      },
    };

    function timerCallback() {}
    timerCallback.prototype = {
      notify: function sLA_timer_notify() {
        channel.cancel(Cr.NS_ERROR_SAVE_LINK_AS_TIMEOUT);
      },
    };

    var channel = lazy.NetUtil.newChannel({
      uri: this.window.makeURI(linkURL),
      loadingPrincipal: this.principal,
      contentPolicyType: Ci.nsIContentPolicy.TYPE_SAVEAS_DOWNLOAD,
      securityFlags: Ci.nsILoadInfo.SEC_ALLOW_CROSS_ORIGIN_INHERITS_SEC_CONTEXT,
    });

    if (linkDownload) {
      channel.contentDispositionFilename = linkDownload;
    }
    if (channel instanceof Ci.nsIPrivateBrowsingChannel) {
      let docIsPrivate = lazy.PrivateBrowsingUtils.isBrowserPrivate(
        this.browser
      );
      channel.setPrivate(docIsPrivate);
    }
    channel.notificationCallbacks = new callbacks();

    let flags = Ci.nsIChannel.LOAD_CALL_CONTENT_SNIFFERS;

    if (bypassCache) {
      flags |= Ci.nsIRequest.LOAD_BYPASS_CACHE;
    }

    if (channel instanceof Ci.nsICachingChannel) {
      flags |= Ci.nsICachingChannel.LOAD_BYPASS_LOCAL_CACHE_IF_BUSY;
    }

    channel.loadFlags |= flags;

    if (channel instanceof Ci.nsIHttpChannel) {
      channel.referrerInfo = referrerInfo;
      if (channel instanceof Ci.nsIHttpChannelInternal) {
        channel.forceAllowThirdPartyCookie = true;
      }

      channel.loadInfo.cookieJarSettings = cookieJarSettings;
    }

    var timeToWait = Services.prefs.getIntPref(
      "browser.download.saveLinkAsFilenameTimeout"
    );
    var timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
    timer.initWithCallback(
      new timerCallback(),
      timeToWait,
      timer.TYPE_ONE_SHOT
    );

    channel.asyncOpen(new saveAsListener(this.principal, this.window));
  }

  saveLink() {
    let referrerInfo = this.onLink
      ? this.contentData.linkReferrerInfo
      : this.contentData.referrerInfo;

    let isPrivate = lazy.PrivateBrowsingUtils.isBrowserPrivate(this.browser);
    this.saveHelper(
      this.linkURL,
      this.linkTextStr,
      null,
      true,
      this.ownerDoc,
      referrerInfo,
      this.contentData.cookieJarSettings,
      this.frameOuterWindowID,
      this.linkDownload,
      isPrivate
    );
  }

  saveImage() {
    if (this.onCanvas || this.onImage) {
      this.saveMedia();
    }
  }

  saveMedia() {
    let doc = this.ownerDoc;
    let isPrivate = lazy.PrivateBrowsingUtils.isBrowserPrivate(this.browser);
    let referrerInfo = this.contentData.referrerInfo;
    let cookieJarSettings = this.contentData.cookieJarSettings;
    if (this.onCanvas) {
      this._canvasToBlobURL(this.targetIdentifier).then(blobURL => {
        this.window.internalSave(
          blobURL,
          null, 
          null, 
          "canvas.png",
          null, 
          "image/png", 
          true, 
          "SaveImageTitle",
          null, 
          referrerInfo,
          cookieJarSettings,
          null, 
          false, 
          null, 
          isPrivate,
          this.document.nodePrincipal 
        );
      }, console.error);
    } else if (this.onImage) {
      const isAllowedChromeImage = ALLOWED_CHROME_IMAGE_URLS.has(this.mediaURL);
      const principal = isAllowedChromeImage
        ? Services.scriptSecurityManager.getSystemPrincipal()
        : this.principal;

      this.window.urlSecurityCheck(this.mediaURL, principal);
      this.window.internalSave(
        this.mediaURL,
        null, 
        null, 
        null, 
        this.contentData.contentDisposition,
        this.contentData.contentType,
        false, 
        "SaveImageTitle",
        null, 
        referrerInfo,
        cookieJarSettings,
        null, 
        false, 
        null, 
        isPrivate,
        principal
      );
    } else if (this.onVideo || this.onAudio) {
      let defaultFileName = "";
      if (this.mediaURL.startsWith("data")) {
        defaultFileName =
          this.window.ContentAreaUtils.stringBundle.GetStringFromName(
            "UntitledSaveFileName"
          );
      }

      var dialogTitle = this.onVideo ? "SaveVideoTitle" : "SaveAudioTitle";
      this.saveHelper(
        this.mediaURL,
        null,
        dialogTitle,
        false,
        doc,
        referrerInfo,
        cookieJarSettings,
        this.frameOuterWindowID,
        defaultFileName,
        isPrivate
      );
    }
  }

  sendImage() {
    if (this.onCanvas || this.onImage) {
      this.sendMedia();
    }
  }

  sendMedia() {
    this.window.MailIntegration.sendMessage(this.mediaURL, "");
  }

  copyEmail() {
    var url = this.linkURL;
    var qmark = url.indexOf("?");
    var addresses;

    addresses = qmark > 7 ? url.substring(7, qmark) : url.substr(7);

    try {
      addresses = Services.textToSubURI.unEscapeURIForUI(addresses);
    } catch (ex) {
    }

    lazy.clipboard.copyString(
      addresses,
      this.actor.manager.browsingContext.currentWindowGlobal
    );
  }

  copyPhone() {
    var url = this.linkURL;
    var phone = url.substr(4);

    try {
      phone = Services.textToSubURI.unEscapeURIForUI(phone);
    } catch (ex) {
    }

    lazy.clipboard.copyString(
      phone,
      this.actor.manager.browsingContext.currentWindowGlobal
    );
  }

  copyLink(url = this.linkURL) {
    let linkURL = url.replace(/^view-source:/, "");
    lazy.clipboard.copyString(
      linkURL,
      this.actor.manager.browsingContext.currentWindowGlobal
    );
  }

  copyStrippedLink(uri = this.linkURI) {
    let strippedLinkURI = this.getStrippedLink(uri);
    let strippedLinkURL =
      Services.io.createExposableURI(strippedLinkURI)?.displaySpec;
    if (strippedLinkURL) {
      lazy.clipboard.copyString(
        strippedLinkURL,
        this.actor.manager.browsingContext.currentWindowGlobal
      );
    }
  }

  async addSearchFieldAsEngine() {
    let { url, formData, charset, method } =
      await this.actor.getSearchFieldEngineData(this.targetIdentifier);

    for (let value of formData.values()) {
      if (typeof value != "string") {
        throw new Error("Non-string values are not supported.");
      }
    }

    let { engineInfo } = await this.window.gDialogBox.open(
      "chrome://browser/content/search/addEngine.xhtml",
      {
        mode: "FORM",
        title: true,
        nameTemplate: Services.io.newURI(url).host,
      }
    );

    if (engineInfo) {
      let searchEngine = await lazy.SearchService.addUserEngine({
        name: engineInfo.name,
        alias: engineInfo.alias,
        url,
        params: new URLSearchParams(formData),
        charset,
        method,
      });

      this.window.gURLBar.search("", { searchEngine });
    }
  }


  showItem(aItemOrId, aShow) {
    var item =
      aItemOrId.constructor == String
        ? this.document.getElementById(aItemOrId)
        : aItemOrId;
    if (item) {
      item.hidden = !aShow;
    }
  }

  setItemAttr(aID, aAttr, aVal) {
    var elem = this.document.getElementById(aID);
    if (!elem) {
      return;
    }
    if (aVal == null) {
      elem.removeAttribute(aAttr);
      return;
    }
    if (typeof aVal == "boolean") {
      if (aVal) {
        elem.setAttribute(aAttr, aVal);
      } else {
        elem.removeAttribute(aAttr);
      }
      return;
    }
    elem.setAttribute(aAttr, aVal);
  }

  cloneNode(aItem) {
    var node = this.document.createElement(aItem.tagName);

    var attrs = aItem.attributes;
    for (var i = 0; i < attrs.length; i++) {
      var attr = attrs.item(i);
      node.setAttribute(attr.nodeName, attr.nodeValue);
    }

    return node;
  }

  getLinkURI(url = this.linkURL) {
    try {
      return this.window.makeURI(url);
    } catch (ex) {
    }

    return null;
  }

  getStrippedLink(uri = this.linkURI) {
    if (!uri) {
      return null;
    }
    let strippedLinkURI = null;
    try {
      strippedLinkURI = lazy.QueryStringStripper.stripForCopyOrShare(uri);
    } catch (e) {
      console.warn(`getStrippedLink: ${e.message}`);
      return uri;
    }

    return strippedLinkURI ?? uri;
  }

  canStripParams(uri = this.linkURI) {
    if (!uri) {
      return false;
    }
    try {
      return lazy.QueryStringStripper.canStripForShare(uri);
    } catch (e) {
      console.warn("canStripForShare failed!", e);
      return false;
    }
  }

  isSecureAboutPage() {
    let { currentURI } = this.browser;
    if (currentURI?.schemeIs("about")) {
      let module = lazy.E10SUtils.getAboutModule(currentURI);
      if (module) {
        let flags = module.getURIFlags(currentURI);
        return !!(flags & Ci.nsIAboutModule.IS_SECURE_CHROME_UI);
      }
    }
    return false;
  }

  shouldShowSeparator(aSeparatorID) {
    var separator = this.document.getElementById(aSeparatorID);
    if (separator) {
      var sibling = separator.previousSibling;
      while (sibling && sibling.localName != "menuseparator") {
        if (!sibling.hidden) {
          return true;
        }
        sibling = sibling.previousSibling;
      }
    }
    return false;
  }

  shouldShowAddEngine() {
    let uri = this.browser.currentURI;

    return (
      this.onTextInput &&
      this.onSearchField &&
      (uri.schemeIs("http") || uri.schemeIs("https"))
    );
  }

  addDictionaries() {
    var uri = Services.urlFormatter.formatURLPref(
      "browser.dictionaries.download.url"
    );

    var locale = "-";
    try {
      locale = Services.locale.acceptLanguages;
    } catch (e) {}

    var version = "-";
    try {
      version = Services.appinfo.version;
    } catch (e) {}

    uri = uri.replace(/%LOCALE%/, escape(locale)).replace(/%VERSION%/, version);

    var newWindowPref = Services.prefs.getIntPref(
      "browser.link.open_newwindow"
    );
    var where = newWindowPref == 3 ? "tab" : "window";

    this.window.openTrustedLinkIn(uri, where);
  }

  bookmarkThisPage() {
    this.window.top.PlacesCommandHook.bookmarkPage().catch(console.error);
  }

  bookmarkLink() {
    this.window.top.PlacesCommandHook.bookmarkLink(
      this.linkURL,
      this.linkTextStr
    ).catch(console.error);
  }

  addBookmarkForFrame() {
    let uri = this.contentData.documentURIObject;

    this.actor.getFrameTitle(this.targetIdentifier).then(title => {
      this.window.top.PlacesCommandHook.bookmarkLink(uri.spec, title).catch(
        console.error
      );
    });
  }

  savePageAs() {
    this.window.saveBrowser(this.browser);
  }

  switchPageDirection() {
    this.window.gBrowser.selectedBrowser.sendMessageToActor(
      "SwitchDocumentDirection",
      {},
      "SwitchDocumentDirection",
      "roots"
    );
  }

  mediaCommand(command, data) {
    this.actor.mediaCommand(this.targetIdentifier, command, data);
  }

  copyMediaLocation() {
    lazy.clipboard.copyString(
      this.originalMediaURL,
      this.actor.manager.browsingContext.currentWindowGlobal
    );
  }

  showAndFormatSearchContextItem() {
    let selectedText = this.isTextSelected
      ? this.selectedText
      : this.linkTextStr;

    let { document } = this.window;
    let menuItem = document.getElementById("context-searchselect");
    let menuItemPrivate = document.getElementById(
      "context-searchselect-private"
    );

    let opts = {
      isContextRelevant: (this.isTextSelected || this.onLink) && !this.onImage,
      searchTerms: selectedText,
      searchUrlType: lazy.SearchUtils.URL_TYPE.SEARCH,
    };
    this.#updateSearchMenuitem({
      ...opts,
      menuitem: menuItem,
    });
    this.#updateSearchMenuitem({
      ...opts,
      menuitem: menuItemPrivate,
      isPrivateSearchMenuitem: true,
    });

    let frameSeparator = document.getElementById("frame-sep");

    frameSeparator.toggleAttribute(
      "ensureHidden",
      menuItem.hidden && this.inFrame
    );

    if (menuItem.hidden && menuItemPrivate.hidden) {
      return;
    }

    if (selectedText.length > 15) {
      let truncLength = 15;
      let truncChar = selectedText[15].charCodeAt(0);
      if (truncChar >= 0xdc00 && truncChar <= 0xdfff) {
        truncLength++;
      }
      selectedText =
        selectedText.substr(0, truncLength) + Services.locale.ellipsis;
    }

    const { gNavigatorBundle } = this.window;
    let engineName = lazy.SearchService.defaultEngine.name;
    let privateEngineName = lazy.SearchService.defaultPrivateEngine.name;
    if (!menuItem.hidden) {
      const docIsPrivate = lazy.PrivateBrowsingUtils.isBrowserPrivate(
        this.browser
      );

      let menuLabel = gNavigatorBundle.getFormattedString("contextMenuSearch", [
        docIsPrivate ? privateEngineName : engineName,
        selectedText,
      ]);
      menuItem.label = menuLabel;
      menuItem.accessKey = gNavigatorBundle.getString(
        "contextMenuSearch.accesskey"
      );
    }

    if (!menuItemPrivate.hidden) {
      let otherEngine = engineName != privateEngineName;
      let accessKey = "contextMenuPrivateSearch.accesskey";
      if (otherEngine) {
        menuItemPrivate.label = gNavigatorBundle.getFormattedString(
          "contextMenuPrivateSearchOtherEngine",
          [privateEngineName]
        );
        accessKey = "contextMenuPrivateSearchOtherEngine.accesskey";
      } else {
        menuItemPrivate.label = gNavigatorBundle.getString(
          "contextMenuPrivateSearch"
        );
      }
      menuItemPrivate.accessKey = gNavigatorBundle.getString(accessKey);
    }
  }

  #updateSearchMenuitem({
    menuitem,
    isContextRelevant,
    searchTerms,
    searchUrlType,
    isPrivateSearchMenuitem = false,
  }) {
    if (!menuitem) {
      return;
    }
    if (!lazy.SearchService.hasSuccessfullyInitialized) {
      menuitem.hidden = true;
      return;
    }

    if (isPrivateSearchMenuitem && !lazy.PrivateBrowsingUtils.enabled) {
      menuitem.hidden = true;
      return;
    }

    let isBrowserPrivate = lazy.PrivateBrowsingUtils.isBrowserPrivate(
      this.browser
    );
    let engine =
      isBrowserPrivate || isPrivateSearchMenuitem
        ? lazy.SearchService.defaultPrivateEngine
        : lazy.SearchService.defaultEngine;

    menuitem.hidden =
      !isContextRelevant ||
      !engine?.supportsResponseType(searchUrlType) ||
      (isPrivateSearchMenuitem &&
        (isBrowserPrivate ||
          !Services.prefs.getBoolPref(
            "browser.search.separatePrivateDefault.ui.enabled"
          )));

    if (!menuitem.hidden) {
      let url = engine.getURLOfType(searchUrlType);
      if (
        url?.acceptedContentTypes &&
        (!this.contentData?.contentType ||
          !url.acceptedContentTypes.includes(this.contentData.contentType))
      ) {
        menuitem.hidden = true;
      }
    }

    if (!menuitem.hidden) {
      menuitem.engine = engine;
      menuitem.searchTerms = searchTerms;
      menuitem.principal = this.principal;
      menuitem.policyContainer = this.policyContainer;
      menuitem.usePrivate = isPrivateSearchMenuitem || isBrowserPrivate;
    }
  }

  loadSearch({ event, searchUrlType = null }) {
    let { engine, searchTerms, usePrivate, principal, policyContainer } =
      event.target;
    lazy.SearchUIUtils.loadSearchFromContext({
      event,
      engine,
      policyContainer,
      searchUrlType,
      usePrivateWindow: usePrivate,
      window: this.window,
      searchText: searchTerms,
      triggeringPrincipal: principal,
    });
  }

  createContainerMenu(aEvent) {
    let createMenuOptions = {
      isContextMenu: true,
      excludeUserContextId: this.contentData.userContextId,
    };
    return this.window.createUserContextMenu(aEvent, createMenuOptions);
  }

}
