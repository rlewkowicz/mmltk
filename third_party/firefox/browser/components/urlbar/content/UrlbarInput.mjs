/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const { XPCOMUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
);

const { AppConstants } = ChromeUtils.importESModule(
  "resource://gre/modules/AppConstants.sys.mjs"
);

import { SearchModeSwitcher } from "chrome://browser/content/urlbar/SearchModeSwitcher.mjs";
import { UrlbarChildController } from "chrome://browser/content/urlbar/UrlbarChildController.mjs";
import { UrlbarEventBufferer } from "chrome://browser/content/urlbar/UrlbarEventBufferer.mjs";
import { UrlbarView } from "chrome://browser/content/urlbar/UrlbarView.mjs";
import { UrlbarShared } from "chrome://browser/content/urlbar/UrlbarShared.mjs";




const lazy = XPCOMUtils.declareLazy({
  BrowserUIUtils: "resource:///modules/BrowserUIUtils.sys.mjs",
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  ConfigSearchEngine:
    "moz-src:///toolkit/components/search/ConfigSearchEngine.sys.mjs",
  CustomizableUI:
    "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs",
  ObjectUtils: "resource://gre/modules/ObjectUtils.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  SearchUIUtils: "moz-src:///browser/components/search/SearchUIUtils.sys.mjs",
  SearchUtils: "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarQueryContext:
    "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
  UrlbarProviderOpenTabs:
    "moz-src:///browser/components/urlbar/UrlbarProviderOpenTabs.sys.mjs",
  UrlbarSearchUtils:
    "moz-src:///browser/components/urlbar/UrlbarSearchUtils.sys.mjs",
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
  UrlbarValueFormatter:
    "moz-src:///browser/components/urlbar/UrlbarValueFormatter.sys.mjs",
  UrlbarSearchTermsPersistence:
    "moz-src:///browser/components/urlbar/UrlbarSearchTermsPersistence.sys.mjs",
  UrlUtils: "resource://gre/modules/UrlUtils.sys.mjs",
  ClipboardHelper: {
    service: "@mozilla.org/widget/clipboardhelper;1",
    iid: Ci.nsIClipboardHelper,
  },
  QUERY_STRIPPING_STRIP_ON_SHARE: {
    pref: "privacy.query_stripping.strip_on_share.enabled",
    default: false,
  },
  logger: () => UrlbarShared.getLogger({ prefix: "Input" }),
});

const UNLIMITED_MAX_RESULTS = 99;

let getBoundsWithoutFlushing = element =>
  element.documentGlobal.windowUtils.getBoundsWithoutFlushing(element);
let px = number => number.toFixed(2) + "px";

