/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  OpenSearchManager:
    "moz-src:///browser/components/search/OpenSearchManager.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  SearchUIUtils: "moz-src:///browser/components/search/SearchUIUtils.sys.mjs",
});




export class SearchOneOffs {
  constructor(container) {
    this.container = container;
    this.window = container.documentGlobal;
    this.document = container.ownerDocument;

    this.container.appendChild(
      this.window.MozXULElement.parseXULToFragment(
        `
      <hbox class="search-panel-one-offs-header search-panel-header">
        <label class="search-panel-one-offs-header-label" data-l10n-id="search-one-offs-with-title"/>
      </hbox>
      <box class="search-panel-one-offs-container">
        <hbox class="search-panel-one-offs" role="group"/>
        <button class="searchbar-engine-one-off-item search-setting-button" tabindex="-1" data-l10n-id="search-one-offs-change-settings-compact-button"/>
      </box>
      <box>
        <menupopup class="search-one-offs-context-menu">
          <menuitem class="search-one-offs-context-open-in-new-tab" data-l10n-id="search-one-offs-context-open-new-tab"/>
          <menuitem class="search-one-offs-context-set-default" data-l10n-id="search-one-offs-context-set-as-default"/>
          <menuitem class="search-one-offs-context-set-default-private" data-l10n-id="search-one-offs-context-set-as-default-private"/>
        </menupopup>
      </box>
      `
      )
    );

    this._popup = null;
    this._textbox = null;

    this._textboxWidth = 0;

    this.telemetryOrigin = "";

    this._query = "";

    this._selectedButton = null;

    this.buttons = this.querySelector(".search-panel-one-offs");

    this.header = this.querySelector(".search-panel-one-offs-header");

    this.settingsButton = this.querySelector(".search-setting-button");

    this.contextMenuPopup = this.querySelector(".search-one-offs-context-menu");

    this._engineInfo = null;

    this._rebuilding = false;

    this.addEventListener("mousedown", this);
    this.addEventListener("click", this);
    this.addEventListener("command", this);
    this.addEventListener("contextmenu", this);

    let listener = aEvent => aEvent.stopPropagation();
    this.contextMenuPopup.addEventListener("popupshowing", listener);
    this.contextMenuPopup.addEventListener("popuphiding", listener);
    this.contextMenuPopup.addEventListener("popupshown", aEvent => {
      aEvent.stopPropagation();
    });
    this.contextMenuPopup.addEventListener("popuphidden", aEvent => {
      aEvent.stopPropagation();
    });

    this.QueryInterface = ChromeUtils.generateQI([
      "nsIObserver",
      "nsISupportsWeakReference",
    ]);
    Services.obs.addObserver(this, "browser-search-engine-modified", true);
    Services.obs.addObserver(this, "browser-search-service", true);

    Services.obs.addObserver(this, "lightweight-theme-changed", true);

    this.disableOneOffsHorizontalKeyNavigation = false;
  }

  addEventListener(...args) {
    this.container.addEventListener(...args);
  }

  removeEventListener(...args) {
    this.container.removeEventListener(...args);
  }

  dispatchEvent(...args) {
    this.container.dispatchEvent(...args);
  }

  getAttribute(...args) {
    return this.container.getAttribute(...args);
  }

  hasAttribute(...args) {
    return this.container.hasAttribute(...args);
  }

  setAttribute(...args) {
    this.container.setAttribute(...args);
  }

  querySelector(...args) {
    return this.container.querySelector(...args);
  }

  handleEvent(event) {
    let methodName = "_on_" + event.type;
    if (methodName in this) {
      this[methodName](event);
    } else {
      throw new Error("Unrecognized search-one-offs event: " + event.type);
    }
  }

  async willHide() {
    if (this._engineInfo?.willHide !== undefined) {
      return this._engineInfo.willHide;
    }
    let engineInfo = await this.getEngineInfo();
    let oneOffCount = engineInfo.engines.length;
    engineInfo.willHide =
      !oneOffCount ||
      (oneOffCount == 1 &&
        engineInfo.engines[0].name == engineInfo.default.name);
    return engineInfo.willHide;
  }

