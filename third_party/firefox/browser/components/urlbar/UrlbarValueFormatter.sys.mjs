/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  BrowserUIUtils: "resource:///modules/BrowserUIUtils.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

export class UrlbarValueFormatter {
  constructor(urlbarInput) {
    this.#urlbarInput = urlbarInput;

    this.#window.addEventListener("resize", this);
  }

  async update() {
    let instance = (this.#updateInstance = {});

    if (!this.#window.gBrowserInit.delayedStartupFinished) {
      await this.#window.delayedStartupPromise;
      if (this.#updateInstance != instance) {
        return;
      }
    }
    if (!lazy.SearchService.isInitialized) {
      try {
        await lazy.SearchService.init();
      } catch {}

      if (this.#updateInstance != instance) {
        return;
      }
    }

    if (!this.#window.docShell) {
      return;
    }

    this.#urlbarInput.removeAttribute("domaindir");
    this.#scheme.value = "";

    if (!this.#urlbarInput.value) {
      return;
    }

    this.#removeURLFormat();
    this.#removeSearchAliasFormat();

    this.#window.requestAnimationFrame(() => {
      if (this.#updateInstance != instance) {
        return;
      }
      this.#formattingApplied = this.#formatURL() || this.#formatSearchAlias();
    });
  }

  #urlbarInput;

  #hostRange = null;

  get #document() {
    return this.#urlbarInput.document;
  }

  get #inputField() {
    return this.#urlbarInput.inputField;
  }

  get #window() {
    return this.#urlbarInput.window;
  }

  get #scheme() {
    return  (
      this.#urlbarInput.querySelector("#urlbar-scheme")
    );
  }

  #scrollHostIntoView(isRTL) {
    if (this.#hostRange) {
      const hostRect = this.#hostRange.getBoundingClientRect();
      const urlbarRect = this.#inputField.getBoundingClientRect();

      const scrollDelta = isRTL
        ? hostRect.left - urlbarRect.left
        : hostRect.right - urlbarRect.right;

      const scrollTarget = this.#inputField.scrollLeft + scrollDelta;

      this.#inputField.scrollLeft = isRTL
        ? Math.min(this.#inputField.scrollLeftMax, scrollTarget)
        : Math.max(0, scrollTarget);
    } else {
      this.#inputField.scrollLeft = isRTL ? this.#inputField.scrollLeftMax : 0;
    }
  }

  #ensureFormattedHostVisible(urlMetaData) {
    urlMetaData = urlMetaData || this.#getUrlMetaData();
    if (!urlMetaData) {
      this.#urlbarInput.removeAttribute("domaindir");
      return;
    }
    let { url, preDomain, domain } = urlMetaData;
    let directionality = this.#window.windowUtils.getDirectionFromText(domain);
    if (
      directionality == this.#window.windowUtils.DIRECTION_RTL &&
      url[preDomain.length + domain.length] != "\u200E"
    ) {
      this.#urlbarInput.setAttribute("domaindir", "rtl");
      this.#scrollHostIntoView(true);
    } else {
      this.#urlbarInput.setAttribute("domaindir", "ltr");
      this.#scrollHostIntoView(false);
    }
    this.#urlbarInput.updateTextOverflow();
  }

  #getUrlMetaData() {
    if (this.#urlbarInput.focused) {
      return null;
    }

    let inputValue = this.#urlbarInput.value;
    if (!inputValue) {
      return null;
    }
    let browser = this.#window.gBrowser.selectedBrowser;
    let browserState = this.#urlbarInput.getBrowserState(browser);

    if (
      browserState.urlMetaData &&
      browserState.urlMetaData.inputValue == inputValue &&
      browserState.urlMetaData.untrimmedValue ==
        this.#urlbarInput.untrimmedValue
    ) {
      return browserState.urlMetaData.data;
    }
    browserState.urlMetaData = {
      inputValue,
      untrimmedValue: this.#urlbarInput.untrimmedValue,
      data: null,
    };

    const untrimmedValue = this.#urlbarInput.untrimmedValue;
    let uri;
    let uriInfo;
    if (
      untrimmedValue.startsWith("http://") ||
      untrimmedValue.startsWith("https://")
    ) {
      try {
        uri = Services.io.newURI(untrimmedValue);
      } catch (e) {}
    }
    if (!uri) {
      let flags =
        Services.uriFixup.FIXUP_FLAG_FIX_SCHEME_TYPOS |
        Services.uriFixup.FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP;
      if (lazy.PrivateBrowsingUtils.isWindowPrivate(this.#window)) {
        flags |= Services.uriFixup.FIXUP_FLAG_PRIVATE_CONTEXT;
      }
      try {
        uriInfo = Services.uriFixup.getFixupURIInfo(untrimmedValue, flags);
      } catch (ex) {}
      if (
        !uriInfo ||
        !uriInfo.fixedURI ||
        uriInfo.keywordProviderId ||
        !["http", "https"].includes(uriInfo.fixedURI.scheme)
      ) {
        return null;
      }
      uri = uriInfo.fixedURI;
    }

    let url = inputValue;
    let trimmedLength = 0;
    let trimmedProtocol = lazy.BrowserUIUtils.trimURLProtocol;
    if (
      untrimmedValue.startsWith(trimmedProtocol) &&
      !inputValue.startsWith(trimmedProtocol)
    ) {
      url = trimmedProtocol + inputValue;
      trimmedLength = trimmedProtocol.length;
    } else if (
      uriInfo?.schemelessInput == Ci.nsILoadInfo.SchemelessInputTypeSchemeless
    ) {
      let scheme = uri.scheme + "://";
      url = scheme + url;
      trimmedLength = scheme.length;
    }

    let matchedURL = url.match(
      /^(([a-z]+:\/\/)(?:[^\/#?]+@)?)(\S+?)(?::\d+)?\s*(?:[\/#?]|$)/
    );
    if (!matchedURL) {
      return null;
    }
    let [, preDomain, schemeWSlashes, domain] = matchedURL;

    let replaceUrl = false;
    try {
      replaceUrl =
        Services.io.newURI("http://" + domain).displayHost != uri.displayHost;
    } catch (ex) {
      return null;
    }
    if (replaceUrl) {
      if (this.#inGetUrlMetaData) {
        return null;
      }
      try {
        this.#inGetUrlMetaData = true;
        this.#window.gBrowser.userTypedValue = null;
        this.#urlbarInput.setURI({ uri });
        return this.#getUrlMetaData();
      } finally {
        this.#inGetUrlMetaData = false;
      }
    }

    return (browserState.urlMetaData.data = {
      domain,
      origin: uri.host,
      preDomain,
      schemeWSlashes,
      trimmedLength,
      url,
    });
  }

  #removeURLFormat() {
    this.#hostRange = null;
    if (!this.#formattingApplied) {
      return;
    }
    let controller = this.#urlbarInput.editor.selectionController;
    let strikeOut = controller.getSelection(controller.SELECTION_URLSTRIKEOUT);
    strikeOut.removeAllRanges();
    let selection = controller.getSelection(controller.SELECTION_URLSECONDARY);
    selection.removeAllRanges();
    this.#formatScheme(controller.SELECTION_URLSTRIKEOUT, true);
    this.#formatScheme(controller.SELECTION_URLSECONDARY, true);
    this.#inputField.style.setProperty("--urlbar-scheme-size", "0px");
  }

  get formattingEnabled() {
    return lazy.UrlbarPrefs.get("formatting.enabled");
  }

  willShowFormattedMixedContentProtocol(val) {
    return (
      this.formattingEnabled &&
      !lazy.UrlbarPrefs.get("security.insecure_connection_text.enabled") &&
      val.startsWith("https://") &&
      val == this.#urlbarInput.value &&
      this.#showingMixedContentLoadedPageUrl
    );
  }

  #formattingApplied = false;

  #updateInstance;

  #selectedResult;

  #resizeThrottleTimeout;

  #resizeInstance;

  #inGetUrlMetaData = false;

  get #showingMixedContentLoadedPageUrl() {
    return (
      this.#urlbarInput.getAttribute("pageproxystate") == "valid" &&
      !!(
        this.#window.gBrowser.securityUI.state &
        Ci.nsIWebProgressListener.STATE_LOADED_MIXED_ACTIVE_CONTENT
      )
    );
  }

  #formatURL() {
    let urlMetaData = this.#getUrlMetaData();
    if (!urlMetaData) {
      return false;
    }
    let state = this.#urlbarInput.getBrowserState(
      this.#window.gBrowser.selectedBrowser
    );
    if (state.searchTerms) {
      return false;
    }

    let { domain, origin, preDomain, schemeWSlashes, trimmedLength, url } =
      urlMetaData;

    let isUnformattedMixedContent =
      this.#showingMixedContentLoadedPageUrl && !this.formattingEnabled;
    if (
      !lazy.UrlbarPrefs.get("security.insecure_connection_text.enabled") &&
      !isUnformattedMixedContent &&
      this.#urlbarInput.value.startsWith(schemeWSlashes)
    ) {
      this.#scheme.value = schemeWSlashes;
      this.#inputField.style.setProperty(
        "--urlbar-scheme-size",
        schemeWSlashes.length + "ch"
      );
    }

    let editor = this.#urlbarInput.editor;
    let textNode = editor.rootElement.firstChild;

    const hostStart = preDomain.length - trimmedLength;
    const hostEnd = hostStart + domain.length;

    if (hostStart < hostEnd) {
      this.#hostRange = this.#document.createRange();
      this.#hostRange.setStart(textNode, hostStart);
      this.#hostRange.setEnd(textNode, hostEnd);
    } else {
      this.#hostRange = null;
    }

    this.#ensureFormattedHostVisible(urlMetaData);

    if (!this.formattingEnabled) {
      return false;
    }

    let controller = editor.selectionController;

    this.#formatScheme(controller.SELECTION_URLSECONDARY);

    if (this.willShowFormattedMixedContentProtocol(this.#urlbarInput.value)) {
      let range = this.#document.createRange();
      range.setStart(textNode, 0);
      range.setEnd(textNode, 5);
      let strikeOut = controller.getSelection(
        controller.SELECTION_URLSTRIKEOUT
      );
      strikeOut.addRange(range);
      this.#formatScheme(controller.SELECTION_URLSTRIKEOUT);
    }

    let baseDomain = domain;
    let subDomain = "";
    try {
      baseDomain = Services.eTLD.getBaseDomainFromHost(origin);
      if (!domain.endsWith(baseDomain)) {
        let IDNService = Cc["@mozilla.org/network/idn-service;1"].getService(
          Ci.nsIIDNService
        );
        baseDomain = IDNService.domainToDisplay(baseDomain);
      }
    } catch (e) {}
    if (baseDomain != domain) {
      subDomain = domain.slice(0, -baseDomain.length);
    }

    let selection = controller.getSelection(controller.SELECTION_URLSECONDARY);

    let rangeLength = preDomain.length + subDomain.length - trimmedLength;
    if (rangeLength) {
      let range = this.#document.createRange();
      range.setStart(textNode, 0);
      range.setEnd(textNode, rangeLength);
      selection.addRange(range);
    }

    let startRest = preDomain.length + domain.length - trimmedLength;
    if (startRest < url.length - trimmedLength) {
      let range = this.#document.createRange();
      range.setStart(textNode, startRest);
      range.setEnd(textNode, url.length - trimmedLength);
      selection.addRange(range);
    }

    return true;
  }

  #formatScheme(selectionType, clear) {
    let editor = this.#scheme.editor;
    let controller = editor.selectionController;
    let textNode = editor.rootElement.firstChild;
    let selection = controller.getSelection(selectionType);
    if (clear) {
      selection.removeAllRanges();
    } else {
      let r = this.#document.createRange();
      r.setStart(textNode, 0);
      r.setEnd(textNode, textNode.textContent.length);
      selection.addRange(r);
    }
  }

  #removeSearchAliasFormat() {
    if (!this.#formattingApplied) {
      return;
    }
    let selection = this.#urlbarInput.editor.selectionController.getSelection(
      Ci.nsISelectionController.SELECTION_FIND
    );
    selection.removeAllRanges();
  }

  #formatSearchAlias() {
    if (!this.formattingEnabled) {
      return false;
    }

    let editor = this.#urlbarInput.editor;
    let textNode = editor.rootElement.firstChild;
    let value = textNode.textContent;
    let trimmedValue = value.trim();

    if (
      !trimmedValue.startsWith("@") ||
      this.#urlbarInput.view.oneOffSearchButtons.selectedButton
    ) {
      return false;
    }

    let alias = this.#findEngineAliasOrRestrictKeyword();
    if (!alias) {
      return false;
    }

    if (trimmedValue != alias && !trimmedValue.startsWith(alias + " ")) {
      return false;
    }

    let index = value.indexOf(alias);
    if (index < 0) {
      return false;
    }

    let selection = editor.selectionController.getSelection(
      Ci.nsISelectionController.SELECTION_FIND
    );

    let end = index + alias.length;
    if (value[end] === " ") {
      end++;
    }

    let range = this.#document.createRange();
    range.setStart(textNode, index);
    range.setEnd(textNode, end);
    selection.addRange(range);

    let fg = "#2362d7";
    let bg = "#d2e6fd";

    if (
      this.#document.documentElement.hasAttribute("lwtheme") ||
      this.#window.matchMedia("(prefers-contrast)").matches
    ) {
      selection.setColors(fg, bg, "currentColor", "currentColor");
    } else {
      selection.setColors(fg, bg, fg, bg);
    }

    return true;
  }

  #findEngineAliasOrRestrictKeyword() {
    this.#selectedResult =
      this.#urlbarInput.view.selectedResult ||
      this.#urlbarInput.view.getResultAtIndex(0) ||
      this.#selectedResult;

    if (!this.#selectedResult) {
      return null;
    }

    let { type, payload } = this.#selectedResult;

    if (type === lazy.UrlbarShared.RESULT_TYPE.SEARCH) {
      return payload.keyword || null;
    }

    if (type === lazy.UrlbarShared.RESULT_TYPE.RESTRICT) {
      return payload.autofillKeyword || null;
    }

    return null;
  }

  handleEvent(event) {
    let methodName = "_on_" + event.type;
    if (methodName in this) {
      this[methodName](event);
    } else {
      throw new Error("Unrecognized UrlbarValueFormatter event: " + event.type);
    }
  }

  _on_resize(event) {
    if (event.target != this.#window) {
      return;
    }
    if (this.#resizeThrottleTimeout) {
      this.#window.clearTimeout(this.#resizeThrottleTimeout);
    }
    this.#resizeThrottleTimeout = this.#window.setTimeout(() => {
      this.#resizeThrottleTimeout = null;
      let instance = (this.#resizeInstance = {});
      this.#window.requestAnimationFrame(() => {
        if (instance == this.#resizeInstance) {
          if (
            this.#hostRange &&
            !this.#hostRange.commonAncestorContainer.isConnected
          ) {
            this.#removeURLFormat();
            this.#formatURL();
          } else {
            this.#ensureFormattedHostVisible();
          }
        }
      });
    }, 100);
  }
}