export class UrlbarInput extends HTMLElement {
  static get #markup() {
    return `
      <html:div class="urlbar-background"/>
      <html:div class="urlbar-input-container"
            pageproxystate="invalid">
        <html:moz-urlbar-slot name="remote-control-box" />

        <html:moz-button class="searchmode-switcher chromeclass-toolbar-additional"
                         iconsrc="chrome://global/skin/icons/search-glass.svg"
                         title="More options"
                         aria-label="More options"
                         data-l10n-id="urlbar-searchmode-default2"
                         tabindex="-1"
                         role="combobox">
          <!-- This span has no purpose other than making the moz-button think
               it contains text even when searchmode-switcher-title is hidden. -->
          <html:span class="urlbar-visually-hidden" aria-hidden="true">a</html:span>
          <html:span class="searchmode-switcher-content">
            <html:img class="searchmode-switcher-dropmarker"
                      data-l10n-id="urlbar-searchmode-dropmarker2"
                      draggable="false" />
            <html:span class="searchmode-switcher-title" />
            <html:button class="searchmode-switcher-close toolbarbutton-icon close-button"
                         data-l10n-id="urlbar-searchmode-exit-button2"
                         tabindex="-1"
                         keyNav="false" />
          </html:span>
        </html:moz-button>
        <!-- In XUL windows, this will be wrapped in a panel with class="searchmode-switcher-panel". -->
        <html:panel-list class="searchmode-switcher-panel-list">
          <html:div class="searchmode-switcher-panel-description" role="heading" />
${
  Services.prefs.getBoolPref("browser.nova.enabled", false)
    ? '<html:hr class="searchmode-switcher-panel-installed-engine-separator"/><html:hr class="searchmode-switcher-panel-footer-separator"/>'
    : '<html:hr/><html:hr class="searchmode-switcher-panel-installed-engine-separator searchmode-switcher-panel-footer-separator"/>'
}
        </html:panel-list>

        <html:moz-urlbar-slot name="site-info" />
        <moz-input-box tooltip="aHTMLTooltip"
                       class="urlbar-input-box"
                       flex="1">
          <!-- In the addressbar, there will be an input with id="urlbar-scheme" here. -->
          <html:input class="urlbar-input textbox-input"
                      role="combobox"
                      aria-autocomplete="both"
                      inputmode="mozAwesomebar"
                      data-l10n-id="urlbar-placeholder"/>
        </moz-input-box>
        <html:moz-urlbar-slot name="revert-button" />
        <html:img class="urlbar-icon urlbar-go-button"
               role="button"
               keyNav="false"
               data-l10n-id="urlbar-go-button2"/>
        <html:moz-urlbar-slot name="page-actions" />
      </html:div>
      <html:div class="urlbarView"
            role="group"
            tooltip="aHTMLTooltip">
        <html:div class="urlbarView-body-outer">
          <html:div class="urlbarView-body-inner">
            <html:div class="urlbarView-results"
                      role="listbox"/>
          </html:div>
        </html:div>
        <html:panel-list class="urlbarView-result-menu"></html:panel-list>
        <html:moz-urlbar-slot name="search-one-offs" />
   </html:div>`;
  }

  static get fragment() {
    if (!UrlbarInput.#fragment) {
      UrlbarInput.#fragment = window.MozXULElement.parseXULToFragment(
        UrlbarInput.#markup
      );
    }
    // @ts-ignore
    return document.importNode(UrlbarInput.#fragment, true);
  }

  static get observedAttributes() {
    return ["open"];
  }

  static #fragment;

  static #inputFieldEvents = [
    "compositionstart",
    "compositionend",
    "contextmenu",
    "dragover",
    "dragstart",
    "drop",
    "focus",
    "blur",
    "input",
    "beforeinput",
    "keydown",
    "keyup",
    "mouseover",
    "overflow",
    "underflow",
    "paste",
    "scrollend",
    "select",
    "selectionchange",
  ];

  #allowBreakout = false;
  #gBrowserListenersAdded = false;
  #breakoutBlockerCount = 0;
  #isAddressbar = false;
  #sapName;
  _userTypedValue = "";
  _actionOverrideKeyCount = 0;
  _lastValidURLStr = "";
  _valueOnLastSearch = "";
  _suppressStartQuery = false;
  _suppressPrimaryAdjustment = false;
  _lastSearchString = "";
  #compositionState = lazy.UrlbarUtils.COMPOSITION.NONE;
  #compositionClosedPopup = false;
  #compositionHadText = false;

  valueIsTyped = false;

  lastQueryContextPromise = Promise.resolve();

  _autofillPlaceholder = null;

  _resultForCurrentValue = null;
  _untrimmedValue = "";
  _enableAutofillPlaceholder = true;

  constructor() {
    super();

    this.window = this.documentGlobal;
    this.document = this.window.document;
    this.isPrivate = lazy.PrivateBrowsingUtils.isWindowPrivate(this.window);

    lazy.UrlbarPrefs.addObserver(this);
    window.addEventListener("unload", () => {
      lazy.UrlbarPrefs.removeObserver(this);
    });
  }

  #populateSlots() {
    let urlbarSlots = this.querySelectorAll("moz-urlbar-slot[name]");
    for (let slot of urlbarSlots) {
      let slotName = slot.getAttribute("name");
      let nodes = this.querySelectorAll(`:scope > [urlbar-slot="${slotName}"]`);

      for (let node of nodes) {
        slot.parentNode.insertBefore(node, slot);
      }

      slot.remove();
    }

    this._identityBox = this.querySelector(".identity-box");
    this._revertButton = this.querySelector(".urlbar-revert-button");
    this._searchModeIndicator = this.querySelector(
      "#urlbar-search-mode-indicator"
    );
    this._searchModeIndicatorTitle = this._searchModeIndicator?.querySelector(
      "#urlbar-search-mode-indicator-title"
    );
    this._searchModeIndicatorClose = this._searchModeIndicator?.querySelector(
      "#urlbar-search-mode-indicator-close"
    );
  }

  #init() {
    this.#sapName =  (
      this.getAttribute("sap-name")
    );
    this.#isAddressbar = this.#sapName == "urlbar";

    this.addEventListener(
      "moz-input-box-rebuilt",
      this.#onContextMenuRebuilt.bind(this)
    );

    this.appendChild(UrlbarInput.fragment);

    if (document.readyState === "loading") {
      document.addEventListener(
        "DOMContentLoaded",
        () => this.#populateSlots(),
        { once: true }
      );
    } else {
      this.#populateSlots();
    }

    this.panel = this.querySelector(".urlbarView");
    this._inputContainer = this.querySelector(".urlbar-input-container");
    this.inputField =  (
      this.querySelector(".urlbar-input")
    );

    let resultListboxId = this.#sapName + "-results";
    this.querySelector(".urlbarView-results").id = resultListboxId;
    this.inputField.setAttribute("aria-controls", resultListboxId);

    if (this.#isAddressbar) {
      this.inputField.id = "urlbar-input";

      let schemeField = document.createElement("input");
      schemeField.id = "urlbar-scheme";
      schemeField.required = true;
      this.inputField.before(schemeField);
    }
    if (this.#sapName == "searchbar") {
      this.inputField.setAttribute("type", "search");
    }

    this.controller = new UrlbarChildController({ input: this });
    this.view = new UrlbarView(this);
    this.searchModeSwitcher = new SearchModeSwitcher(this);

    let searchModeSwitcherDescription = this.querySelector(
      ".searchmode-switcher-panel-description"
    );

    searchModeSwitcherDescription.setAttribute(
      "data-l10n-id",
      this.#isAddressbar &&
        !Services.prefs.getBoolPref("browser.nova.enabled", false)
        ? "urlbar-searchmode-popup-one-off-header"
        : "urlbar-searchmode-popup-header"
    );

    this.eventBufferer = new UrlbarEventBufferer(this);

    const READ_WRITE_PROPERTIES = [
      "placeholder",
      "selectionStart",
      "selectionEnd",
    ];

    for (let property of READ_WRITE_PROPERTIES) {
      Object.defineProperty(this, property, {
        enumerable: true,
        get() {
          return this.inputField[property];
        },
        set(val) {
          this.inputField[property] = val;
        },
      });
    }

    this._setPlaceholder(null);
  }

  attributeChangedCallback(attribute, _oldValue, _newValue) {
    if (attribute != "open") {
      return;
    }

    this.updateLayoutExtend();
  }

  connectedCallback() {
    if (
      this.getAttribute("sap-name") == "searchbar" &&
      !lazy.UrlbarPrefs.get("browser.search.widget.new")
    ) {
      return;
    }

    this.#connectedCallback();
  }

  #connectedCallback() {
    if (!this.controller) {
      this.#init();
    }

    if (this.inOverflowPanel && this.view.isOpen) {
      this.view.close();
    }

    if (this.readOnly) {
      this.#stopBreakout();
      this.#allowBreakout = false;
      this.removeAttribute("focused");
      return;
    }
    this.toggleAttribute("focused", this.focused);

    if (
      this.sapName == "searchbar" &&
      !document.documentElement.hasAttribute("customizing")
    ) {
      let storedWidth = Services.xulStore.getValue(
        document.documentURI,
        this.parentElement.id,
        "width"
      );
      if (storedWidth) {
        this.parentElement.setAttribute("width", storedWidth);
         (this.parentElement).style.width =
          storedWidth + "px";
      }
    }

    this._initCopyCutController();

    for (let event of UrlbarInput.#inputFieldEvents) {
      this.inputField.addEventListener(event, this);
    }

    this.window.addEventListener("keydown", this);
    this.window.addEventListener("keyup", this);

    this.window.addEventListener("mousedown", this);
    if (AppConstants.platform == "win") {
      this.window.addEventListener("draggableregionleftmousedown", this);
    }
    this.addEventListener("mousedown", this);

    this._inputContainer.addEventListener("click", this);
    this._inputContainer.addEventListener("auxclick", this);

    this.view.panel.addEventListener("command", this, true);

    this.window.addEventListener("toolbarvisibilitychange", this);
    let menuToolbar = this.window.document.getElementById("toolbar-menubar");
    if (menuToolbar) {
      menuToolbar.addEventListener("DOMMenuBarInactive", this);
      menuToolbar.addEventListener("DOMMenuBarActive", this);
    }
    this.window.addEventListener("uidensitychanged", this);

    if (this.window.gBrowser) {
      this.addGBrowserListeners();
    }

    if (
      Cu.isESModuleLoaded(
        "moz-src:///toolkit/components/search/SearchService.sys.mjs"
      ) &&
      lazy.SearchService.isInitialized
    ) {
      this.searchModeSwitcher.updateSearchIcon();
      this._updatePlaceholderFromDefaultEngine();
    }

    this.#allowBreakout =
      !!this.closest("toolbar") &&
      !document.documentElement.hasAttribute("customizing");
    if (this.#allowBreakout) {
      this._resizeObserver = new this.window.ResizeObserver(([entry]) => {
        this.style.setProperty(
          "--urlbar-width",
          px(entry.borderBoxSize[0].inlineSize)
        );
      });
      this._resizeObserver.observe(this.parentNode);

      this.#updateLayoutBreakout();
    } else {
      this.#stopBreakout();
    }

    this._addObservers();
  }

  disconnectedCallback() {
    if (
      this.getAttribute("sap-name") == "searchbar" &&
      !lazy.UrlbarPrefs.get("browser.search.widget.new")
    ) {
      return;
    }

    this.#disconnectedCallback();
  }

  #disconnectedCallback() {
    if (this.sapName == "searchbar") {
      this.searchMode = null;
    }

    if (this._copyCutController) {
      this.inputField.controllers.removeController(this._copyCutController);
      delete this._copyCutController;
    }

    for (let event of UrlbarInput.#inputFieldEvents) {
      this.inputField.removeEventListener(event, this);
    }

    this.window.removeEventListener("keydown", this);
    this.window.removeEventListener("keyup", this);

    this.window.removeEventListener("mousedown", this);
    if (AppConstants.platform == "win") {
      this.window.removeEventListener("draggableregionleftmousedown", this);
    }
    this.removeEventListener("mousedown", this);

    this._inputContainer.removeEventListener("click", this);
    this._inputContainer.removeEventListener("auxclick", this);

    this.view.panel.removeEventListener("command", this, true);

    this.window.removeEventListener("toolbarvisibilitychange", this);
    let menuToolbar = this.window.document.getElementById("toolbar-menubar");
    if (menuToolbar) {
      menuToolbar.removeEventListener("DOMMenuBarInactive", this);
      menuToolbar.removeEventListener("DOMMenuBarActive", this);
    }
    this.window.removeEventListener("uidensitychanged", this);

    if (this.#gBrowserListenersAdded) {
      this.window.gBrowser.tabContainer.removeEventListener("TabSelect", this);
      this.window.gBrowser.tabContainer.removeEventListener("TabClose", this);
      this.window.gBrowser.removeTabsProgressListener(this);
      this.#gBrowserListenersAdded = false;
    }

    this._resizeObserver?.disconnect();

    this._removeObservers();
  }

  #contextMenuListeners = [];

  #addContextMenuListener(listener) {
    let inputBox = this.querySelector("moz-input-box");
    inputBox.addEventListener("contextmenu", listener);
    this.#contextMenuListeners.push(listener);
  }

  #removeContextMenuListeners() {
    let inputBox = this.querySelector("moz-input-box");
    for (let listener of this.#contextMenuListeners) {
      inputBox.removeEventListener("contextmenu", listener);
    }
    this.#contextMenuListeners.length = 0;
  }

  #onContextMenuRebuilt() {
    this.#removeContextMenuListeners();

    this._initPasteAndGo();
    if (this.#isAddressbar) {
      this._initAutofillDismiss();
    }
  }

  addGBrowserListeners() {
    if (this.window.gBrowser && !this.#gBrowserListenersAdded) {
      this.window.gBrowser.tabContainer.addEventListener("TabSelect", this);
      this.window.gBrowser.tabContainer.addEventListener("TabClose", this);
      this.window.gBrowser.addTabsProgressListener(this);
      this.#gBrowserListenersAdded = true;
    }
  }

  #lazy = XPCOMUtils.declareLazy({
    valueFormatter: () => new lazy.UrlbarValueFormatter(this),
    addSearchEngineHelper: () => new AddSearchEngineHelper(this),
  });

  get addSearchEngineHelper() {
    return this.#lazy.addSearchEngineHelper;
  }

  get sapName() {
    return this.#sapName;
  }


  blur() {
    this.inputField.blur();
  }

  set readOnly(val) {
    if (val != this.inputField.readOnly) {
      this.inputField.readOnly = val;
      if (this.isConnected) {
        this.#disconnectedCallback();
        this.#connectedCallback();
      }
    }
  }

  get readOnly() {
    return this.inputField.readOnly;
  }

  placeholder;

  selectionStart;

  selectionEnd;

  onPrefChanged(pref) {
    switch (pref) {
      case "keyword.enabled":
        this._updatePlaceholderFromDefaultEngine().catch(e =>
          console.warn("Falied to update urlbar placeholder:", e)
        );
        break;
      case "browser.search.widget.new": {
        if (this.getAttribute("sap-name") == "searchbar" && this.isConnected) {
          if (lazy.UrlbarPrefs.get("browser.search.widget.new")) {
            this.#connectedCallback();
          } else {
            this.#disconnectedCallback();
          }
        }
      }
    }
  }

  formatValue() {
    if (this.#isAddressbar && this.editor) {
      this.#lazy.valueFormatter.update();
    }
  }

  focus() {
    let beforeFocus = new CustomEvent("beforefocus", {
      bubbles: true,
      cancelable: true,
    });
    this.inputField.dispatchEvent(beforeFocus);
    if (beforeFocus.defaultPrevented) {
      return;
    }

    this.inputField.focus();
  }

  select() {
    let beforeSelect = new CustomEvent("beforeselect", {
      bubbles: true,
      cancelable: true,
    });
    this.inputField.dispatchEvent(beforeSelect);
    if (beforeSelect.defaultPrevented) {
      return;
    }

    this._suppressPrimaryAdjustment = true;
    this.inputField.select();
    this._suppressPrimaryAdjustment = false;
  }

  setSelectionRange(selectionStart, selectionEnd) {
    let beforeSelect = new CustomEvent("beforeselect", {
      bubbles: true,
      cancelable: true,
    });
    this.inputField.dispatchEvent(beforeSelect);
    if (beforeSelect.defaultPrevented) {
      return;
    }

    this._suppressPrimaryAdjustment = true;
    this.inputField.setSelectionRange(selectionStart, selectionEnd);
    this._suppressPrimaryAdjustment = false;
  }

  saveSelectionStateForBrowser(browser) {
    let state = this.getBrowserState(browser);
    state.selection = {
      start: this.value ? this.selectionStart : 0,
      end: this.value ? this.selectionEnd : Number.MAX_SAFE_INTEGER,
      shouldUntrim: this.value && !this._protocolIsTrimmed,
    };
  }

  restoreSelectionStateForBrowser(browser) {
    this.focus();
    let state = this.getBrowserState(browser);
    if (state.selection) {
      if (state.selection.shouldUntrim) {
        this.#maybeUntrimUrl();
      }
      this.setSelectionRange(
        state.selection.start,
        Math.min(state.selection.end, this.value.length)
      );
    }
  }

  setURI({
    uri = null,
    dueToTabSwitch = false,
    dueToSessionRestore = false,
    hideSearchTerms = false,
    isSameDocument = false,
  } = {}) {
    if (!this.#isAddressbar) {
      throw new Error(
        "Cannot set URI for UrlbarInput that is not an address bar"
      );
    }
    if (dueToTabSwitch) {
      this._updateSearchModeUI(this.searchMode);
    }

    let state = this.getBrowserState(this.window.gBrowser.selectedBrowser);
    this.#handlePersistedSearchTerms({
      state,
      uri,
      dueToTabSwitch,
      hideSearchTerms,
      isSameDocument,
    });

    let value = this.userTypedValue;
    let valid = false;
    let isReverting = !uri;

    if (value === null || (!value && dueToTabSwitch)) {
      uri =
        this.window.gBrowser.selectedBrowser.currentAuthPromptURI ||
        uri ||
        this.#isOpenedPageInBlankTargetLoading ||
        this.window.gBrowser.currentURI;
      try {
        uri = Services.io.createExposableURI(uri);
      } catch (e) {}

      let isInitialPageControlledByWebContent = false;

      if (
        this.window.isInitialPage(uri) &&
        lazy.BrowserUIUtils.checkEmptyPageOrigin(
          this.window.gBrowser.selectedBrowser,
          uri
        )
      ) {
        value = "";
      } else {
        isInitialPageControlledByWebContent = true;

        try {
          value = losslessDecodeURI(uri);
        } catch (ex) {
          value = "about:blank";
        }
      }
      valid =
        !dueToSessionRestore &&
        (!this.#canHandleAsBlankPage(uri.spec) ||
          isInitialPageControlledByWebContent);
    } else if (
      this.window.isInitialPage(value) &&
      lazy.BrowserUIUtils.checkEmptyPageOrigin(
        this.window.gBrowser.selectedBrowser
      )
    ) {
      value = "";
      valid = true;
    }

    const previousUntrimmedValue = this.untrimmedValue;
    let offset = this._protocolIsTrimmed
      ? lazy.BrowserUIUtils.trimURLProtocol.length
      : 0;
    const previousSelectionStart = this.selectionStart + offset;
    const previousSelectionEnd = this.selectionEnd + offset;

    this._setValue(value, { allowTrim: true, valueIsTyped: !valid });
    this.toggleAttribute("usertyping", !valid && value);

    if (this.focused && value != previousUntrimmedValue) {
      if (
        previousSelectionStart != previousSelectionEnd &&
        value.substring(previousSelectionStart, previousSelectionEnd) ===
          previousUntrimmedValue.substring(
            previousSelectionStart,
            previousSelectionEnd
          )
      ) {
        this.inputField.setSelectionRange(
          previousSelectionStart - offset,
          previousSelectionEnd - offset
        );
      } else if (
        previousSelectionEnd &&
        (previousUntrimmedValue.length === previousSelectionEnd ||
          value.length <= previousSelectionEnd)
      ) {
        this.inputField.setSelectionRange(value.length, value.length);
      } else {
        this.inputField.setSelectionRange(
          previousSelectionEnd - offset,
          previousSelectionEnd - offset
        );
      }
    }

    this.setPageProxyState(
      valid ? "valid" : "invalid",
      dueToTabSwitch,
      !isReverting &&
        dueToTabSwitch &&
        this.getBrowserState(this.window.gBrowser.selectedBrowser)
          .isUnifiedSearchButtonAvailable
    );

    if (
      state.persist?.shouldPersist &&
      !lazy.UrlbarSearchTermsPersistence.searchModeMatchesState(
        this.searchMode,
        state
      )
    ) {
      if (state.persist.isDefaultEngine) {
        this.searchMode = null;
      } else {
        this.searchMode = {
          engineName: state.persist.originalEngineName,
          source: UrlbarShared.RESULT_SOURCE.SEARCH,
          isPreview: false,
        };
      }
    } else if (dueToTabSwitch && !valid) {
      this.restoreSearchModeState();
    } else if (valid) {
      this.searchMode = null;
    }

    let event = new CustomEvent("SetURI", { bubbles: true });
    this.inputField.dispatchEvent(event);
  }

  makeURIReadable(uri) {
    try {
      return Services.io.createExposableURI(uri);
    } catch (ex) {}

    return uri;
  }

  onLocationChange(browser, webProgress, request, locationURI) {
    if (!webProgress.isTopLevel) {
      return;
    }

    if (
      browser != this.window.gBrowser.selectedBrowser &&
      !this.#canHandleAsBlankPage(locationURI.spec)
    ) {
      this.getBrowserState(browser).isUnifiedSearchButtonAvailable = false;
    }

  }

  handleEvent(event) {
    let methodName = "_on_" + event.type;
    if (methodName in this) {
      try {
        this[methodName](event);
      } catch (e) {
        console.error(`Error calling UrlbarInput::${methodName}:`, e);
      }
    } else {
      throw new Error("Unrecognized UrlbarInput event: " + event.type);
    }
  }

  handleCommand(event = null) {
    let isMouseEvent = MouseEvent.isInstance(event);
    if (isMouseEvent && event.button == 2) {
      return;
    }

    if (this.view.isOpen) {
      let selectedOneOff = this.view.oneOffSearchButtons?.selectedButton;
      if (selectedOneOff && (!isMouseEvent || event.target == selectedOneOff)) {
        this.view.oneOffSearchButtons.handleSearchCommand(event, {
          engineName: selectedOneOff.engine?.name,
          source: selectedOneOff.source,
          entry: "oneoff",
        });
        return;
      }
    }

    if (lazy.UrlbarPrefs.get("unifiedSearchButton.always")) {
      this.searchModeSwitcher?.updateSearchIcon();
    }

    this.handleNavigation({ event });
  }


  handleNavigation({ event, oneOffParams, triggeringPrincipal }) {
    let element = this.view.selectedElement;
    let result = this.view.getResultFromElement(element);
    let openParams = oneOffParams?.openParams || { triggeringPrincipal };

    let isComposing = this.editor.composing;

    let selectedPrivateResult =
      result &&
      result.type == UrlbarShared.RESULT_TYPE.SEARCH &&
      result.payload.inPrivateWindow;
    let selectedPrivateEngineResult =
      selectedPrivateResult && result.payload.isPrivateEngine;
    let safeToPickResult =
      result &&
      (result.heuristic ||
        !this.valueIsTyped ||
        result.type == UrlbarShared.RESULT_TYPE.TIP ||
        this.value == this.#getValueFromResult(result));
    if (
      !isComposing &&
      element &&
      (!oneOffParams?.engine || selectedPrivateEngineResult) &&
      safeToPickResult
    ) {
      this.pickElement(element, event);
      return;
    }

    if (
      lazy.UrlbarPrefs.get("experimental.hideHeuristic") &&
      !element &&
      !isComposing &&
      !oneOffParams?.engine &&
      this._resultForCurrentValue?.heuristic
    ) {
      this.pickResult(this._resultForCurrentValue, event);
      return;
    }

    if (!result && this.value.startsWith("@")) {
      let tokenAliasResult = this.view.getResultAtIndex(0);
      if (tokenAliasResult?.autofill && tokenAliasResult?.payload.keyword) {
        this.pickResult(tokenAliasResult, event);
        return;
      }
    }

    let url;
    let typedValue = this.value;
    if (oneOffParams?.engine) {
      typedValue = this._lastSearchString;
      result = this._resultForCurrentValue;

      let searchString = result?.payload.query || this._lastSearchString;
      [url, openParams.postData] = lazy.UrlbarUtils.getSearchQueryUrl(
        oneOffParams.engine,
        searchString
      );
      if (oneOffParams.openWhere == "tab") {
      } else {
      }

    } else {
      url = this.untrimmedValue;
      openParams.postData = null;
    }

    if (!url) {
      if (this.sapName == "searchbar") {
        let searchEngine;
        if (this.searchMode) {
          searchEngine = lazy.UrlbarSearchUtils.getEngineByName(
            this.searchMode.engineName
          );
        } else {
          searchEngine = lazy.UrlbarSearchUtils.getDefaultEngine(
            this.isPrivate
          );
        }

        this.openSearchEnginePage("", {
          searchEngine,
          event,
          where: this._whereToOpen(event),
        });
      }
      return;
    }

    if (
      this.searchMode &&
      !this.searchMode.engineName &&
      !result &&
      !oneOffParams
    ) {
      return;
    }

    let where = oneOffParams?.openWhere || this._whereToOpen(event);
    if (selectedPrivateResult) {
      where = "window";
      openParams.private = true;
    }
    openParams.allowInheritPrincipal = false;
    url = this._maybeCanonizeURL(event, url) || url.trim();

    let selectedResult = result || this.view.selectedResult;

    if (this.#isAddressbar && URL.canParse(url)) {
      openParams.schemelessInput = this.#getSchemelessInput(
        this.untrimmedValue
      );
      this._loadURL(url, event, where, openParams);
      return;
    }


    if (!isComposing && this._resultForCurrentValue) {
      this.pickResult(this._resultForCurrentValue, event);
      return;
    }

    let browser = this.window.gBrowser.selectedBrowser;
    let lastLocationChange = browser.lastLocationChange;


    lazy.UrlbarUtils.getHeuristicResultFor(url, this)
      .then(newResult => {
        if (
          where != "current" ||
          browser.lastLocationChange == lastLocationChange
        ) {
          this.pickResult(newResult, event, null, browser);
        }
      })
      .catch(() => {
        if (url) {


          let flags =
            Ci.nsIURIFixup.FIXUP_FLAG_FIX_SCHEME_TYPOS |
            Ci.nsIURIFixup.FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP;
          if (this.isPrivate) {
            flags |= Ci.nsIURIFixup.FIXUP_FLAG_PRIVATE_CONTEXT;
          }
          let {
            preferredURI: uri,
            postData,
            keywordAsSent,
          } = Services.uriFixup.getFixupURIInfo(url, flags);
          if (
            where != "current" ||
            browser.lastLocationChange == lastLocationChange
          ) {
            openParams.postData = postData;
            if (!keywordAsSent) {
              openParams.schemelessInput = this.#getSchemelessInput(
                this.untrimmedValue
              );
            }
            this._loadURL(uri.spec, event, where, openParams, null, browser);
          }
        }
      });
  }

  handleRevert() {
    this.userTypedValue = null;
    this.searchMode = null;
    if (!this.#isAddressbar) {
      this.value = "";
      this.toggleAttribute("usertyping", false);
      return;
    }

    this.setURI({
      dueToTabSwitch: true,
      hideSearchTerms: true,
    });

    if (this.value && this.focused) {
      this.select();
    }
  }

  maybeHandleRevertFromPopup(anchorElement) {
    let state = this.getBrowserState(this.window.gBrowser.selectedBrowser);
    if (anchorElement?.closest("#urlbar") && state.persist?.shouldPersist) {
      this.handleRevert();
    }
  }

  handoff(searchString, searchEngine, newtabSessionId) {
    this._isHandoffSession = true;
    this._handoffSession = newtabSessionId;
    if (lazy.UrlbarPrefs.get("shouldHandOffToSearchMode") && searchEngine) {
      this.search(searchString, {
        searchEngine,
        searchModeEntry: "handoff",
      });
    } else {
      this.search(searchString);
    }
  }

  pickElement(element, event) {
    let result = this.view.getResultFromElement(element);
    lazy.logger.debug(
      `pickElement ${element} with event ${event?.type}, result: ${result}`
    );
    if (!result) {
      return;
    }
    this.pickResult(result, event, element);
  }

  // eslint-disable-next-line complexity
  pickResult(
    result,
    event,
    element = null,
    browser = this.window.gBrowser.selectedBrowser
  ) {
    if (element?.classList.contains("urlbarView-button-menu")) {
      this.view.openResultMenu(result, element);
      return;
    }

    if (element?.dataset.command) {
      this.#pickMenuResult(result, event, element, browser);
      return;
    }

    if (
      lazy.UrlbarPrefs.get("autoFill.adaptiveHistory.enabled") &&
      result.autofill &&
      result.payload?.url &&
      !this.isPrivate
    ) {
      lazy.UrlbarUtils.clearAutofillBackspaceEntryForUrl(result.payload.url);
    }

    if (
      result.providerName == "UrlbarProviderGlobalActions" &&
      this.#providesSearchMode(result) &&
      !this.view.selectedElement?.dataset.immediateSearch
    ) {
      this.maybeConfirmSearchModeFromResult({
        result,
        checkValue: false,
      });
      return;
    }

    if (
      (this.searchMode?.isPreview &&
        result.providerName == "UrlbarProviderGlobalActions" &&
        !this.view.selectedElement?.dataset.immediateSearch) ||
      (result.heuristic &&
        this.searchMode?.isPreview &&
        this.view.oneOffSearchButtons?.selectedButton)
    ) {
      this.confirmSearchMode();
      this.search(this.value);
      return;
    }

    if (
      result.type == UrlbarShared.RESULT_TYPE.TIP &&
      result.payload.type == "dismissalAcknowledgment"
    ) {
      this.view.onQueryResultRemoved(result.rowIndex);
      return;
    }

    let resultUrl = element?.dataset.url;
    let originalUntrimmedValue = this.untrimmedValue;
    let isCanonized = this.setValueFromResult({
      result,
      event,
      element,
      urlOverride: resultUrl,
    });

    let where;
    let openParams = {
      allowInheritPrincipal: false,
      globalHistoryOptions: {
        triggeringSource: this.#sapName,
        triggeringSearchEngine: result.payload?.engine,
      },
      private: this.isPrivate,
    };

    if (element?.closest("#urlbarView-context-menu")) {
      switch (element.id) {
        case "urlbar-view-context-menu-open-in-tab": {
          where = "tab";
          break;
        }
        case "urlbarView-context-menu-open-in-window": {
          where = "window";
          break;
        }
        case "urlbarView-context-menu-open-in-private-window": {
          where = "window";
          openParams.private = true;
          break;
        }
        default: {
          where = "tab";
          openParams.userContextId = parseInt(
            element.getAttribute("data-usercontextid")
          );
        }
      }

      openParams.forceForeground = false;
    } else {
      where = this._whereToOpen(event);
      if (resultUrl && where == "current") {
        where = "tab";
      }

      openParams.forceForeground = true;
    }

    let keepViewOpen = lazy.BrowserUtils.willLoadInBackground(
      where,
      openParams
    );
    openParams.avoidBrowserFocus = keepViewOpen;

    if (!this.#providesSearchMode(result) && !keepViewOpen) {
      this.view.close({ elementPicked: true });
    }

    if (isCanonized) {
      this._loadURL(this._untrimmedValue, event, where, openParams, browser);
      return;
    }

    let { url, postData } = resultUrl
      ? { url: resultUrl, postData: null }
      : lazy.UrlbarUtils.getUrlFromResult(result, { element });
    openParams.postData = postData;

    switch (result.type) {
      case UrlbarShared.RESULT_TYPE.URL: {
        if (result.heuristic) {
          if (
            lazy.UrlbarPrefs.get("browser.fixup.dns_first_for_single_words") &&
            lazy.UrlbarUtils.looksLikeSingleWordHost(originalUntrimmedValue)
          ) {
            url = originalUntrimmedValue;
          }
          openParams.schemelessInput = this.#getSchemelessInput(
            originalUntrimmedValue
          );
        }
        break;
      }
      case UrlbarShared.RESULT_TYPE.KEYWORD: {
        openParams.allowInheritPrincipal = true;
        break;
      }
      case UrlbarShared.RESULT_TYPE.TAB_SWITCH: {
        if (
          this.hasAttribute("action-override") ||
          (lazy.UrlbarPrefs.get("secondaryActions.switchToTab") &&
            element?.dataset.action !== "tabswitch")
        ) {
          where = "current";
          break;
        }

        this.handleRevert();
        let prevTab = this.window.gBrowser.selectedTab;
        let loadOpts = {
          adoptIntoActiveWindow: lazy.UrlbarPrefs.get(
            "switchTabs.adoptIntoActiveWindow"
          ),
        };

        let searchString = this._lastSearchString;

        let activeSplitView = this.window.gBrowser.selectedTab.splitview;

        let switched = this.window.switchToTabHavingURI(
          Services.io.newURI(url),
          true,
          loadOpts,
          lazy.UrlbarProviderOpenTabs.isNonPrivateUserContextId(
            result.payload.userContextId
          )
            ? result.payload.userContextId
            : null,
          activeSplitView
        );
        if (switched && !activeSplitView && prevTab.isEmpty) {
          this.window.gBrowser.removeTab(prevTab);
        }

        if (switched && !this.isPrivate && !result.heuristic) {
          lazy.UrlbarUtils.addToInputHistory(url, searchString).catch(
            console.error
          );
        }

        if (!switched) {
          console.error(`Tried to switch to non-existent tab: ${url}`);
          lazy.UrlbarProviderOpenTabs.unregisterOpenTab(
            url,
            result.payload.userContextId,
            result.payload.tabGroup,
            this.isPrivate
          );
        }

        return;
      }
      case UrlbarShared.RESULT_TYPE.SEARCH: {
        if (result.payload.providesSearchMode) {
          this.maybeConfirmSearchModeFromResult({
            result,
            checkValue: false,
          });
          return;
        }

        if (
          !this.searchMode &&
          result.heuristic &&
          !lazy.UrlbarPrefs.get("browser.fixup.dns_first_for_single_words") &&
          lazy.UrlbarPrefs.get("dnsResolveSingleWordsAfterSearch") > 0 &&
          this.window.gKeywordURIFixup &&
          lazy.UrlbarUtils.looksLikeSingleWordHost(originalUntrimmedValue)
        ) {
          let fixupInfo = this._getURIFixupInfo(originalUntrimmedValue.trim());
          if (fixupInfo) {
            this.window.gKeywordURIFixup.check(
              this.window.gBrowser.selectedBrowser,
              fixupInfo
            );
          }
        }

        if (result.payload.inPrivateWindow) {
          where = "window";
          openParams.private = true;
        }

        break;
      }
      case UrlbarShared.RESULT_TYPE.TIP: {
        if (url) {
          break;
        }
        this.handleRevert();
        return;
      }
      case UrlbarShared.RESULT_TYPE.DYNAMIC: {
        if (!url) {
          const { searchMode } = this;
          if (this.sapName != "searchbar") {
            this.handleRevert();
          }
          return;
        }
        break;
      }
      case UrlbarShared.RESULT_TYPE.RESTRICT: {
        this.handleRevert();
        this.maybeConfirmSearchModeFromResult({
          result,
          checkValue: false,
        });

        return;
      }
    }

    if (!url) {
      throw new Error(`Invalid url for result ${JSON.stringify(result)}`);
    }

    if (!this.isPrivate) {
      let input;
      if (!result.heuristic && result.type != UrlbarShared.RESULT_TYPE.SEARCH) {
        input = this._lastSearchString;
      } else if (
        result.autofill?.type == "adaptive_url" ||
        result.autofill?.type == "adaptive_origin"
      ) {
        input = result.autofill.adaptiveHistoryInput;
      } else if (
        lazy.UrlbarPrefs.get("autoFill.adaptiveHistory.enabled") &&
        result.autofill?.type == "origin" &&
        this._lastSearchString?.length > 0
      ) {
        lazy.UrlbarUtils.addToInputHistoryWhenReady(
          url,
          this._lastSearchString
        ).catch(console.error);
      }

      if (input !== undefined) {
        lazy.UrlbarUtils.addToInputHistory(url, input).catch(console.error);
      }

      if (
        lazy.UrlbarPrefs.get("autoFill.adaptiveHistory.enabled") &&
        !result.autofill &&
        result.type == UrlbarShared.RESULT_TYPE.URL
      ) {
        let isOrigin = lazy.UrlbarUtils.isOriginUrl(url);
        let clear = isOrigin
          ? lazy.UrlbarUtils.clearOriginAutofillBlock(url)
          : lazy.UrlbarUtils.clearOriginPageAutofillBlock(url);
        clear
          .then(wasBlocked => {
            let entry = lazy.UrlbarUtils.getBackspaceBlock(url);
            lazy.UrlbarUtils.clearAutofillBackspaceEntryForUrl(url);

            if (!wasBlocked) {
              return;
            }

            let level = isOrigin ? "origin" : "url";

          })
          .catch(console.error);
      }
    }




    this._loadURL(
      url,
      event,
      where,
      openParams,
      {
        source: result.source,
        type: result.type,
        searchTerm: result.payload.query,
      },
      browser,
      keepViewOpen
    );
  }

  setValueFromResult({
    result = null,
    event = null,
    urlOverride = null,
    element = null,
  } = {}) {
    this.setPageProxyState("invalid", true);

    if (
      this.searchMode?.isPreview &&
      !this.#providesSearchMode(result) &&
      !this.view.oneOffSearchButtons?.selectedButton
    ) {
      this.searchMode = null;
    }

    if (!result) {
      this.value = this._lastSearchString || this._valueOnLastSearch;
      this.setResultForCurrentValue(result);
      return false;
    }


    let canonizedUrl = this._maybeCanonizeURL(
      event,
      result.autofill ? this._lastSearchString : this.value
    );
    if (canonizedUrl) {
      this._setValue(canonizedUrl);

      this.setResultForCurrentValue(result);
      return true;
    }

    if (result.autofill) {
      this._autofillValue(result.autofill);
    }

    if (this.#providesSearchMode(result)) {
      let enteredSearchMode;
      if (this.view.resultIsSelected(result)) {
        enteredSearchMode = this.maybeConfirmSearchModeFromResult({
          result,
          checkValue: false,
          startQuery: this.view.visibleResults.length == 1,
        });
      }
      if (!enteredSearchMode) {
        this._setValue(this.#getValueFromResult(result), {
          actionType: this.#getActionTypeFromResult(result),
        });
        this.searchMode = null;
      }
      this.setResultForCurrentValue(result);
      return false;
    }

    if (!result.autofill) {
      let value = this.#getValueFromResult(result, { urlOverride, element });
      this._setValue(value, {
        actionType: this.#getActionTypeFromResult(result),
      });
    }

    this.setResultForCurrentValue(result);

    if (!result.autofill && this._autofillPlaceholder) {
      this._autofillPlaceholder.value = this.value;
      this._autofillPlaceholder.selectionStart = this.value.length;
      this._autofillPlaceholder.selectionEnd = this.value.length;
    }

    return false;
  }

  setResultForCurrentValue(result) {
    this._resultForCurrentValue = result;
  }

  _autofillFirstResult(result) {
    if (!result.autofill) {
      return;
    }

    let isPlaceholderSelected =
      this._autofillPlaceholder &&
      this.selectionEnd == this._autofillPlaceholder.value.length &&
      this.selectionStart == this._lastSearchString.length &&
      this._autofillPlaceholder.value
        .toLocaleLowerCase()
        .startsWith(this._lastSearchString.toLocaleLowerCase());

    if (
      !isPlaceholderSelected &&
      !this._autofillIgnoresSelection &&
      (this.selectionStart != this.selectionEnd ||
        this.selectionEnd != this._lastSearchString.length)
    ) {
      return;
    }

    this.setValueFromResult({ result });
  }
  #clearAutofill() {
    if (!this._autofillPlaceholder) {
      return;
    }
    let currentSelectionStart = this.selectionStart;
    let currentSelectionEnd = this.selectionEnd;

    this.inputField.value = this.value.substring(
      0,
      this._autofillPlaceholder.selectionStart
    );
    this._autofillPlaceholder = null;
    this.setSelectionRange(currentSelectionStart, currentSelectionEnd);
  }

  onFirstResult(firstResult) {
    if (
      firstResult.heuristic &&
      firstResult.payload.keyword &&
      !this.#providesSearchMode(firstResult) &&
      this.maybeConfirmSearchModeFromResult({
        result: firstResult,
        entry: "typed",
        checkValue: false,
      })
    ) {
      return true;
    }

    if (firstResult.autofill) {
      this._autofillFirstResult(firstResult);
    } else if (
      this._autofillPlaceholder &&
      !this.value.endsWith(" ")
    ) {
      this._autofillPlaceholder = null;
      this._setValue(this.userTypedValue);
    }

    return false;
  }

  startQuery({
    allowAutofill,
    autofillIgnoresSelection = false,
    searchString,
    resetSearchState = true,
    event,
    interactionType,
  } = {}) {
    if (!searchString) {
      searchString =
        this.getAttribute("pageproxystate") == "valid" ? "" : this.value;
    } else if (!this.value.startsWith(searchString)) {
      throw new Error("The current value doesn't start with the search string");
    }

    let queryContext = this.#makeQueryContext({
      allowAutofill,
      event,
      searchString,
    });


    if (this._suppressStartQuery) {
      return;
    }

    this._autofillIgnoresSelection = autofillIgnoresSelection;
    if (resetSearchState) {
      this._resetSearchState();
    }

    if (this.searchMode) {
      this.confirmSearchMode();
    }

    if (this.inOverflowPanel) {
      return;
    }

    this._lastSearchString = searchString;
    this._valueOnLastSearch = this.value;

    this.lastQueryContextPromise = this.controller.startQuery(queryContext);
  }

  search(value, options = {}) {
    let { searchEngine, searchModeEntry, startQuery = true } = options;
    if (options.focus ?? true) {
      this.focus();
    }
    let trimmedValue = value.trim();
    let end = trimmedValue.search(lazy.UrlUtils.REGEXP_SPACES);
    let firstToken = end == -1 ? trimmedValue : trimmedValue.substring(0, end);
    let searchMode = this.searchModeForToken(firstToken);
    let firstTokenIsRestriction = !!searchMode;
    if (!searchMode && searchEngine) {
      searchMode = { engineName: searchEngine.name };
      firstTokenIsRestriction = searchEngine.aliases.includes(firstToken);
    }

    if (searchMode) {
      searchMode.entry = searchModeEntry;
      this.searchMode = searchMode;
      if (firstTokenIsRestriction) {
        value = value.replace(firstToken, "");
      }
      if (lazy.UrlUtils.REGEXP_SPACES.test(value[0])) {
        value = value.slice(1);
      }
    } else if (
      Object.values(UrlbarShared.RESTRICT_TOKENS).includes(firstToken)
    ) {
      this.searchMode = null;
      if (Object.values(UrlbarShared.RESTRICT_TOKENS).includes(value)) {
        value += " ";
      }
    }
    this.inputField.value = value;
    this.selectionStart = -1;

    if (startQuery) {
      this._lastSearchString = "";
      let event = new UIEvent("input", {
        bubbles: true,
        cancelable: false,
        view: this.window,
        detail: 0,
      });
      this.inputField.dispatchEvent(event);
    }
  }

  searchModeForToken(token) {
    if (token == UrlbarShared.RESTRICT_TOKENS.SEARCH) {
      return {
        engineName: lazy.UrlbarSearchUtils.getDefaultEngine(this.isPrivate)
          ?.name,
      };
    }

    let mode =
      this.#isAddressbar &&
      lazy.UrlbarUtils.LOCAL_SEARCH_MODES.find(m => m.restrict == token);
    if (mode) {
      return { ...mode };
    }

    return null;
  }

  openSearchEnginePage(
    value,
    { searchEngine, event, where, inBackground = false }
  ) {
    if (!searchEngine || !event || !where) {
      console.warn("Missing parameters");
      return;
    }

    let trimmedValue = value.trim();
    let url, postData;
    if (trimmedValue) {
      [url, postData] = lazy.UrlbarUtils.getSearchQueryUrl(
        searchEngine,
        trimmedValue
      );
      if (where.startsWith("tab")) {
      } else {
      }

      if (where == "current") {
        this.setSearchMode(
          {
            engineName: searchEngine.name,
            entry: "searchbutton",
            source: UrlbarShared.RESULT_SOURCE.SEARCH,
            isPreview: false,
          },
          this.window.gBrowser.selectedBrowser
        );
      }
    } else {
      url = searchEngine.searchForm;
      if (this.#isAddressbar && where == "current") {
        this.inputField.value = url;
        this.selectionStart = -1;
      }
    }
    this._lastSearchString = trimmedValue;

    this.window.openTrustedLinkIn(url, where, { inBackground, postData });
  }

  setHiddenFocus() {
    this._hideFocus = true;
    if (this.focused) {
      this.removeAttribute("focused");
    } else {
      this.focus();
    }
  }

  removeHiddenFocus(forceSuppressFocusBorder = false) {
    this._hideFocus = false;
    if (this.focused) {
      this.toggleAttribute("focused", true);

      if (forceSuppressFocusBorder) {
        this.toggleAttribute("suppress-focus-border", true);
      }
    }
  }

  getSearchMode(browser, confirmedOnly = false) {
    let modes = this.#getSearchModesObject(browser);

    if (!confirmedOnly && modes.preview) {
      return { ...modes.preview };
    }
    if (modes.confirmed) {
      return { ...modes.confirmed };
    }
    return null;
  }

  async setSearchMode(searchMode, browser) {
    let currentSearchMode = this.getSearchMode(browser);
    let areSearchModesSame =
      (!currentSearchMode && !searchMode) ||
      lazy.ObjectUtils.deepEqual(currentSearchMode, searchMode);

    let engine;
    if (searchMode?.engineName) {
      if (!lazy.SearchService.isInitialized) {
        await lazy.SearchService.init();
      }
      engine = lazy.SearchService.getEngineByName(searchMode.engineName);
      if (!engine || engine.hidden) {
        searchMode = null;
      }
    }

    let {
      engineName,
      source,
      entry,
      restrictType,
      isPreview = true,
    } = searchMode || {};

    searchMode = null;

    if (engineName) {
      searchMode = {
        engineName,
        isGeneralPurposeEngine: engine.isGeneralPurposeEngine,
      };
      if (source) {
        searchMode.source = source;
      } else if (searchMode.isGeneralPurposeEngine) {
        searchMode.source = UrlbarShared.RESULT_SOURCE.SEARCH;
      }
    } else if (source) {
      let sourceName = lazy.UrlbarUtils.getResultSourceName(source);
      if (sourceName) {
        searchMode = { source };
      } else {
        console.error(`Unrecognized source: ${source}`);
      }
    }

    let modes = this.#getSearchModesObject(browser);

    if (searchMode) {
      searchMode.isPreview = isPreview;
      if (lazy.UrlbarUtils.SEARCH_MODE_ENTRY.has(entry)) {
        searchMode.entry = entry;
      } else {
        searchMode.entry = "other";
      }

      if (!searchMode.isPreview) {
        modes.confirmed = searchMode;
        delete modes.preview;
      } else {
        modes.preview = searchMode;
      }
    } else {
      delete modes.preview;
      delete modes.confirmed;
    }

    if (restrictType) {
      searchMode.restrictType = restrictType;
    }

    if (browser == this.window.gBrowser.selectedBrowser) {
      this._updateSearchModeUI(searchMode);
      if (searchMode) {
        this.userTypedValue = this.untrimmedValue;
        this.valueIsTyped = true;
        if (
          !AppConstants.MOZ_MINIMAL_BROWSER &&
          !searchMode.isPreview &&
          !areSearchModesSame
        ) {
          try {
          } catch (ex) {
            console.error(ex);
          }
        }
      }
    }
    lazy.UrlbarSearchTermsPersistence.onSearchModeChanged(this.window);
    this.dispatchEvent(new Event("searchmodechanged"));
  }


  #searchbarSearchModes;

  #getSearchModesObject(browser) {
    if (!this.#isAddressbar) {
      this.#searchbarSearchModes ??= {};
      return this.#searchbarSearchModes;
    }

    let state = this.getBrowserState(browser);
    state.searchModes ??= {};
    return state.searchModes;
  }

  restoreSearchModeState() {
    this.searchMode = this.#getSearchModesObject(
      this.window.gBrowser.selectedBrowser
    ).confirmed;
  }

  searchModeShortcut() {
    this.searchMode = {
      source: UrlbarShared.RESULT_SOURCE.SEARCH,
      engineName: lazy.UrlbarSearchUtils.getDefaultEngine(this.isPrivate)?.name,
      entry: "shortcut",
    };
    this.search(this.value);
    this.select();
  }

  confirmSearchMode() {
    let searchMode = this.searchMode;
    if (searchMode?.isPreview) {
      searchMode.isPreview = false;
      this.searchMode = searchMode;

      if (this.view.oneOffSearchButtons) {
        this.view.oneOffSearchButtons.selectedButton = null;
      }
    }
  }


  get editor() {
    return this.inputField.editor;
  }

  get focused() {
    return this.document.activeElement == this.inputField;
  }

  get goButton() {
    return this.querySelector(".urlbar-go-button");
  }

  get value() {
    return this.inputField.value;
  }

  set value(val) {
    this._setValue(val, { allowTrim: true });
  }

  get untrimmedValue() {
    return this._untrimmedValue;
  }

  get userTypedValue() {
    return this.#isAddressbar
      ? this.window.gBrowser.userTypedValue
      : this._userTypedValue;
  }

  set userTypedValue(val) {
    if (this.#isAddressbar) {
      this.window.gBrowser.userTypedValue = val;
    } else {
      this._userTypedValue = val;
    }
  }

  get lastSearchString() {
    return this._lastSearchString;
  }

  get searchMode() {
    if (!this.window.gBrowser) {
      return null;
    }
    return this.getSearchMode(this.window.gBrowser.selectedBrowser);
  }

  set searchMode(searchMode) {
    this.setSearchMode(searchMode, this.window.gBrowser.selectedBrowser);
  }

  getBrowserState(browser) {
    let state = this.#browserStates.get(browser);
    if (!state) {
      state = {};
      this.#browserStates.set(browser, state);
    }
    return state;
  }

  async #updateLayoutBreakout() {
    if (!this.#allowBreakout) {
      return;
    }
    if (this.document.fullscreenElement) {
      this.window.addEventListener(
        "fullscreen",
        () => {
          this.#updateLayoutBreakout();
        },
        { once: true }
      );
      return;
    }
    await this.#updateLayoutBreakoutDimensions();
  }

  startLayoutExtend() {
    if (!this.#allowBreakout || this.hasAttribute("breakout-extend")) {
      return;
    }

    if (!this.view.isOpen) {
      return;
    }

    this.toggleAttribute("breakout-extend", true);
    this.showPopover();
    this.#updateTextboxPosition();

    if (!this.hasAttribute("breakout-extend-animate")) {
      this.window.promiseDocumentFlushed(() => {
        this.window.requestAnimationFrame(() => {
          this.toggleAttribute("breakout-extend-animate", true);
        });
      });
    }
  }

  endLayoutExtend() {
    if (!this.hasAttribute("breakout-extend")) {
      return;
    }

    if (this.view.isOpen && this.view.visibleRowCount) {
      return;
    }

    this.hidePopover();
    this.toggleAttribute("breakout-extend", false);
    this.#updateTextboxPosition();
  }

  updateLayoutExtend() {
    if (this.view.isOpen) {
      this.startLayoutExtend();
    } else {
      this.endLayoutExtend();
    }
  }

  setPageProxyState(
    state,
    updatePopupNotifications,
    forceUnifiedSearchButtonAvailable = false
  ) {
    let prevState = this.getAttribute("pageproxystate");

    this.setAttribute("pageproxystate", state);
    this._inputContainer.setAttribute("pageproxystate", state);
    this._identityBox?.setAttribute("pageproxystate", state);
    this.setUnifiedSearchButtonAvailability(
      forceUnifiedSearchButtonAvailable || state == "invalid"
    );

    if (state == "valid") {
      this._lastValidURLStr = this.value;
    }

    if (
      updatePopupNotifications &&
      prevState != state &&
      this.window.UpdatePopupNotificationsVisibility
    ) {
      this.window.UpdatePopupNotificationsVisibility();
    }
  }

  afterTabSwitchFocusChange() {
    this._gotFocusChange = true;
    this._afterTabSelectAndFocusChange();
  }

  maybeConfirmSearchModeFromResult({
    entry,
    result = this._resultForCurrentValue,
    checkValue = true,
    startQuery = true,
  }) {
    if (
      !result ||
      (checkValue &&
        this.value.trim() != result.payload.keyword?.trim() &&
        this.value.trim() != result.payload.autofillKeyword?.trim())
    ) {
      return false;
    }

    let searchMode = this._searchModeForResult(result, entry);
    if (!searchMode) {
      return false;
    }

    this.searchMode = searchMode;

    let value = result.payload.query?.trimStart() || "";
    this._setValue(value);

    if (startQuery) {
      this.startQuery({ allowAutofill: false });
    }

    return true;
  }

  observe(subject, topic, data) {
    switch (topic) {
      case lazy.SearchUtils.TOPIC_ENGINE_MODIFIED: {
        let engine = subject.wrappedJSObject;
        switch (data) {
          case lazy.SearchUtils.MODIFIED_TYPE.CHANGED:
          case lazy.SearchUtils.MODIFIED_TYPE.REMOVED: {
            let searchMode = this.searchMode;
            if (searchMode?.engineName == engine.name) {
              this.searchMode = searchMode;
            }
            break;
          }
          case lazy.SearchUtils.MODIFIED_TYPE.DEFAULT:
            if (!this.isPrivate) {
              this._updatePlaceholder(engine.name);
              this._resultForCurrentValue = null;
            }
            break;
          case lazy.SearchUtils.MODIFIED_TYPE.DEFAULT_PRIVATE:
            if (this.isPrivate) {
              this._updatePlaceholder(engine.name);
              this._resultForCurrentValue = null;
            }
            break;
        }
        break;
      }
    }
  }



  #providesSearchMode(result) {
    if (!result) {
      return false;
    }
    if (
      this.view.selectedElement &&
      result.providerName == "UrlbarProviderGlobalActions"
    ) {
      return this.view.selectedElement.dataset.providesSearchmode == "true";
    }
    return result.payload.providesSearchMode;
  }

  _addObservers() {
    this._observer ??= {
      observe: this.observe.bind(this),
      QueryInterface: ChromeUtils.generateQI([
        "nsIObserver",
        "nsISupportsWeakReference",
      ]),
    };
    Services.obs.addObserver(
      this._observer,
      lazy.SearchUtils.TOPIC_ENGINE_MODIFIED,
      true
    );
  }

  _removeObservers() {
    if (this._observer) {
      Services.obs.removeObserver(
        this._observer,
        lazy.SearchUtils.TOPIC_ENGINE_MODIFIED
      );
      this._observer = null;
    }
  }

  _getURIFixupInfo(searchString) {
    let flags =
      Ci.nsIURIFixup.FIXUP_FLAG_FIX_SCHEME_TYPOS |
      Ci.nsIURIFixup.FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP;
    if (this.isPrivate) {
      flags |= Ci.nsIURIFixup.FIXUP_FLAG_PRIVATE_CONTEXT;
    }
    try {
      return Services.uriFixup.getFixupURIInfo(searchString, flags);
    } catch (ex) {
      console.error(
        `An error occured while trying to fixup "${searchString}"`,
        ex
      );
    }
    return null;
  }

  _afterTabSelectAndFocusChange() {
    if (!this._gotFocusChange || !this._gotTabSelect) {
      return;
    }
    this._gotFocusChange = this._gotTabSelect = false;

    this.formatValue();
    this._resetSearchState();

    const event = new CustomEvent("tabswitch");

    if (this.view.autoOpen({ event })) {
      return;
    }
    this.searchModeSwitcher.closePanel();
    this.view.close();
  }

  #updateTextboxPosition() {
    if (!this.hasAttribute("breakout-extend")) {
      this.style.top = "";
      return;
    }

    this.style.top = px(
      this.parentNode.getBoxQuads({
        ignoreTransforms: true,
        flush: false,
      })[0].p1.y
    );
  }

  #updateTextboxPositionNextFrame() {
    if (!this.hasAttribute("breakout")) {
      return;
    }
    this.window.requestAnimationFrame(() => {
      this.window.requestAnimationFrame(() => {
        this.#updateTextboxPosition();
      });
    });
  }

  #stopBreakout() {
    this.removeAttribute("breakout");
    this.parentNode.removeAttribute("breakout");
    this.style.top = "";
    this._layoutBreakoutUpdateKey = {};
  }

  incrementBreakoutBlockerCount() {
    this.#breakoutBlockerCount++;
    if (this.#breakoutBlockerCount == 1) {
      this.#stopBreakout();
    }
  }

  decrementBreakoutBlockerCount() {
    if (this.#breakoutBlockerCount > 0) {
      this.#breakoutBlockerCount--;
    }
    if (this.#breakoutBlockerCount === 0) {
      this.#updateLayoutBreakout();
    }
  }

  async #updateLayoutBreakoutDimensions() {
    this.#stopBreakout();

    let updateKey = {};
    this._layoutBreakoutUpdateKey = updateKey;
    await this.window.promiseDocumentFlushed(() => {});
    await new Promise(resolve => {
      this.window.requestAnimationFrame(() => {
        if (this._layoutBreakoutUpdateKey != updateKey || !this.isConnected) {
          return;
        }

        this.parentNode.style.setProperty(
          "--urlbar-container-height",
          px(getBoundsWithoutFlushing(this.parentNode).height)
        );
        this.style.setProperty(
          "--urlbar-height",
          px(getBoundsWithoutFlushing(this).height)
        );

        if (this.#breakoutBlockerCount) {
          return;
        }

        this.setAttribute("breakout", "true");
        this.parentNode.setAttribute("breakout", "true");
        this.#updateTextboxPosition();

        resolve();
      });
    });
  }

  _setValue(
    val,
    {
      allowTrim = false,
      untrimmedValue = null,
      valueIsTyped = false,
      actionType = undefined,
    } = {}
  ) {
    this._untrimmedValue = untrimmedValue ?? val;
    this._protocolIsTrimmed = false;
    if (allowTrim) {
      let oldVal = val;
      val = this._trimValue(val);
      this._protocolIsTrimmed =
        oldVal.startsWith(lazy.BrowserUIUtils.trimURLProtocol) &&
        !val.startsWith(lazy.BrowserUIUtils.trimURLProtocol);
    }

    this.valueIsTyped = valueIsTyped;
    this._resultForCurrentValue = null;
    this.inputField.value = val;
    this.formatValue();

    if (actionType !== undefined) {
      this.setAttribute("actiontype", actionType);
    } else {
      this.removeAttribute("actiontype");
    }

    let event = this.document.createEvent("Events");
    event.initEvent("ValueChange", true, true);
    this.inputField.dispatchEvent(event);

    return val;
  }

  #getValueFromResult(result, { urlOverride = null, element = null } = {}) {
    switch (result.type) {
      case UrlbarShared.RESULT_TYPE.KEYWORD:
        return result.payload.input;
      case UrlbarShared.RESULT_TYPE.SEARCH: {
        let value = "";
        if (result.payload.keyword) {
          value += result.payload.keyword + " ";
        }
        value += result.payload.query;
        return value;
      }
      case UrlbarShared.RESULT_TYPE.DYNAMIC:
        return (
          element?.dataset.query ||
          element?.dataset.url ||
          result.payload.input ||
          result.payload.query ||
          ""
        );
      case UrlbarShared.RESULT_TYPE.RESTRICT:
        return result.payload.autofillKeyword + " ";
      case UrlbarShared.RESULT_TYPE.TIP: {
        let value = element?.dataset.url || element?.dataset.input;
        if (value) {
          return value;
        }
        break;
      }
    }

    if (urlOverride !== null) {
      let url = URL.parse(urlOverride);
      return url ? losslessDecodeURI(url.URI) : "";
    }

    let parsedUrl = URL.parse(result.payload.url);
    if (!parsedUrl) {
      return "";
    }

    let url = losslessDecodeURI(parsedUrl.URI);
    let stripHttp =
      result.heuristic &&
      result.payload.url.startsWith("http://") &&
      this.userTypedValue &&
      this.#getSchemelessInput(this.userTypedValue) ==
        Ci.nsILoadInfo.SchemelessInputTypeSchemeless;
    if (!stripHttp) {
      return url;
    }
    let trimmedUrl = lazy.UrlbarUtils.stripPrefixAndTrim(url, { stripHttp })[0];
    let isSearch = !!this._getURIFixupInfo(trimmedUrl)?.keywordAsSent;
    if (isSearch) {
      return url;
    }
    return trimmedUrl;
  }

  #getActionTypeFromResult(result) {
    switch (result.type) {
      case UrlbarShared.RESULT_TYPE.TAB_SWITCH:
        return "switchtab";
      default:
        return undefined;
    }
  }

  _resetSearchState() {
    this._lastSearchString = this.value;
    this._autofillPlaceholder = null;
  }

  _maybeAutofillPlaceholder(value) {
    let allowAutofill =
      this.selectionEnd == value.length &&
      !this.searchMode?.engineName &&
      this.searchMode?.source != UrlbarShared.RESULT_SOURCE.SEARCH;

    if (!allowAutofill) {
      this.#clearAutofill();
      return false;
    }

    let canAutofillPlaceholder = false;
    if (this._autofillPlaceholder) {
      if (
        this._autofillPlaceholder.type == "adaptive_url" ||
        this._autofillPlaceholder.type == "adaptive_origin"
      ) {
        canAutofillPlaceholder =
          value.length >=
            this._autofillPlaceholder.adaptiveHistoryInput.length &&
          this._autofillPlaceholder.value
            .toLocaleLowerCase()
            .startsWith(value.toLocaleLowerCase());
      } else {
        canAutofillPlaceholder = lazy.UrlbarUtils.canAutofillURL(
          this._autofillPlaceholder.value,
          value
        );
      }
    }

    if (!canAutofillPlaceholder) {
      this._autofillPlaceholder = null;
    } else if (
      this._autofillPlaceholder &&
      this.selectionEnd == this.value.length &&
      this._enableAutofillPlaceholder
    ) {
      let autofillValue =
        value + this._autofillPlaceholder.value.substring(value.length);
      this._autofillValue({
        value: autofillValue,
        selectionStart: value.length,
        selectionEnd: autofillValue.length,
        type: this._autofillPlaceholder.type,
        adaptiveHistoryInput: this._autofillPlaceholder.adaptiveHistoryInput,
        untrimmedValue: this._autofillPlaceholder.untrimmedValue,
      });
    }

    return true;
  }

  updateTextOverflow() {
    if (!this.#isAddressbar) {
      return;
    }

    if (!this._overflowing) {
      this.removeAttribute("textoverflow");
      return;
    }

    let isRTL =
      this.getAttribute("domaindir") === "rtl" &&
      lazy.UrlbarUtils.isTextDirectionRTL(this.value, this.window);

    this.window.promiseDocumentFlushed(() => {
      let input = this.inputField;
      if (input && this._overflowing) {
        let side = "both";
        if (isRTL) {
          if (input.scrollLeft == 0) {
            side = "left";
          } else if (input.scrollLeft == input.scrollLeftMin) {
            side = "right";
          }
        } else if (input.scrollLeft == 0) {
          side = "right";
        } else if (input.scrollLeft == input.scrollLeftMax) {
          side = "left";
        }

        this.window.requestAnimationFrame(() => {
          if (this._overflowing) {
            this.setAttribute("textoverflow", side);
          }
        });
      }
    });
  }

  get inOverflowPanel() {
    if (!this.parentElement?.id) {
      return false;
    }
    return (
      lazy.CustomizableUI.getPlacementOfWidget(this.parentElement.id)?.area ==
        lazy.CustomizableUI.AREA_FIXED_OVERFLOW_PANEL ||
      this.parentElement.getAttribute("overflowedItem") == "true"
    );
  }

  _updateUrlTooltip() {
    if (this.focused || !this._overflowing) {
      this.inputField.removeAttribute("title");
    } else {
      this.inputField.setAttribute("title", this.untrimmedValue);
    }
  }

  _getSelectedValueForClipboard() {
    let selectedVal = this.#selectedText;

    if (this.editor.selection.rangeCount > 1) {
      return selectedVal;
    }

    if (
      this.selectionStart > 0 ||
      selectedVal == "" ||
      (this.valueIsTyped && !this._protocolIsTrimmed)
    ) {
      return selectedVal;
    }

    if (!selectedVal.includes("/")) {
      let remainder = this.value.replace(selectedVal, "");
      if (remainder != "" && remainder[0] != "/") {
        return selectedVal;
      }
    }

    let uri;
    if (this.getAttribute("pageproxystate") == "valid") {
      uri = this.#isOpenedPageInBlankTargetLoading
        ? this.window.gBrowser.selectedBrowser.browsingContext
            .nonWebControlledLoadingURI
        : this.window.gBrowser.currentURI;
    } else {

      let result = this._resultForCurrentValue;

      if (result?.autofill?.value == selectedVal) {
        return result.payload.url;
      }

      uri = URL.parse(this._untrimmedValue)?.URI;
      if (!uri) {
        return selectedVal;
      }
    }
    uri = this.makeURIReadable(uri);
    let displaySpec = uri.displaySpec;

    if (
      this.value == selectedVal &&
      !uri.schemeIs("javascript") &&
      !uri.schemeIs("data") &&
      !lazy.UrlbarPrefs.get("decodeURLsOnCopy")
    ) {
      return displaySpec;
    }


    if (
      !selectedVal.startsWith(lazy.BrowserUIUtils.trimURLProtocol) &&
      !displaySpec.startsWith(this._trimValue(displaySpec))
    ) {
      selectedVal = lazy.BrowserUIUtils.trimURLProtocol + selectedVal;
    }

    if (!lazy.UrlbarPrefs.get("decodeURLsOnCopy") && !uri.schemeIs("data")) {
      try {
        if (URL.canParse(selectedVal)) {
          selectedVal = encodeURI(selectedVal);
        }
      } catch (ex) {
      }
    }

    return selectedVal;
  }

  _toggleActionOverride(event) {
    if (
      event.keyCode == KeyEvent.DOM_VK_SHIFT ||
      event.keyCode == KeyEvent.DOM_VK_ALT ||
      event.keyCode ==
        (AppConstants.platform == "macosx"
          ? KeyEvent.DOM_VK_META
          : KeyEvent.DOM_VK_CONTROL)
    ) {
      if (event.type == "keydown") {
        this._actionOverrideKeyCount++;
        this.toggleAttribute("action-override", true);
        this.view.panel.setAttribute("action-override", true);
      } else if (
        this._actionOverrideKeyCount &&
        --this._actionOverrideKeyCount == 0
      ) {
        this._clearActionOverride();
      }
    }
  }

  _clearActionOverride() {
    this._actionOverrideKeyCount = 0;
    this.removeAttribute("action-override");
    this.view.panel.removeAttribute("action-override");
  }


  _trimValue(val) {
    if (!this.#isAddressbar) {
      return val;
    }
    let trimmedValue = lazy.UrlbarPrefs.get("trimURLs")
      ? lazy.BrowserUIUtils.trimURL(val)
      : val;
    return lazy.UrlbarUtils.isTextDirectionRTL(trimmedValue, this.window) ||
      this.#lazy.valueFormatter.willShowFormattedMixedContentProtocol(val)
      ? val
      : trimmedValue;
  }

  #isCanonizeKeyboardEvent(event) {
    if (this.sapName == "searchbar") {
      return false;
    }
    return (
      KeyboardEvent.isInstance(event) &&
      event.keyCode == KeyEvent.DOM_VK_RETURN &&
      (AppConstants.platform == "macosx" ? event.metaKey : event.ctrlKey) &&
      !event._disableCanonization &&
      lazy.UrlbarPrefs.get("ctrlCanonizesURLs")
    );
  }

  _maybeCanonizeURL(event, value) {
    if (
      !this.#isCanonizeKeyboardEvent(event) ||
      !/^\s*[^.:\/\s]+(?:\/.*|\s*)$/i.test(value)
    ) {
      return null;
    }

    let suffix = Services.locale.urlFixupSuffix;
    if (!suffix.endsWith("/")) {
      suffix += "/";
    }

    value = value.trim();

    let firstSlash = value.indexOf("/");
    if (firstSlash >= 0) {
      value =
        value.substring(0, firstSlash) +
        suffix +
        value.substring(firstSlash + 1);
    } else {
      value = value + suffix;
    }

    try {
      const info = Services.uriFixup.getFixupURIInfo(
        value,
        Ci.nsIURIFixup.FIXUP_FLAGS_MAKE_ALTERNATE_URI
      );
      value = info.fixedURI.spec;
    } catch (ex) {
      console.error(`An error occured while trying to fixup "${value}"`, ex);
    }

    this.value = value;
    return value;
  }

  _autofillValue({
    value,
    selectionStart,
    selectionEnd,
    type,
    adaptiveHistoryInput,
    untrimmedValue,
  }) {
    this._setValue(value, { untrimmedValue });
    this.inputField.setSelectionRange(selectionStart, selectionEnd);
    this._autofillPlaceholder = {
      value,
      type,
      adaptiveHistoryInput,
      selectionStart,
      selectionEnd,
      untrimmedValue,
    };
  }

  #pickMenuResult(result, event, element, browser) {

    if (element.dataset.command == "manage") {
      this.window.openPreferences("search-locationBar");
      return;
    }

    let url;
    if (element.dataset.command == "help") {
      url = result.payload.helpUrl;
    }
    url ||= element.dataset.url;

    if (!url) {
      return;
    }

    let where = this._whereToOpen(event);
    if (element.dataset.command == "help" && where == "current") {
      where = "tab";
    }

    this.view.close({ elementPicked: true });

    this._loadURL(
      url,
      event,
      where,
      {
        allowInheritPrincipal: false,
        private: this.isPrivate,
      },
      {
        source: result.source,
        type: result.type,
      },
      browser
    );
  }

  #prepareAddressbarLoad(
    url,
    openUILinkWhere,
    params,
    resultDetails = null,
    browser
  ) {
    if (!this.#isAddressbar) {
      throw new Error(
        "Can't prepare addressbar load when this isn't an addressbar input"
      );
    }

    if (openUILinkWhere == "current") {
      let formattedURL = url;
      try {
        formattedURL = losslessDecodeURI(new URL(url).URI);
      } catch {}

      this.value =
        lazy.UrlbarPrefs.isPersistedSearchTermsEnabled() &&
        resultDetails?.searchTerm
          ? resultDetails.searchTerm
          : formattedURL;
      browser.userTypedValue = this.value;
    }

    if (
      openUILinkWhere != "window" &&
      this.window.gInitialPages.includes(url)
    ) {
      browser.initialPageLoadedFromUserAction = url;
    }

    try {
      lazy.UrlbarUtils.addToUrlbarHistory(url, this.window);
    } catch (ex) {
      console.error(ex);
    }

    if (
      !params.triggeringPrincipal ||
      params.triggeringPrincipal.isSystemPrincipal
    ) {
      delete browser.authPromptAbuseCounter;

      if (
        openUILinkWhere == "current" &&
        browser.currentURI &&
        url === browser.currentURI.spec
      ) {
        this.window.SitePermissions.clearTemporaryBlockPermissions(browser);
      }
    }

    params.initiatedByURLBar = true;
  }

  _loadURL(
    url,
    event,
    openUILinkWhere,
    params,
    resultDetails = null,
    browser = this.window.gBrowser.selectedBrowser,
    keepViewOpen = false
  ) {
    if (this.#isAddressbar) {
      this.#prepareAddressbarLoad(
        url,
        openUILinkWhere,
        params,
        resultDetails,
        browser
      );
    }

    params.allowThirdPartyFixup = true;

    if (openUILinkWhere == "current") {
      params.targetBrowser = browser;
      params.indicateErrorPageLoad = true;
      params.allowPinnedTabHostChange = this.#isAddressbar;
      params.allowPopups = url.startsWith("javascript:");
    } else {
      params.initiatingDoc = this.window.document;
    }

    if (
      this._keyDownEnterDeferred &&
      event?.keyCode === KeyEvent.DOM_VK_RETURN &&
      openUILinkWhere === "current"
    ) {
      params.avoidBrowserFocus = true;
      this._keyDownEnterDeferred.loadedContent = true;
      this._keyDownEnterDeferred.resolve(browser);
    }

    if (this.isPrivate && !("private" in params)) {
      params.private = true;
    }

    if (!params.avoidBrowserFocus) {
      browser.focus();
      this.inputField.setSelectionRange(0, 0);
    }

    if (openUILinkWhere != "current" && this.sapName != "searchbar") {
      this.handleRevert();
    }

    this.#notifyStartNavigation(resultDetails);

    try {
      this.window.openTrustedLinkIn(url, openUILinkWhere, params);
    } catch (ex) {
      if (ex.result != Cr.NS_ERROR_LOAD_SHOWED_ERRORPAGE) {
        this.handleRevert();
      }
    }

    if (!keepViewOpen) {
      this.view.close({ showFocusBorder: false });
    }
  }

  _whereToOpen(event) {
    let isKeyboardEvent = KeyboardEvent.isInstance(event);
    let reuseEmpty = isKeyboardEvent;
    let where = undefined;
    if (
      isKeyboardEvent &&
      (event.altKey || event.getModifierState("AltGraph"))
    ) {
      where = event.shiftKey ? "tabshifted" : "tab";
    } else if (this.#isCanonizeKeyboardEvent(event)) {
      where = "current";
    } else {
      where = lazy.BrowserUtils.whereToOpenLink(event, false, false);
    }
    let openInTabPref =
      this.#sapName == "searchbar"
        ? lazy.UrlbarPrefs.get("browser.search.openintab")
        : lazy.UrlbarPrefs.get("openintab");
    if (openInTabPref) {
      if (where == "current") {
        where = "tab";
      } else if (where == "tab") {
        where = "current";
      }
      reuseEmpty = true;
    }
    if (
      where == "tab" &&
      reuseEmpty &&
      this.window.gBrowser.selectedTab.isEmpty
    ) {
      where = "current";
    }
    return where;
  }

  _initCopyCutController() {
    if (this._copyCutController) {
      return;
    }
    this._copyCutController = new CopyCutController(this);
    this.inputField.controllers.insertControllerAt(0, this._copyCutController);
  }

  #findMenuItemLocation(menuItemCommand) {
    let inputBox = this.querySelector("moz-input-box");
    let contextMenu = inputBox.menupopup;
    let insertLocation = contextMenu.firstElementChild;
    while (
      insertLocation.nextElementSibling &&
      insertLocation.getAttribute("cmd") != menuItemCommand
    ) {
      insertLocation = insertLocation.nextElementSibling;
    }

    return insertLocation;
  }

  #maybeUntrimUrl({ moveCursorToStart = false, ignoreSelection = false } = {}) {
    if (
      !lazy.UrlbarPrefs.get(
        "untrimOnUserInteraction.featureGate"
      ) ||
      !this._protocolIsTrimmed ||
      !this.focused ||
      (!ignoreSelection && this.#allTextSelected)
    ) {
      return;
    }

    let selectionStart = this.selectionStart;
    let selectionEnd = this.selectionEnd;

    let offset = lazy.BrowserUIUtils.trimURLProtocol.length;

    if (this._autofillPlaceholder) {
      this._autofillPlaceholder.selectionStart += offset;
      this._autofillPlaceholder.selectionEnd += offset;
    }

    if (moveCursorToStart) {
      this._setValue(this._untrimmedValue, {
        valueIsTyped: this.valueIsTyped,
      });
      this.setSelectionRange(0, 0);
      return;
    }

    if (selectionStart == selectionEnd) {
      if (selectionEnd == this.value.length) {
        offset += 1;
      }
      selectionStart = selectionEnd += offset;
    } else {
      if (selectionStart != 0) {
        selectionStart += offset;
      } else {
        let prePathMinusPort;
        try {
          let uri = Services.io.newURI(this._untrimmedValue);
          prePathMinusPort = [uri.userPass, uri.displayHost]
            .filter(Boolean)
            .join("@");
        } catch (ex) {
          lazy.logger.error("Should only try to untrim valid URLs");
        }
        if (!this.#selectedText.startsWith(prePathMinusPort)) {
          selectionStart += offset;
        }
      }
      if (selectionEnd == this.value.length) {
        offset += 1;
      }
      selectionEnd += offset;
    }

    this._setValue(this._untrimmedValue, {
      valueIsTyped: this.valueIsTyped,
    });

    this.setSelectionRange(selectionStart, selectionEnd);
  }

  _initPasteAndGo() {
    let insertLocation = this.#findMenuItemLocation("cmd_paste");
    if (!insertLocation) {
      return;
    }

    let pasteAndGo = this.document.createXULElement("menuitem");
    pasteAndGo.id = "paste-and-go";
    let label = Services.strings
      .createBundle("chrome://browser/locale/browser.properties")
      .GetStringFromName("pasteAndGo.label");
    pasteAndGo.setAttribute("label", label);
    pasteAndGo.setAttribute("anonid", "paste-and-go");
    pasteAndGo.addEventListener("command", () => {
      this._suppressStartQuery = true;

      this.select();
      this.window.goDoCommand("cmd_paste");
      this.setResultForCurrentValue(null);
      this.handleCommand();
      this.controller.clearLastQueryContextCache();

      this._suppressStartQuery = false;
    });

    this.#addContextMenuListener(() => {
      this.view.close();
      this.inputField.focus();

      let controller =
        this.document.commandDispatcher.getControllerForCommand("cmd_paste");
      let enabled = controller.isCommandEnabled("cmd_paste");
      if (enabled) {
        pasteAndGo.removeAttribute("disabled");
      } else {
        pasteAndGo.setAttribute("disabled", "true");
      }
    });

    insertLocation.insertAdjacentElement("afterend", pasteAndGo);
  }

  _initAutofillDismiss() {
    let contextMenu = this.querySelector("moz-input-box").menupopup;
    let insertLocation = this.#findMenuItemLocation("cmd_selectAll");
    if (!insertLocation) {
      return;
    }

    let separator = this.document.createXULElement("menuseparator");
    separator.setAttribute("anonid", "urlbar-input-autofill-dismiss-separator");

    let dismiss = this.document.createXULElement("menuitem");
    dismiss.setAttribute("anonid", "urlbar-input-dismiss-autofill");
    this.document.l10n.setAttributes(dismiss, "urlbar-input-dismiss-autofill");
    dismiss.addEventListener("command", () => {
      this.#dismissAdaptiveAutofillFromContextMenu("dismiss");
    });

    let forget = this.document.createXULElement("menuitem");
    forget.setAttribute("anonid", "urlbar-input-remove-from-history");
    this.document.l10n.setAttributes(
      forget,
      "urlbar-input-remove-from-history"
    );
    forget.addEventListener("command", () => {
      this.#dismissAdaptiveAutofillFromContextMenu("forget");
    });

    insertLocation.insertAdjacentElement("afterend", separator);
    separator.insertAdjacentElement("afterend", dismiss);
    dismiss.insertAdjacentElement("afterend", forget);

    contextMenu.addEventListener("popupshowing", () => {
      let { showDismiss, showForget } =
        this.#autofillDismissContextMenuVisibility();
      separator.hidden = !showDismiss && !showForget;
      dismiss.hidden = !showDismiss;
      forget.hidden = !showForget;
    });
  }

  #autofillDismissContextMenuVisibility() {
    let hidden = { showDismiss: false, showForget: false };

    if (!lazy.UrlbarPrefs.get("autoFill.adaptiveHistory.enabled")) {
      return hidden;
    }

    let result = this._resultForCurrentValue;
    if (!result?.heuristic || !result.autofill) {
      return hidden;
    }

    let type = result.autofill.type;
    if (
      type !== "adaptive_url" &&
      type !== "adaptive_origin" &&
      type !== "origin"
    ) {
      return hidden;
    }

    let isOrigin = lazy.UrlbarUtils.isOriginUrl(result.payload.url);
    return {
      showDismiss: !this.isPrivate,
      showForget: !isOrigin,
    };
  }

  async #dismissAdaptiveAutofillFromContextMenu(action) {
    let result = this._resultForCurrentValue;
    if (!result?.heuristic || !result.autofill) {
      return;
    }


    let { url } = result.payload;
    if (action === "forget") {
      await lazy.PlacesUtils.history.remove(url).catch(console.error);
    } else {
      let blockUntilMs =
        Date.now() + lazy.UrlbarPrefs.get("autoFill.dismissalBlockDurationMs");
      await lazy.UrlbarUtils.blockAutofill(url, blockUntilMs).catch(
        console.error
      );
    }

    lazy.UrlbarUtils.clearAutofillBackspaceEntryForUrl(url);

    this._setValue(this._lastSearchString);
    this.startQuery({
      searchString: this._lastSearchString,
      allowAutofill: false,
      resetSearchState: false,
    });
  }

  #notifyStartNavigation(result) {
    if (this.#isAddressbar) {
      Services.obs.notifyObservers({ result }, "urlbar-user-start-navigation");
    }
  }

  _searchModeForResult(result, entry = null) {
    if (
      !result.payload.keyword &&
      !result.payload.engine &&
      !this.view.selectedElement.dataset?.engine
    ) {
      return null;
    }

    let searchMode = this.searchModeForToken(result.payload.keyword);
    if (
      !searchMode &&
      result.payload.engine &&
      (!result.payload.originalEngine ||
        result.payload.engine == result.payload.originalEngine)
    ) {
      searchMode = { engineName: result.payload.engine };
    } else if (this.view.selectedElement?.dataset.engine) {
      searchMode = { engineName: this.view.selectedElement.dataset.engine };
    }

    if (searchMode) {
      if (result.type == UrlbarShared.RESULT_TYPE.RESTRICT) {
        searchMode.restrictType = "keyword";
      } else if (
        UrlbarShared.SEARCH_MODE_RESTRICT.has(result.payload.keyword)
      ) {
        searchMode.restrictType = "symbol";
      }
      if (entry) {
        searchMode.entry = entry;
      } else {
        searchMode.entry = "keywordoffer";
      }
    }

    return searchMode;
  }

  _updateSearchModeUI(searchMode) {
    let { engineName, source, isGeneralPurposeEngine } = searchMode || {};

    if (!engineName && !source && !this.hasAttribute("searchmode")) {
      return;
    }

    if (this._searchModeIndicatorTitle) {
      this._searchModeIndicatorTitle.textContent = "";
      this._searchModeIndicatorTitle.removeAttribute("data-l10n-id");
    }

    if (!engineName && !source) {
      this.removeAttribute("searchmode");
      this.initPlaceHolder(true);
      return;
    }

    if (this.#isAddressbar) {
      if (engineName) {
        this._searchModeIndicatorTitle.textContent = engineName;
        this.document.l10n.setAttributes(
          this.inputField,
          isGeneralPurposeEngine
            ? "urlbar-placeholder-search-mode-web-2"
            : "urlbar-placeholder-search-mode-other-engine",
          { name: engineName }
        );
      } else if (source) {
        const messageIDs = {
          actions: "urlbar-placeholder-search-mode-other-actions",
          bookmarks: "urlbar-placeholder-search-mode-other-bookmarks",
          engine: "urlbar-placeholder-search-mode-other-engine",
          history: "urlbar-placeholder-search-mode-other-history",
          tabs: "urlbar-placeholder-search-mode-other-tabs",
        };
        let sourceName = lazy.UrlbarUtils.getResultSourceName(source);
        let l10nID = `urlbar-search-mode-${sourceName}`;
        this.document.l10n.setAttributes(
          this._searchModeIndicatorTitle,
          l10nID
        );
        this.document.l10n.setAttributes(
          this.inputField,
          messageIDs[sourceName]
        );
      }
    }

    this.toggleAttribute("searchmode", true);
    if (this._autofillPlaceholder && this.userTypedValue) {
      this.value = this.userTypedValue;
    }
    if (this.getAttribute("pageproxystate") == "valid") {
      this.value = "";
      this.setPageProxyState("invalid", true);
    }

    lazy.UrlbarSearchTermsPersistence.onSearchModeChanged(this.window);
    this.dispatchEvent(new Event("searchmodechanged"));
  }

  #handlePersistedSearchTerms({
    state,
    hideSearchTerms,
    dueToTabSwitch,
    isSameDocument,
    uri,
  }) {
    if (!lazy.UrlbarPrefs.isPersistedSearchTermsEnabled()) {
      if (state.persist) {
        this.removeAttribute("persistsearchterms");
        delete state.persist;
      }
      return false;
    }

    let firstView = (!isSameDocument && !dueToTabSwitch) || !state.persist;

    let cachedUriDidChange =
      state.persist?.originalURI &&
      (!this.window.gBrowser.selectedBrowser.originalURI ||
        !state.persist.originalURI.equals(
          this.window.gBrowser.selectedBrowser.originalURI
        ));

    let wasPersisting = state.persist?.shouldPersist ?? false;

    if (firstView || cachedUriDidChange) {
      lazy.UrlbarSearchTermsPersistence.setPersistenceState(
        state,
        this.window.gBrowser.selectedBrowser.originalURI
      );
    }
    let shouldPersist =
      !hideSearchTerms &&
      lazy.UrlbarSearchTermsPersistence.shouldPersist(state, {
        dueToTabSwitch,
        isSameDocument,
        uri: uri ?? this.window.gBrowser.currentURI,
        userTypedValue: this.userTypedValue,
        firstView,
      });
    if (shouldPersist) {
      this.userTypedValue = state.persist.searchTerms;
    } else if (wasPersisting && !shouldPersist) {
      this.userTypedValue = null;
    }

    state.persist.shouldPersist = shouldPersist;
    this.toggleAttribute("persistsearchterms", state.persist.shouldPersist);


    return shouldPersist;
  }

  initPlaceHolder(force = false) {
    if (!this.#isAddressbar) {
      return;
    }

    let prefName =
      "browser.urlbar.placeholderName" + (this.isPrivate ? ".private" : "");
    let engineName = Services.prefs.getStringPref(prefName, "");
    if (engineName || force) {
      this._setPlaceholder(engineName || null);
    }
  }

  async delayedStartupInit() {
    if (!this.value) {
      let updateListener = () => {
        if (this.value && !this.searchMode) {
          this.searchModeSwitcher.updateSearchIcon();
          this._updatePlaceholderFromDefaultEngine();
          this.inputField.removeEventListener("input", updateListener);
          this.window.gBrowser.tabContainer.removeEventListener(
            "TabSelect",
            updateListener
          );
        }
      };

      this.inputField.addEventListener("input", updateListener);
      this.window.gBrowser.tabContainer.addEventListener(
        "TabSelect",
        updateListener
      );
    } else {
      await this._updatePlaceholderFromDefaultEngine();
    }

    if (this.#isAddressbar) {
      lazy.SearchUIUtils.updatePlaceholderNamePreference(
        await this._getDefaultSearchEngine(),
        this.isPrivate
      );
    }
  }

  setUnifiedSearchButtonAvailability(available) {
    available ||= lazy.UrlbarPrefs.get("unifiedSearchButton.always");
    const switcher = this.querySelector(".searchmode-switcher");
    switcher.toggleAttribute("offscreen", !available);
    if (available) {
      switcher.removeAttribute("aria-hidden");
    } else {
      switcher.setAttribute("aria-hidden", "true");
    }
    this.getBrowserState(
      this.window.gBrowser.selectedBrowser
    ).isUnifiedSearchButtonAvailable = available;
  }

  _getDefaultSearchEngine() {
    return this.isPrivate
      ? lazy.SearchService.getDefaultPrivate()
      : lazy.SearchService.getDefault();
  }

  async _updatePlaceholderFromDefaultEngine() {
    const defaultEngine = await this._getDefaultSearchEngine();
    this._updatePlaceholder(defaultEngine.name);
  }

  _updatePlaceholder(engineName) {
    if (!engineName) {
      throw new Error("Expected an engineName to be specified");
    }

    if (this.searchMode || !this.#isAddressbar) {
      return;
    }

    let engine = lazy.SearchService.getEngineByName(engineName);
    if (engine instanceof lazy.ConfigSearchEngine) {
      this._setPlaceholder(engineName);
    } else {
      this._setPlaceholder(null);
    }
  }

  _setPlaceholder(engineName) {
    if (!this.#isAddressbar) {
      this.document.l10n.setAttributes(this.inputField, "searchbar-input");
      return;
    }

    let l10nId;
    if (lazy.UrlbarPrefs.get("keyword.enabled")) {
      l10nId = engineName
        ? "urlbar-placeholder-with-name"
        : "urlbar-placeholder";
    } else {
      l10nId = "urlbar-placeholder-keyword-disabled";
    }

    this.document.l10n.setAttributes(
      this.inputField,
      l10nId,
      l10nId == "urlbar-placeholder-with-name"
        ? { name: engineName }
        : undefined
    );
  }

  #preventClickSelectsAll = false;

  #maybeSelectAll() {
    if (
      !this.#preventClickSelectsAll &&
      this.#compositionState != lazy.UrlbarUtils.COMPOSITION.COMPOSING &&
      this.focused &&
      this.inputField.selectionStart == this.inputField.selectionEnd
    ) {
      this.select();
    }
  }


  _on_command(event) {
  }

  _on_blur(event) {
    if (this.view.resultMenu.hasAttribute("open")) {
      return;
    }

    lazy.logger.debug("Blur Event");

    this.focusedViaMousedown = false;
    this._handoffSession = undefined;
    this._isHandoffSession = false;
    this.removeAttribute("focused");
    this.#preventClickSelectsAll = false;

    if (this._autofillPlaceholder && this.userTypedValue) {
      this.value = this.userTypedValue;
    } else if (
      this.value == this._untrimmedValue &&
      !this.userTypedValue &&
      !this.focused
    ) {
      this.value = this._untrimmedValue;
    } else {
      this.formatValue();
    }

    this._resetSearchState();

    this._clearActionOverride();

    if (!lazy.UrlbarPrefs.get("ui.popup.disable_autohide")) {
      this.view.close();
    }

    if (
      this.getAttribute("pageproxystate") != "valid" &&
      this.window.UpdatePopupNotificationsVisibility
    ) {
      this.window.UpdatePopupNotificationsVisibility();
    }

    if (this._keyDownEnterDeferred) {
      this._keyDownEnterDeferred.resolve();
      this._keyDownEnterDeferred = null;
    }
    this._isKeyDownWithCtrl = false;
    this._isKeyDownWithMeta = false;
    this._isKeyDownWithMetaAndLeft = false;

    Services.obs.notifyObservers(null, "urlbar-blur");
  }

  _on_click(event) {
    switch (event.target) {
      case this.inputField:
      case this._inputContainer:
        this.#maybeSelectAll();
        this.#maybeUntrimUrl();
        break;

      case this._searchModeIndicatorClose:
        if (event.button != 2) {
          this.searchMode = null;
          if (this.view.oneOffSearchButtons) {
            this.view.oneOffSearchButtons.selectedButton = null;
          }
          if (this.view.isOpen) {
            this.startQuery({
              event,
            });
          }
        }
        break;

      case this._revertButton:
        this.handleRevert();
        this.select();
        break;

      case this.goButton:
        this.handleCommand(event);
        break;
    }
  }

  _on_auxclick(event) {
    switch (event.target) {
      case this.inputField:
      case this._inputContainer:
        this.#maybeSelectAll();
        this.#maybeUntrimUrl();
        break;

      case this.goButton:
        this.handleCommand(event);
        break;
    }
  }

  _on_contextmenu(event) {
    this.#lazy.addSearchEngineHelper.refreshContextMenu(event);

    if (!event.button) {
      return;
    }

    this.#maybeSelectAll();
  }

  _on_focus(event) {
    lazy.logger.debug("Focus Event");
    if (!this._hideFocus) {
      this.toggleAttribute("focused", true);
    }

    if (this._protocolIsTrimmed) {
      let untrim = false;
      let fixedURI = this._getURIFixupInfo(this.value)?.preferredURI;
      if (fixedURI) {
        try {
          let expectedURI = Services.io.newURI(this._untrimmedValue);
          if (
            lazy.UrlbarPrefs.get("trimHttps") &&
            this._untrimmedValue.startsWith("https://")
          ) {
            untrim =
              fixedURI.displaySpec.replace("http://", "https://") !=
              expectedURI.displaySpec; 
          } else {
            untrim = fixedURI.displaySpec != expectedURI.displaySpec;
          }
        } catch (ex) {
          untrim = true;
        }
      }
      if (untrim) {
        this._setValue(this._untrimmedValue);
      }
    }

    if (this.focusedViaMousedown) {
      this.view.autoOpen({ event });
    } else {
      if (this._untrimOnFocusAfterKeydown) {
        this.#maybeUntrimUrl({ ignoreSelection: true });
      }

      if (this.inputField.hasAttribute("refocused-by-panel")) {
        this.#maybeSelectAll();
      }
    }

    this._updateUrlTooltip();
    this.formatValue();

    if (
      this.getAttribute("pageproxystate") != "valid" &&
      this.window.UpdatePopupNotificationsVisibility
    ) {
      this.window.UpdatePopupNotificationsVisibility();
    }

    Services.obs.notifyObservers(null, "urlbar-focus");
  }

  _on_mouseover() {
    this._updateUrlTooltip();
  }

  _on_draggableregionleftmousedown() {
    if (!lazy.UrlbarPrefs.get("ui.popup.disable_autohide")) {
      this.view.close();
    }
  }

  _on_mousedown(event) {
    switch (event.currentTarget) {
      case this: {
        this._mousedownOnUrlbarDescendant = true;
        if (
          event.composedTarget != this.inputField &&
          event.composedTarget != this._inputContainer
        ) {
          break;
        }

        this.focusedViaMousedown = !this.focused;
        this.#preventClickSelectsAll = this.focused;

        const hasFocus = this.hasAttribute("focused");
        if (event.composedTarget != this.inputField) {
          this.focus();
        }

        if (event.button != 0) {
          break;
        }

        if (this.focusedViaMousedown) {
          this.inputField.setSelectionRange(0, 0);
        }

        this.view.autoOpen({
          event,
          suppressFocusBorder: !hasFocus,
        });
        break;
      }
      case this.window:
        if (this._mousedownOnUrlbarDescendant) {
          this._mousedownOnUrlbarDescendant = false;
          break;
        }
        if (event.target.closest?.("tab, #urlbarView-context-menu")) {
          break;
        }

        if (!lazy.UrlbarPrefs.get("ui.popup.disable_autohide")) {
          if (this.view.isOpen && !this.hasAttribute("focused")) {
            let blurEvent = new FocusEvent("blur", {
              relatedTarget: this.inputField,
            });
          }

          this.view.close();
        }
        break;
    }
  }

  _on_input(event) {

    if (
      lazy.UrlbarPrefs.get("autoFill.adaptiveHistory.enabled") &&
      event.inputType?.startsWith("deleteContent") &&
      !this.isPrivate &&
      this._autofillPlaceholder &&
      this.value === this.userTypedValue &&
      this._resultForCurrentValue?.payload?.url
    ) {
      lazy.UrlbarUtils._lastRecordAutofillBackspacePromise =
        lazy.UrlbarUtils.recordAutofillBackspace(
          this._resultForCurrentValue.payload.url
        );
    }

    let value = this.value;
    this.valueIsTyped = true;
    this._untrimmedValue = value;
    this._protocolIsTrimmed = false;
    this._resultForCurrentValue = null;

    this.userTypedValue = value;
    this.controller.userSelectionBehavior = "none";

    let compositionState = this.#compositionState;
    let compositionClosedPopup = this.#compositionClosedPopup;

    if (this.#compositionState != lazy.UrlbarUtils.COMPOSITION.COMPOSING) {
      this.#compositionState = lazy.UrlbarUtils.COMPOSITION.NONE;
      this.#compositionClosedPopup = false;
    }

    if (
      compositionState == lazy.UrlbarUtils.COMPOSITION.COMPOSING &&
      event.data
    ) {
      this.#compositionHadText = true;
    }

    this.toggleAttribute("usertyping", value);
    this.removeAttribute("actiontype");

    if (
      this.getAttribute("pageproxystate") == "valid" &&
      this.value != this._lastValidURLStr
    ) {
      this.setPageProxyState("invalid", true);
    }

    let state = this.getBrowserState(this.window.gBrowser.selectedBrowser);
    if (
      state.persist?.shouldPersist &&
      this.value !== state.persist.searchTerms
    ) {
      state.persist.shouldPersist = false;
      this.removeAttribute("persistsearchterms");
    }

    if (this.view.isOpen) {
      this.view.maybeRollupPopups();

      if (!value && !lazy.UrlbarPrefs.get("suggest.topsites")) {
        this.view.clear();
        if (!this.searchMode || !this.view.oneOffSearchButtons?.hasView) {
          this.view.close();
          return;
        }
      }
    } else {
      this.view.clear();
    }

    this.view.removeAccessibleFocus();


    if (
      !lazy.UrlbarPrefs.get("keepPanelOpenDuringImeComposition") &&
      (compositionState == lazy.UrlbarUtils.COMPOSITION.COMPOSING ||
        (compositionState == lazy.UrlbarUtils.COMPOSITION.CANCELED &&
          !compositionClosedPopup))
    ) {
      return;
    }

    const allowAutofill =
      (!lazy.UrlbarPrefs.get("keepPanelOpenDuringImeComposition") ||
        compositionState !== lazy.UrlbarUtils.COMPOSITION.COMPOSING) &&
      !event.inputType?.startsWith("delete") &&
      !event.inputType?.startsWith("history") &&
      !lazy.UrlbarUtils.isPasteEvent(event) &&
      this._maybeAutofillPlaceholder(value);

    this.startQuery({
      searchString: value,
      allowAutofill,
      resetSearchState: false,
      event,
    });
  }

  _on_selectionchange() {
    if (
      this._autofillPlaceholder &&
      this._autofillPlaceholder.value == this.value &&
      (this._autofillPlaceholder.selectionStart != this.selectionStart ||
        this._autofillPlaceholder.selectionEnd != this.selectionEnd)
    ) {
      this._autofillPlaceholder = null;
      this.userTypedValue = this.value;
    }
  }

  _on_select() {
    if (
      this._suppressPrimaryAdjustment ||
      !this.window.windowUtils.isHandlingUserInput ||
      !Services.clipboard.isClipboardTypeSupported(
        Services.clipboard.kSelectionClipboard
      )
    ) {
      return;
    }

    let val = this._getSelectedValueForClipboard();
    if (!val) {
      return;
    }

    lazy.ClipboardHelper.copyStringToClipboard(
      val,
      Services.clipboard.kSelectionClipboard
    );
  }

  _on_overflow(_event) {
    this._overflowing = true;
    this.updateTextOverflow();
  }

  _on_underflow(_event) {
    this._overflowing = false;
    this.updateTextOverflow();
    this._updateUrlTooltip();
  }

  _on_paste(event) {
    let originalPasteData = event.clipboardData.getData("text/plain");
    if (!originalPasteData) {
      return;
    }

    let oldValue = this.value;
    let oldStart = oldValue.substring(0, this.selectionStart);
    if (oldStart.trim()) {
      return;
    }
    let oldEnd = oldValue.substring(this.selectionEnd);

    const pasteData =
      lazy.UrlbarUtils.sanitizeTextFromClipboard(originalPasteData);

    if (originalPasteData != pasteData) {
      event.preventDefault();
      event.stopImmediatePropagation();

      const value = oldStart + pasteData + oldEnd;
      this._setValue(value, { valueIsTyped: true });
      this.userTypedValue = value;

      if (this.getAttribute("pageproxystate") == "valid") {
        this.setPageProxyState("invalid");
      }
      this.toggleAttribute("usertyping", this._untrimmedValue);

      let newCursorPos = oldStart.length + pasteData.length;
      this.inputField.setSelectionRange(newCursorPos, newCursorPos);

      this.startQuery({
        searchString: this.value,
        allowAutofill: false,
        resetSearchState: false,
        event,
      });
    }
  }

  #makeQueryContext({
    allowAutofill = true,
    searchString = null,
    event = null,
  } = {}) {
    let maxResults =
      this.searchMode?.source != UrlbarShared.RESULT_SOURCE.ACTIONS
        ? lazy.UrlbarPrefs.get("maxRichResults")
        : UNLIMITED_MAX_RESULTS;
    let options = {
      allowAutofill,
      isPrivate: this.isPrivate,
      sapName: this.sapName,
      maxResults,
      searchString,
      userContextId: parseInt(
        this.window.gBrowser.selectedBrowser.getAttribute("usercontextid") || 0
      ),
      tabGroup: this.window.gBrowser.selectedTab.group?.id ?? null,
      currentPage: this.window.gBrowser.currentURI.spec,
    };

    if (this.searchMode) {
      options.searchMode = this.searchMode;
      if (
        this.searchMode.source &&
        !lazy.UrlbarPrefs.get("unifiedSearchButton.historyInSearchMode")
      ) {
        options.sources = [this.searchMode.source];
      }
    }

    return new lazy.UrlbarQueryContext(options);
  }

  _on_scrollend() {
    this.updateTextOverflow();
  }

  _on_TabSelect() {
    this._untrimOnFocusAfterKeydown = false;
    this._gotTabSelect = true;
    this._afterTabSelectAndFocusChange();
  }

  _on_TabClose(event) {

    if (this.view.isOpen) {
      this.startQuery();
    }
  }

  _on_beforeinput(event) {
    if (
      (event.data && this._keyDownEnterDeferred) ||
      (event.data == " " && this.view.shouldSpaceActivateSelectedElement())
    ) {
      event.preventDefault();
    }
  }

  _on_keydown(event) {
    if (this.view.resultMenu.hasAttribute("open")) {
      return;
    }

    if (event.currentTarget == this.window) {
      this._untrimOnFocusAfterKeydown = !this.focused;
      return;
    }

    if (!event.repeat) {
      this.#allTextSelectedOnKeyDown = this.#allTextSelected;

      if (event.keyCode === KeyEvent.DOM_VK_RETURN) {
        if (this._keyDownEnterDeferred) {
          this._keyDownEnterDeferred.reject();
        }
        this._keyDownEnterDeferred = Promise.withResolvers();
        event._disableCanonization =
          AppConstants.platform == "macosx"
            ? this._isKeyDownWithMeta
            : this._isKeyDownWithCtrl;
      }

      if (event.ctrlKey && event.keyCode != KeyEvent.DOM_VK_CONTROL) {
        this._isKeyDownWithCtrl = true;
      }
      if (event.metaKey && event.keyCode != KeyEvent.DOM_VK_META) {
        this._isKeyDownWithMeta = true;
      }
      this._isKeyDownWithMetaAndLeft =
        this._isKeyDownWithMeta &&
        !event.shiftKey &&
        event.keyCode == KeyEvent.DOM_VK_LEFT;

      this._toggleActionOverride(event);
    }

    if (this.eventBufferer.shouldDeferEvent(event)) {
      this.controller.handleKeyNavigation(event, false);
    }
    this.eventBufferer.maybeDeferEvent(event, () => {
      this.controller.handleKeyNavigation(event);
    });
  }

  async _on_keyup(event) {
    if (event.currentTarget == this.window) {
      this._untrimOnFocusAfterKeydown = false;
      return;
    }

    if (this.#allTextSelectedOnKeyDown) {
      let moveCursorToStart = this.#isHomeKeyUpEvent(event);
      if (moveCursorToStart) {
        this.selectionStart = this.selectionEnd = 0;
      }
      this.#maybeUntrimUrl({ moveCursorToStart });
    }
    if (event.keyCode === KeyEvent.DOM_VK_META) {
      this._isKeyDownWithMeta = false;
      this._isKeyDownWithMetaAndLeft = false;
    }
    if (event.keyCode === KeyEvent.DOM_VK_CONTROL) {
      this._isKeyDownWithCtrl = false;
    }

    this._toggleActionOverride(event);

    if (this._keyDownEnterDeferred) {
      if (this._keyDownEnterDeferred.loadedContent) {
        try {
          const loadingBrowser = await this._keyDownEnterDeferred.promise;
          if (this.window.gBrowser.selectedBrowser === loadingBrowser) {
            loadingBrowser.focus();
            this.inputField.setSelectionRange(0, 0);
          }
        } catch (ex) {
        }
      } else {
        this._keyDownEnterDeferred.resolve();
      }

      this._keyDownEnterDeferred = null;
    }
  }

  _on_compositionstart() {
    if (this.#compositionState == lazy.UrlbarUtils.COMPOSITION.COMPOSING) {
      throw new Error("Trying to start a nested composition?");
    }
    this.#compositionState = lazy.UrlbarUtils.COMPOSITION.COMPOSING;
    this.#compositionHadText = false;

    if (lazy.UrlbarPrefs.get("keepPanelOpenDuringImeComposition")) {
      return;
    }

    if (this.view.isOpen) {
      if (this.searchMode) {
        if (!this.value) {
          this.userTypedValue = null;
        }
        this.confirmSearchMode();
      }
      this.#compositionClosedPopup = true;
      this.view.close();
    } else {
      this.#compositionClosedPopup = false;
    }
  }

  _on_compositionend(event) {
    if (this.#compositionState != lazy.UrlbarUtils.COMPOSITION.COMPOSING) {
      throw new Error("Trying to stop a non existing composition?");
    }

    if (!lazy.UrlbarPrefs.get("keepPanelOpenDuringImeComposition")) {
      this.view.clearSelection();
      this._resultForCurrentValue = null;
    }

    this.#compositionState = event.data
      ? lazy.UrlbarUtils.COMPOSITION.COMMIT
      : lazy.UrlbarUtils.COMPOSITION.CANCELED;

    if (
      !event.data &&
      !this.#compositionHadText &&
      this.#compositionClosedPopup &&
      !lazy.UrlbarPrefs.get("keepPanelOpenDuringImeComposition")
    ) {
      this.#compositionState = lazy.UrlbarUtils.COMPOSITION.NONE;
      this.#compositionClosedPopup = false;
      this.startQuery({ resetSearchState: false, event });
    }
  }

  _on_dragstart(event) {
    let nodePosition = this.inputField.compareDocumentPosition(
      event.originalTarget
    );
    if (
      event.target != this.inputField &&
      !(nodePosition & Node.DOCUMENT_POSITION_CONTAINED_BY)
    ) {
      return;
    }

    this.view.close();

    if (
      !this.#allTextSelected ||
      this.getAttribute("pageproxystate") != "valid"
    ) {
      return;
    }

    let uri = this.makeURIReadable(this.window.gBrowser.currentURI);
    let href = uri.displaySpec;
    let title = this.window.gBrowser.contentTitle || href;

    event.dataTransfer.setData("text/x-moz-url", `${href}\n${title}`);
    event.dataTransfer.setData("text/plain", href);
    event.dataTransfer.setData("text/html", `<a href="${href}">${title}</a>`);
    event.dataTransfer.effectAllowed = "copyLink";
    event.stopPropagation();
  }

  _on_dragover(event) {
    if (!this.#isAddressbar) {
      if (!event.dataTransfer.types.includes("text/plain")) {
        event.dataTransfer.dropEffect = "none";
      }
      return;
    }

    if (!Services.droppedLinkHandler.canDropLink(event, true)) {
      event.dataTransfer.dropEffect = "none";
    }
  }

  _on_drop(event) {
    if (!this.#isAddressbar) {
      this.value = "";
      return;
    }

    let droppedData = getDroppableData(event);
    if (!droppedData) {
      return;
    }
    let droppedString = URL.isInstance(droppedData)
      ? droppedData.href
      : droppedData;
    if (droppedString == this.window.gBrowser.currentURI.spec) {
      return;
    }

    this.value = droppedString;
    this.setPageProxyState("invalid");
    this.focus();

    let principal = Services.droppedLinkHandler.getTriggeringPrincipal(event);
    let queryContext = this.#makeQueryContext({
      searchString: droppedString,
    });
    this.controller.setLastQueryContextCache(queryContext);
    this.handleNavigation({ triggeringPrincipal: principal });
    this.userTypedValue = null;
    this.setURI({ dueToTabSwitch: true });
  }

  _on_uidensitychanged() {
    if (this.#breakoutBlockerCount) {
      return;
    }
    this.#updateLayoutBreakout();
  }

  _on_toolbarvisibilitychange() {
    this.#updateTextboxPositionNextFrame();
  }

  _on_DOMMenuBarActive() {
    this.#updateTextboxPositionNextFrame();
  }

  _on_DOMMenuBarInactive() {
    this.#updateTextboxPositionNextFrame();
  }

  #allTextSelectedOnKeyDown = false;
  get #allTextSelected() {
    return this.selectionStart == 0 && this.selectionEnd == this.value.length;
  }

  #getSchemelessInput(value) {
    return ["http://", "https://", "file://"].every(
      scheme => !value.trim().startsWith(scheme)
    )
      ? Ci.nsILoadInfo.SchemelessInputTypeSchemeless
      : Ci.nsILoadInfo.SchemelessInputTypeSchemeful;
  }

  get #isOpenedPageInBlankTargetLoading() {
    return (
      this.window.gBrowser.selectedBrowser.browsingContext.sessionHistory
        ?.count === 0 &&
      this.window.gBrowser.selectedBrowser.browsingContext
        .nonWebControlledLoadingURI
    );
  }


  #browserStates = new WeakMap();

  get #selectedText() {
    return this.editor.selection.toStringWithFormat(
      "text/plain",
      Ci.nsIDocumentEncoder.OutputPreformatted |
        Ci.nsIDocumentEncoder.OutputRaw,
      0
    );
  }

  #isHomeKeyUpEvent(event) {
    let isMac = AppConstants.platform === "macosx";
    return (
      event.keyCode == KeyEvent.DOM_VK_HOME ||
      (!isMac &&
        event.keyCode == KeyboardEvent.DOM_VK_LEFT &&
        event.ctrlKey &&
        !event.shiftKey) ||
      (isMac &&
        event.keyCode == KeyboardEvent.DOM_VK_A &&
        event.ctrlKey &&
        !event.shiftKey) ||
      (isMac &&
        event.keyCode == KeyEvent.DOM_VK_META &&
        this._isKeyDownWithMetaAndLeft)
    );
  }

  #canHandleAsBlankPage(spec) {
    return this.window.isBlankPageURL(spec) || spec == "about:privatebrowsing";
  }
}