  invalidateCache() {
    if (!this._rebuilding) {
      this._engineInfo = null;
    }
  }

  get buttonWidth() {
    return 48;
  }

  set popup(val) {
    if (this._popup) {
      this._popup.removeEventListener("popupshowing", this);
      this._popup.removeEventListener("popuphidden", this);
    }
    if (val) {
      val.addEventListener("popupshowing", this);
      val.addEventListener("popuphidden", this);
    }
    this._popup = val;

    if (val && val.state != "closed") {
      this._rebuild();
    }
  }

  get popup() {
    return this._popup;
  }

  set textbox(val) {
    if (this._textbox) {
      this._textbox.removeEventListener("input", this);
    }
    if (val) {
      val.addEventListener("input", this);
    }
    this._textbox = val;
  }

  get style() {
    return this.container.style;
  }

  get textbox() {
    return this._textbox;
  }

  set query(val) {
    this._query = val;
    if (this.isViewOpen) {
      let isOneOffSelected =
        this.selectedButton &&
        this.selectedButton.classList.contains(
          "searchbar-engine-one-off-item"
        ) &&
        !(
          this.selectedButton == this.settingsButton &&
          this.hasAttribute("is_searchbar")
        );
      if (this.selectedButton && !isOneOffSelected) {
        this.selectedButton = null;
      }
    }
  }

  get query() {
    return this._query;
  }

  set selectedButton(val) {
    let previousButton = this._selectedButton;
    if (previousButton) {
      previousButton.removeAttribute("selected");
    }
    if (val) {
      val.toggleAttribute("selected", true);
    }
    this._selectedButton = val;

    if (this.textbox) {
      if (val) {
        this.textbox.setAttribute("aria-activedescendant", val.id);
      } else {
        let active = this.textbox.getAttribute("aria-activedescendant");
        if (active && active.includes("-engine-one-off-item-")) {
          this.textbox.removeAttribute("aria-activedescendant");
        }
      }
    }

    this.dispatchEvent(new CustomEvent("SelectedOneOffButtonChanged"));
  }

  get selectedButton() {
    return this._selectedButton;
  }

  set selectedButtonIndex(val) {
    let buttons = this.getSelectableButtons(true);
    this.selectedButton = buttons[val];
  }

  get selectedButtonIndex() {
    let buttons = this.getSelectableButtons(true);
    for (let i = 0; i < buttons.length; i++) {
      if (buttons[i] == this._selectedButton) {
        return i;
      }
    }
    return -1;
  }

  async getEngineInfo() {
    if (this._engineInfo) {
      return this._engineInfo;
    }

    let defaultEngine;
    if (lazy.PrivateBrowsingUtils.isWindowPrivate(this.window)) {
      defaultEngine = await lazy.SearchService.getDefaultPrivate();
    } else {
      defaultEngine = await lazy.SearchService.getDefault();
    }

    let currentEngineNameToIgnore;
    if (!this.getAttribute("includecurrentengine")) {
      currentEngineNameToIgnore = defaultEngine.name;
    }

    let engines = (await lazy.SearchService.getVisibleEngines()).filter(e => {
      let name = e.name;
      return (
        (!currentEngineNameToIgnore || name != currentEngineNameToIgnore) &&
        !e.hideOneOffButton
      );
    });

    this._engineInfo = { default: defaultEngine, engines };
    return this._engineInfo;
  }

  observe(aSubject, aTopic, aData) {
    if (aTopic != "browser-search-service" || aData == "engines-reloaded") {
      this.invalidateCache();
    }

    if (aData === "engine-icon-changed") {
      let engine = aSubject.wrappedJSObject;
      engine.getIconURL().then(icon => {
        this.getSelectableButtons(false)
          .find(b => b.engine?.id == engine.id)
          ?.setAttribute(
            "image",
            icon || "chrome://browser/skin/search-engine-placeholder.png"
          );
      });
    }
  }