function getDroppableData(event) {
  let links;
  try {
    links = Services.droppedLinkHandler.dropLinks(event);
  } catch (ex) {
    return null;
  }
  if (links[0]?.url) {
    event.preventDefault();
    let href = links[0].url;
    if (lazy.UrlbarUtils.stripUnsafeProtocolOnPaste(href) != href) {
      event.stopImmediatePropagation();
      return null;
    }

    let url = URL.parse(href);
    if (url) {
      try {
        let principal =
          Services.droppedLinkHandler.getTriggeringPrincipal(event);
        Services.scriptSecurityManager.checkLoadURIStrWithPrincipal(
          principal,
          url.href,
          Ci.nsIScriptSecurityManager.DISALLOW_INHERIT_PRINCIPAL
        );
        return url;
      } catch (ex) {
        return null;
      }
    }
  }
  return event.dataTransfer.getData("text/plain");
}

function losslessDecodeURI(aURI) {
  let scheme = aURI.scheme;
  let value = aURI.displaySpec;

  if (!/%25(?:3B|2F|3F|3A|40|26|3D|2B|24|2C|23)/i.test(value)) {
    let decodeASCIIOnly = !["https", "http", "file", "ftp"].includes(scheme);
    if (decodeASCIIOnly) {
      value = value.replace(
        /%(2[0-4]|2[6-9a-f]|[3-6][0-9a-f]|7[0-9a-e])/g,
        decodeURI
      );
    } else {
      try {
        value = decodeURI(value)
          .replace(
            /%(?!3B|2F|3F|3A|40|26|3D|2B|24|2C|23)/gi,
            encodeURIComponent
          );
      } catch (e) {}
    }
  }


  value = value.replace(
    // eslint-disable-next-line no-control-regex
    /[[\p{Separator}--\u{0020}]\p{Control}\u{2800}\u{FFFC}]|\u{0020}(?=[\p{Other}\p{Separator}])|\s$/gv,
    encodeURIComponent
  );

  value = value.replace(
    // eslint-disable-next-line no-misleading-character-class
    /[[\p{Format}--[\u{200C}\u{200D}]]\u{034F}\u{115F}\u{1160}\u{17B4}\u{17B5}\u{180B}-\u{180D}\u{3164}\u{FE00}-\u{FE0F}\u{FFA0}\u{FFF0}-\u{FFFB}\p{Unassigned}\p{Private_Use}\u{E0000}-\u{E0FFF}\u{1F50F}-\u{1F513}\u{1F6E1}]/gv,
    encodeURIComponent
  );
  return value;
}