  get _maxInlineAddEngines() {
    return 3;
  }

  async _rebuild() {
    if (this._rebuilding) {
      return;
    }

    this._rebuilding = true;
    try {
      await this.__rebuild();
    } catch (ex) {
      console.error("Search-one-offs::_rebuild() error:", ex);
    } finally {
      this._rebuilding = false;
      this.dispatchEvent(new Event("rebuild"));
    }
  }

  async __rebuild() {
    if (!this.popup && this._engineInfo?.domWasUpdated) {
      return;
    }

    const addEngines = lazy.OpenSearchManager.getEngines(
      this.window.gBrowser.selectedBrowser
    );

    if (this.popup && this._textbox) {
      let textboxWidth = await this.window.promiseDocumentFlushed(() => {
        return this._textbox.clientWidth;
      });

      if (
        this._engineInfo?.domWasUpdated &&
        this._textboxWidth == textboxWidth &&
        this._addEngines == addEngines
      ) {
        return;
      }
      this._textboxWidth = textboxWidth;
      this._addEngines = addEngines;
    }

    const isSearchBar = this.hasAttribute("is_searchbar");
    if (isSearchBar) {
      this.container.hidden = true;
    }

    while (this.buttons.firstElementChild) {
      this.buttons.firstElementChild.remove();
    }

    let headerText = this.header.querySelector(
      ".search-panel-one-offs-header-label"
    );
    headerText.id = this.telemetryOrigin + "-one-offs-header-label";
    this.buttons.setAttribute("aria-labelledby", headerText.id);

    let addEngineNeeded = isSearchBar && addEngines.length;
    let hideOneOffs = (await this.willHide()) && !addEngineNeeded;

    this._engineInfo.domWasUpdated = true;

    this.container.hidden = hideOneOffs;

    if (hideOneOffs) {
      return;
    }

    let origin = this.telemetryOrigin;
    this.settingsButton.id = origin + "-anon-search-settings";

    let engines = (await this.getEngineInfo()).engines;
    await this._rebuildEngineList(engines, addEngines);
  }

  async _rebuildEngineList(engines, addEngines) {
    for (let i = 0; i < engines.length; ++i) {
      let engine = engines[i];
      let button = this.document.createXULElement("button");
      button.engine = engine;
      button.id = this._buttonIDForEngine(engine);
      let iconURL =
        (await engine.getIconURL()) ||
        "chrome://browser/skin/search-engine-placeholder.png";
      button.setAttribute("image", iconURL);
      button.setAttribute("class", "searchbar-engine-one-off-item");
      button.setAttribute("tabindex", "-1");
      this.setTooltipForEngineButton(button);
      this.buttons.appendChild(button);
    }

    for (
      let i = 0, len = Math.min(addEngines.length, this._maxInlineAddEngines);
      i < len;
      i++
    ) {
      const engine = addEngines[i];
      const button = this.document.createXULElement("button");
      button.id = this._buttonIDForEngine(engine);
      button.classList.add("searchbar-engine-one-off-item");
      button.classList.add("searchbar-engine-one-off-add-engine");
      button.setAttribute("tabindex", "-1");
      if (engine.icon) {
        button.setAttribute("image", engine.icon);
      }
      this.document.l10n.setAttributes(button, "search-one-offs-add-engine", {
        engineName: engine.title,
      });
      button.setAttribute("engine-name", engine.title);
      button.setAttribute("uri", engine.uri);
      this.buttons.appendChild(button);
    }
  }

  _buttonIDForEngine(engine) {
    return (
      this.telemetryOrigin +
      "-engine-one-off-item-engine-" +
      this._engineInfo.engines.indexOf(engine)
    );
  }

  getSelectableButtons(aIncludeNonEngineButtons) {
    const buttons = [
      ...this.buttons.querySelectorAll(".searchbar-engine-one-off-item"),
    ];

    if (aIncludeNonEngineButtons) {
      buttons.push(this.settingsButton);
    }

    return buttons;
  }

  _whereToOpen(aEvent, aForceNewTab = false) {
    let where = "current";
    let params;
    if (aForceNewTab) {
      where = "tab";
      if (Services.prefs.getBoolPref("browser.tabs.loadInBackground")) {
        params = {
          inBackground: true,
        };
      }
    } else {
      let newTabPref = Services.prefs.getBoolPref("browser.search.openintab");
      if (
        (KeyboardEvent.isInstance(aEvent) && aEvent.altKey) != newTabPref &&
        !this.window.gBrowser.selectedTab.isEmpty
      ) {
        where = "tab";
      }
      if (
        MouseEvent.isInstance(aEvent) &&
        (aEvent.button == 1 || aEvent.getModifierState("Accel"))
      ) {
        where = "tab";
        params = {
          inBackground: true,
        };
      }
    }

    return { where, params };
  }

  advanceSelection(aForward, aIncludeNonEngineButtons, aWrapAround) {
    let buttons = this.getSelectableButtons(aIncludeNonEngineButtons);
    let index;
    if (this.selectedButton) {
      let inc = aForward ? 1 : -1;
      let oldIndex = buttons.indexOf(this.selectedButton);
      index = (oldIndex + inc + buttons.length) % buttons.length;
      if (
        !aWrapAround &&
        ((aForward && index <= oldIndex) || (!aForward && oldIndex <= index))
      ) {
        index = -1;
      }
    } else {
      index = aForward ? 0 : buttons.length - 1;
    }
    this.selectedButton = index < 0 ? null : buttons[index];
  }

  handleKeyDown(event, numListItems, allowEmptySelection, textboxUserValue) {
    if (!this.hasView) {
      return false;
    }
    let handled = this._handleKeyDown(
      event,
      numListItems,
      allowEmptySelection,
      textboxUserValue
    );
    if (handled) {
      event.preventDefault();
      event.stopPropagation();
    }
    return handled;
  }

  _handleKeyDown(event, numListItems, allowEmptySelection, textboxUserValue) {
    if (this.container.hidden) {
      return false;
    }
    if (
      event.keyCode == KeyEvent.DOM_VK_RIGHT &&
      this.selectedButton &&
      this.selectedButton.classList.contains("addengine-menu-button")
    ) {
      this.selectedButton.open = true;
      return true;
    }

    if (
      event.keyCode == KeyEvent.DOM_VK_TAB &&
      !event.getModifierState("Alt") &&
      !event.getModifierState("AltGraph") &&
      !event.getModifierState("Control") &&
      !event.getModifierState("Meta")
    ) {
      if (
        this.getAttribute("disabletab") == "true" ||
        (event.shiftKey && this.selectedButtonIndex <= 0) ||
        (!event.shiftKey &&
          this.selectedButtonIndex ==
            this.getSelectableButtons(true).length - 1)
      ) {
        this.selectedButton = null;
        return false;
      }
      this.selectedViewIndex = -1;
      this.advanceSelection(!event.shiftKey, true, false);
      return !!this.selectedButton;
    }

    if (event.keyCode == KeyboardEvent.DOM_VK_UP) {
      if (event.altKey) {
        this.advanceSelection(false, false, false);
        return true;
      }
      if (numListItems == 0) {
        this.advanceSelection(false, true, false);
        return true;
      }
      if (this.selectedViewIndex > 0) {
        this.selectedButton = null;
        return false;
      }
      if (this.selectedViewIndex == 0) {
        if (allowEmptySelection) {
          return false;
        }
        if (this.textbox && typeof textboxUserValue == "string") {
          this.textbox.value = textboxUserValue;
        }
        this.selectedViewIndex = -1;
        this.advanceSelection(false, true, true);
        return true;
      }
      if (!this.selectedButton) {
        this.advanceSelection(false, true, true);
        return true;
      }
      if (this.selectedButtonIndex == 0) {
        this.selectedButton = null;
        return false;
      }
      this.advanceSelection(false, true, false);
      return true;
    }

    if (event.keyCode == KeyboardEvent.DOM_VK_DOWN) {
      if (event.altKey) {
        this.advanceSelection(true, false, false);
        return true;
      }
      if (numListItems == 0) {
        this.advanceSelection(true, true, false);
        return true;
      }
      if (
        this.selectedViewIndex >= 0 &&
        this.selectedViewIndex < numListItems - 1
      ) {
        this.selectedButton = null;
        return false;
      }
      if (this.selectedViewIndex == numListItems - 1) {
        if (!allowEmptySelection) {
          this.selectedViewIndex = -1;
          if (this.textbox && typeof textboxUserValue == "string") {
            this.textbox.value = textboxUserValue;
          }
        }
        this.selectedButtonIndex = 0;
        if (allowEmptySelection) {
          return false;
        }
        return true;
      }
      if (this.selectedButton) {
        let buttons = this.getSelectableButtons(true);
        if (this.selectedButtonIndex == buttons.length - 1) {
          this.selectedButton = null;
          if (allowEmptySelection) {
            return true;
          }
          return false;
        }
        this.advanceSelection(true, true, false);
        return true;
      }
      return false;
    }

    if (event.keyCode == KeyboardEvent.DOM_VK_LEFT) {
      if (
        this.selectedButton &&
        this.selectedButton.engine &&
        !this.disableOneOffsHorizontalKeyNavigation
      ) {
        this.advanceSelection(false, true, true);
        return true;
      }
      return false;
    }

    if (event.keyCode == KeyboardEvent.DOM_VK_RIGHT) {
      if (
        this.selectedButton &&
        this.selectedButton.engine &&
        !this.disableOneOffsHorizontalKeyNavigation
      ) {
        this.advanceSelection(true, true, true);
        return true;
      }
      return false;
    }

    return false;
  }