class CopyCutController {
  constructor(urlbar) {
    this.urlbar = urlbar;
  }

  doCommand(command) {
    let urlbar = this.urlbar;
    let val = urlbar._getSelectedValueForClipboard();
    if (!val) {
      return;
    }

    if (command == "cmd_cut" && this.isCommandEnabled(command)) {
      let start = urlbar.selectionStart;
      let end = urlbar.selectionEnd;
      urlbar.inputField.value =
        urlbar.inputField.value.substring(0, start) +
        urlbar.inputField.value.substring(end);
      urlbar.inputField.setSelectionRange(start, start);

      let event = new UIEvent("input", {
        bubbles: true,
        cancelable: false,
        view: urlbar.window,
        detail: 0,
      });
      urlbar.inputField.dispatchEvent(event);
    }

    lazy.ClipboardHelper.copyString(val);
  }

  supportsCommand(command) {
    switch (command) {
      case "cmd_copy":
      case "cmd_cut":
        return true;
    }
    return false;
  }

  isCommandEnabled(command) {
    return (
      this.supportsCommand(command) &&
      (command != "cmd_cut" || !this.urlbar.readOnly) &&
      this.urlbar.selectionStart < this.urlbar.selectionEnd
    );
  }

  onEvent() {}
}

class AddSearchEngineHelper {
  shortcutButtons;