  eventTargetIsAOneOff(event) {
    if (!event) {
      return false;
    }

    let target = event.originalTarget;

    if (KeyboardEvent.isInstance(event) && this.selectedButton) {
      return true;
    }
    if (
      MouseEvent.isInstance(event) &&
      Element.isInstance(target) &&
      target.classList.contains("searchbar-engine-one-off-item")
    ) {
      return true;
    }
    if (
      this.window.XULCommandEvent.isInstance(event) &&
      Element.isInstance(target) &&
      target.classList.contains("search-one-offs-context-open-in-new-tab")
    ) {
      return true;
    }

    return false;
  }


  get hasView() {
    return !!this.popup;
  }

  get isViewOpen() {
    // @ts-expect-error - MozSearchAutocompleteRichlistboxPopup is defined in JS and lacks type declarations.
    return this.popup && this.popup.popupOpen;
  }

  get selectedViewIndex() {
    // @ts-expect-error - MozSearchAutocompleteRichlistboxPopup is defined in JS and lacks type declarations.
    return this.popup.selectedIndex;
  }

  set selectedViewIndex(val) {
    // @ts-expect-error - MozSearchAutocompleteRichlistboxPopup is defined in JS and lacks type declarations.
    this.popup.selectedIndex = val;
  }

  closeView() {
    this.popup.hidePopup();
  }

  handleSearchCommand(event, engine, forceNewTab = false) {
    let { where, params } = this._whereToOpen(event, forceNewTab);
    // @ts-expect-error - MozSearchAutocompleteRichlistboxPopup is defined in JS and lacks type declarations.
    this.popup.handleOneOffSearch(event, engine, where, params);
  }

  setTooltipForEngineButton(button) {
    button.setAttribute("tooltiptext", button.engine.name);
  }


  _on_mousedown(event) {
    event.preventDefault();
  }

  _on_click(event) {
    if (event.button == 2) {
      return; 
    }

    let button = event.originalTarget;
    let engine = button.engine;

    if (!engine) {
      return;
    }

    if (!this.textbox.value) {
      if (event.shiftKey) {
        // @ts-expect-error - MozSearchAutocompleteRichlistboxPopup is defined in JS and lacks type declarations.
        this.popup.openSearchForm(event, engine);
      }
      return;
    }
    this.selectedButton = button;
    this.handleSearchCommand(event, engine);
  }