  constructor(input) {
    this.input = input;
    this.shortcutButtons = input.view.oneOffSearchButtons;
  }

  get maxInlineEngines() {
    return SearchModeSwitcher.MAX_OPENSEARCH_ENGINES;
  }

  setEnginesFromBrowser(browser, engines) {
    this.browsingContext = browser.browsingContext;
    engines = engines.slice();
    if (!this._sameEngines(this.engines, engines)) {
      this.engines = engines;
      this.shortcutButtons?.updateWebEngines();
    }
  }

  _sameEngines(engines1, engines2) {
    if (engines1?.length != engines2?.length) {
      return false;
    }
    return lazy.ObjectUtils.deepEqual(
      engines1.map(e => e.title),
      engines2.map(e => e.title)
    );
  }

  _createMenuitem(engine, index) {
    let elt = this.input.document.createXULElement("menuitem");
    elt.setAttribute("anonid", `add-engine-${index}`);
    elt.classList.add("menuitem-iconic");
    elt.classList.add("context-menu-add-engine");
    this.input.document.l10n.setAttributes(elt, "search-one-offs-add-engine", {
      engineName: engine.title,
    });
    elt.setAttribute("uri", engine.uri);
    if (engine.icon) {
      elt.setAttribute("image", engine.icon);
    } else {
      elt.removeAttribute("image");
    }
    elt.addEventListener("command", this._onCommand.bind(this));
    return elt;
  }

  _createMenu(engine) {
    let elt = this.input.document.createXULElement("menu");
    elt.setAttribute("anonid", "add-engine-menu");
    elt.classList.add("menu-iconic");
    elt.classList.add("context-menu-add-engine");
    this.input.document.l10n.setAttributes(
      elt,
      "search-one-offs-add-engine-menu"
    );
    if (engine.icon) {
      elt.setAttribute("image", ChromeUtils.encodeURIForSrcset(engine.icon));
    }
    let popup = this.input.document.createXULElement("menupopup");
    elt.appendChild(popup);
    return elt;
  }

  refreshContextMenu() {
    let engines = this.engines;
    let contextMenu = this.input.querySelector("moz-input-box").menupopup;

    if (!contextMenu.querySelector(".menuseparator-add-engine")) {
      this.contextSeparator =
        this.input.document.createXULElement("menuseparator");
      this.contextSeparator.setAttribute("anonid", "add-engine-separator");
      this.contextSeparator.classList.add("menuseparator-add-engine");
      this.contextSeparator.collapsed = true;
      contextMenu.appendChild(this.contextSeparator);
    }

    this.contextSeparator.collapsed = !engines.length;
    let curElt = this.contextSeparator;
    for (let elt = curElt.nextElementSibling; elt; ) {
      let nextElementSibling = elt.nextElementSibling;
      elt.remove();
      elt = nextElementSibling;
    }

    if (engines.length > this.maxInlineEngines) {
      let elt = this._createMenu(engines[0]);
      this.contextSeparator.insertAdjacentElement("afterend", elt);
      curElt = elt.lastElementChild;
    }

    for (let i = 0; i < engines.length; ++i) {
      let elt = this._createMenuitem(engines[i], i);
      if (curElt.localName == "menupopup") {
        curElt.appendChild(elt);
      } else {
        curElt.insertAdjacentElement("afterend", elt);
      }
      curElt = elt;
    }
  }

  async _onCommand(event) {
    let added = await lazy.SearchUIUtils.addOpenSearchEngine(
      event.target.getAttribute("uri"),
      event.target.getAttribute("image"),
      this.browsingContext
    ).catch(console.error);
    if (added) {
      this.refreshContextMenu();
    }
  }
}

customElements.define("moz-urlbar", UrlbarInput);