  async _on_command(event) {
    let target = event.target;

    if (target == this.settingsButton) {
      this.window.openPreferences("paneSearch");

      this.closeView();
      return;
    }

    if (target.classList.contains("searchbar-engine-one-off-add-engine")) {
      lazy.SearchUIUtils.addOpenSearchEngine(
        target.getAttribute("uri"),
        target.getAttribute("image"),
        this.window.gBrowser.selectedBrowser.browsingContext
      )
        .then(result => {
          if (result) {
            this._rebuild();
          }
        })
        .catch(console.error);
      return;
    }

    if (target.classList.contains("search-one-offs-context-open-in-new-tab")) {
      this.selectedButton = target.closest("menupopup")._triggerButton;
      if (this.textbox.value) {
        this.handleSearchCommand(event, this.selectedButton.engine, true);
      } else {
        // @ts-expect-error - MozSearchAutocompleteRichlistboxPopup is defined in JS and lacks type declarations.
        this.popup.openSearchForm(event, this.selectedButton.engine, true);
      }
    }

    const isPrivateButton = target.classList.contains(
      "search-one-offs-context-set-default-private"
    );
    if (
      target.classList.contains("search-one-offs-context-set-default") ||
      isPrivateButton
    ) {
      const engineType = isPrivateButton
        ? "defaultPrivateEngine"
        : "defaultEngine";
      let currentEngine = lazy.SearchService[engineType];

      const isPrivateWin = lazy.PrivateBrowsingUtils.isWindowPrivate(
        this.window
      );
      let button = target.closest("menupopup")._triggerButton;
      let newDefaultEngine = button.engine;
      if (
        !this.getAttribute("includecurrentengine") &&
        isPrivateButton == isPrivateWin
      ) {
        let iconURL =
          (await currentEngine.getIconURL()) ||
          "chrome://browser/skin/search-engine-placeholder.png";
        button.setAttribute("image", iconURL);
        button.setAttribute("tooltiptext", currentEngine.name);
        button.engine = currentEngine;
      }

      if (isPrivateButton) {
        lazy.SearchService.setDefaultPrivate(
          newDefaultEngine,
          lazy.SearchService.CHANGE_REASON.USER_SEARCHBAR_CONTEXT
        );
      } else {
        lazy.SearchService.setDefault(
          newDefaultEngine,
          lazy.SearchService.CHANGE_REASON.USER_SEARCHBAR_CONTEXT
        );
      }
    }
  }

  _on_contextmenu(event) {
    let target = event.originalTarget;
    if (
      !target.classList.contains("searchbar-engine-one-off-item") ||
      target.classList.contains("search-setting-button")
    ) {
      event.preventDefault();
      return;
    }
    this.contextMenuPopup
      .querySelector(".search-one-offs-context-set-default")
      .setAttribute(
        "disabled",
        target.engine == lazy.SearchService.defaultEngine
      );

    const privateDefaultItem = this.contextMenuPopup.querySelector(
      ".search-one-offs-context-set-default-private"
    );

    if (
      Services.prefs.getBoolPref(
        "browser.search.separatePrivateDefault.ui.enabled",
        false
      ) &&
      Services.prefs.getBoolPref("browser.search.separatePrivateDefault", false)
    ) {
      privateDefaultItem.hidden = false;
      privateDefaultItem.setAttribute(
        "disabled",
        target.engine == lazy.SearchService.defaultPrivateEngine
      );
    } else {
      privateDefaultItem.hidden = true;
    }

    this.contextMenuPopup._triggerButton = target;
    this.contextMenuPopup.openPopupAtScreen(event.screenX, event.screenY, true);
    event.preventDefault();
  }

  _on_input(event) {
    this.query = event.target.oneOffSearchQuery || event.target.value;
  }

  _on_popupshowing() {
    this._rebuild();
  }

  _on_popuphidden() {
    this.selectedButton = null;
  }
}
