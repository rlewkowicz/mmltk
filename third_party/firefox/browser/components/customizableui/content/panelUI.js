/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

ChromeUtils.importESModule(
  "chrome://browser/content/tabbrowser/tab-groups-list.mjs",
  { global: "current" }
);

ChromeUtils.defineESModuleGetters(this, {
  PanelMultiView:
    "moz-src:///browser/components/customizableui/PanelMultiView.sys.mjs",
  updateZoomUI: "resource:///modules/ZoomUI.sys.mjs",
});


const PanelUI = {
  get kEvents() {
    return ["popupshowing", "popupshown", "popuphiding", "popuphidden"];
  },

  get kElements() {
    return {
      multiView: "appMenu-multiView",
      menuButton: "PanelUI-menu-button",
      panel: "appMenu-popup",
      overflowFixedList: "widget-overflow-fixed-list",
      overflowPanel: "widget-overflow",
      navbar: "nav-bar",
    };
  },

  _initialized: false,

  init() {
    this._initElements();

    this.menuButton.addEventListener("mousedown", this);
    this.menuButton.addEventListener("keypress", this);

    XPCOMUtils.defineLazyPreferenceGetter(
      this,
      "tabGroupsAlternateMenu",
      "browser.tabs.groups.alternateMenu",
      false,
      (_pref, _previousValue, _newValue) => {
        this._showTabGroupsMenuItem();
      }
    );

    CustomizableUI.addListener(this);

    this.overflowFixedList.hidden = false;
    this.overflowFixedList.previousElementSibling.hidden = false;
    CustomizableUI.registerPanelNode(
      this.overflowFixedList,
      CustomizableUI.AREA_FIXED_OVERFLOW_PANEL
    );
    this.updateOverflowStatus();

    this._showTabGroupsMenuItem();
    this._initialized = true;
  },

  _initElements() {
    for (let [k, v] of Object.entries(this.kElements)) {
      let getKey = k;
      let id = v;
      this.__defineGetter__(getKey, function () {
        delete this[getKey];
        return (this[getKey] = document.getElementById(id));
      });
    }
  },

  _eventListenersAdded: false,
  _ensureEventListenersAdded() {
    if (this._eventListenersAdded) {
      return;
    }
    this._addEventListeners();
  },

  _addEventListeners() {
    for (let event of this.kEvents) {
      this.panel.addEventListener(event, this);
    }

    let helpView = PanelMultiView.getViewNode(document, "PanelUI-helpView");
    helpView.addEventListener("ViewShowing", this._onHelpViewShow);
    helpView.addEventListener("command", this._onHelpCommand);
    this._onLibraryCommand = this._onLibraryCommand.bind(this);
    PanelMultiView.getViewNode(
      document,
      "appMenu-libraryView"
    ).addEventListener("command", this._onLibraryCommand);
    this.mainView.addEventListener("command", this);
    this.mainView.addEventListener("ViewShowing", this._onMainViewShow);
    this._eventListenersAdded = true;
  },

  _removeEventListeners() {
    for (let event of this.kEvents) {
      this.panel.removeEventListener(event, this);
    }
    let helpView = PanelMultiView.getViewNode(document, "PanelUI-helpView");
    helpView.removeEventListener("ViewShowing", this._onHelpViewShow);
    helpView.removeEventListener("command", this._onHelpCommand);
    PanelMultiView.getViewNode(
      document,
      "appMenu-libraryView"
    ).removeEventListener("command", this._onLibraryCommand);
    this.mainView.removeEventListener("command", this);
    this._eventListenersAdded = false;
  },

  uninit() {
    this._removeEventListeners();

    this.menuButton.removeEventListener("mousedown", this);
    this.menuButton.removeEventListener("keypress", this);
    CustomizableUI.removeListener(this);
  },

  toggle(aEvent) {
    if (document.documentElement.hasAttribute("customizing")) {
      return;
    }
    this._ensureEventListenersAdded();
    if (this.panel.state == "open") {
      this.hide();
    } else if (this.panel.state == "closed") {
      this.show(aEvent);
    }
  },

  show(aEvent) {
    this._ensureShortcutsShown();
    (async () => {
      await this.ensureReady();

      if (
        this.panel.state == "open" ||
        document.documentElement.hasAttribute("customizing")
      ) {
        return;
      }

      let domEvent = null;
      if (aEvent && aEvent.type != "command") {
        domEvent = aEvent;
      }

      let anchor = this._getPanelAnchor(this.menuButton);
      await PanelMultiView.openPopup(this.panel, anchor, {
        triggerEvent: domEvent,
      });
    })().catch(console.error);
  },

  hide() {
    if (document.documentElement.hasAttribute("customizing")) {
      return;
    }

    PanelMultiView.hidePopup(this.panel);
  },

  handleEvent(aEvent) {
    if (aEvent.type.startsWith("popup") && aEvent.target != this.panel) {
      return;
    }
    switch (aEvent.type) {
      case "popupshowing":
        updateEditUIVisibility();
      case "popupshown":
        if (aEvent.type == "popupshown") {
          CustomizableUI.addPanelCloseListeners(this.panel);
        }
      case "popuphiding":
        if (aEvent.type == "popuphiding") {
          updateEditUIVisibility();
        }
      case "popuphidden":
        this._updatePanelButton(aEvent.target);
        if (aEvent.type == "popuphidden") {
          CustomizableUI.removePanelCloseListeners(this.panel);
        }
        break;
      case "mousedown":
        if (
          aEvent.button == 0 &&
          (AppConstants.platform != "macosx" || !aEvent.ctrlKey)
        ) {
          this.toggle(aEvent);
        }
        break;
      case "keypress":
        if (aEvent.key == " " || aEvent.key == "Enter") {
          this.toggle(aEvent);
          aEvent.stopPropagation();
        }
        break;
      case "command":
        this.onCommand(aEvent);
        break;
    }
  },

  onCommand(aEvent) {
    let { target } = aEvent;
    switch (target.id) {
      case "appMenu-bookmarks-button":
        BookmarkingUI.showSubView(target);
        break;
      case "appMenu-history-button":
        this.showSubView("PanelUI-history", target);
        break;
      case "appMenu-tab-groups-button":
        this.showSubView("appMenu-tabGroupsListView", target);
        break;
      case "appMenu-fullscreen-button2":
        target.closest("panel").hidePopup();
        setTimeout(() => BrowserCommands.fullScreen(), 0);
        break;
      case "appMenu-settings-button":
        openPreferences();
        break;
      case "appMenu-more-button2":
        this.showMoreToolsPanel(target);
        break;
      case "appMenu-help-button2":
        this.showSubView("PanelUI-helpView", target);
        break;
    }
  },

  get isReady() {
    return !!this._isReady;
  },

  async ensureReady() {
    if (this._isReady) {
      return;
    }

    await window.delayedStartupPromise;
    this._ensureEventListenersAdded();
    this.panel.hidden = false;
    this._isReady = true;
  },

  showHelpView(aAnchor) {
    this._ensureEventListenersAdded();
    this.multiView.showSubView("PanelUI-helpView", aAnchor);
  },

  showMoreToolsPanel(moreTools) {
    this.showSubView("appmenu-moreTools", moreTools);
  },

  async showSubView(aViewId, aAnchor, aEvent) {
    if (aEvent) {
      if (
        aEvent.type == "mousedown" &&
        (aEvent.button != 0 ||
          (AppConstants.platform == "macosx" && aEvent.ctrlKey))
      ) {
        return;
      }
      if (
        aEvent.type == "keypress" &&
        aEvent.key != " " &&
        aEvent.key != "Enter"
      ) {
        return;
      }
    }

    this._ensureEventListenersAdded();

    let viewNode = PanelMultiView.getViewNode(document, aViewId);
    if (!viewNode) {
      console.error("Could not show panel subview with id: ", aViewId);
      return;
    }

    if (!aAnchor) {
      console.error(
        "Expected an anchor when opening subview with id: ",
        aViewId
      );
      return;
    }

    this._ensureShortcutsShown(viewNode);

    let container = aAnchor.closest("panelmultiview");
    if (container && !viewNode.hasAttribute("disallowSubView")) {
      container.showSubView(aViewId, aAnchor);
    } else if (!aAnchor.open) {
      aAnchor.open = true;

      let tempPanel = document.createXULElement("panel");
      tempPanel.setAttribute("type", "arrow");
      tempPanel.setAttribute("id", "customizationui-widget-panel");
      if (viewNode.hasAttribute("neverhidden")) {
        tempPanel.setAttribute("neverhidden", "true");
      }

      tempPanel.setAttribute("class", "cui-widget-panel panel-no-padding");
      tempPanel.setAttribute("viewId", aViewId);
      if (aAnchor.getAttribute("tabspecific")) {
        tempPanel.setAttribute("tabspecific", true);
      }
      if (aAnchor.getAttribute("locationspecific")) {
        tempPanel.setAttribute("locationspecific", true);
      }
      if (this._disableAnimations) {
        tempPanel.setAttribute("animate", "false");
      }
      tempPanel.setAttribute("context", "");

      document.getElementById("mainPopupSet").appendChild(tempPanel);

      let multiView = document.createXULElement("panelmultiview");
      multiView.setAttribute("id", "customizationui-widget-multiview");
      multiView.setAttribute("viewCacheId", "appMenu-viewCache");
      multiView.setAttribute("mainViewId", viewNode.id);
      multiView.appendChild(viewNode);
      tempPanel.appendChild(multiView);
      viewNode.classList.add("cui-widget-panelview", "PanelUI-subView");

      tempPanel.role = viewNode.dataset.panelrole || "group";
      if (viewNode.dataset.panelname) {
        tempPanel.ariaLabel = viewNode.dataset.panelname;
      } else {
        tempPanel.ariaLabelledByElements = [aAnchor];
      }

      let viewShown = false;
      let panelRemover = event => {
        if (event && event.target != tempPanel) {
          return;
        }
        viewNode.classList.remove("cui-widget-panelview");
        if (viewShown) {
          CustomizableUI.removePanelCloseListeners(tempPanel);
          tempPanel.removeEventListener("popuphidden", panelRemover);
        }
        aAnchor.open = false;

        PanelMultiView.removePopup(tempPanel);
      };

      if (aAnchor.parentNode.id == "PersonalToolbar") {
        tempPanel.classList.add("bookmarks-toolbar");
      }

      let anchor = this._getPanelAnchor(aAnchor);

      if (aAnchor != anchor && aAnchor.id) {
        anchor.setAttribute("consumeanchor", aAnchor.id);
      }

      try {
        viewShown = await PanelMultiView.openPopup(tempPanel, anchor, {
          position: "bottomright topright",
          triggerEvent: aEvent,
        });
      } catch (ex) {
        console.error(ex);
      }

      if (viewShown) {
        CustomizableUI.addPanelCloseListeners(tempPanel);
        tempPanel.addEventListener("popuphidden", panelRemover);
      } else {
        panelRemover();
      }
    }
  },

  disableSingleSubviewPanelAnimations() {
    this._disableAnimations = true;
  },

  enableSingleSubviewPanelAnimations() {
    this._disableAnimations = false;
  },

  updateOverflowStatus() {
    let hasKids = this.overflowFixedList.hasChildNodes();
    if (hasKids && !this.navbar.hasAttribute("nonemptyoverflow")) {
      this.navbar.setAttribute("nonemptyoverflow", "true");
      this.overflowPanel.setAttribute("hasfixeditems", "true");
    } else if (!hasKids && this.navbar.hasAttribute("nonemptyoverflow")) {
      PanelMultiView.hidePopup(this.overflowPanel);
      this.overflowPanel.removeAttribute("hasfixeditems");
      this.navbar.removeAttribute("nonemptyoverflow");
    }
  },

  onWidgetAfterDOMChange(aNode, aNextNode, aContainer) {
    if (aContainer == this.overflowFixedList) {
      this.updateOverflowStatus();
    }
  },

  onAreaReset(aArea, aContainer) {
    if (aContainer == this.overflowFixedList) {
      this.updateOverflowStatus();
    }
  },

  _updatePanelButton() {
    let { state } = this.panel;
    if (state == "open" || state == "showing") {
      this.menuButton.open = true;
      document.l10n.setAttributes(
        this.menuButton,
        "appmenu-menu-button-opened2"
      );
    } else {
      this.menuButton.open = false;
      document.l10n.setAttributes(
        this.menuButton,
        "appmenu-menu-button-closed2"
      );
    }
  },

  _onMainViewShow() {
    updateZoomUI(gBrowser.selectedBrowser);
  },

  _onHelpViewShow() {

    let helpMenu = document.getElementById("menu_HelpPopup");
    let items = this.getElementsByTagName("vbox")[0];
    let attrs = ["command", "onclick", "key", "disabled", "accesskey", "label"];

    while (items.firstChild) {
      items.firstChild.remove();
    }

    let menuItems = Array.prototype.slice.call(
      helpMenu.getElementsByTagName("menuitem")
    );
    let fragment = document.createDocumentFragment();
    for (let node of menuItems) {
      if (node.hidden) {
        continue;
      }
      let button = document.createXULElement("toolbarbutton");
      for (let attrName of attrs) {
        if (!node.hasAttribute(attrName)) {
          continue;
        }
        button.setAttribute(attrName, node.getAttribute(attrName));
      }

      let l10nId = node.getAttribute("appmenu-data-l10n-id");
      if (l10nId) {
        document.l10n.setAttributes(button, l10nId);
      }

      if (node.id) {
        button.id = "appMenu_" + node.id;
      }

      button.classList.add("subviewbutton");
      fragment.appendChild(button);
    }

    items.appendChild(fragment);

  },

  _onHelpCommand(aEvent) {
    switch (aEvent.target.id) {
      case "appMenu_menu_openHelp":
        openHelpLink("firefox-help");
        break;
      case "appMenu_menu_layout_debugger":
        toOpenWindowByType(
          "mozapp:layoutdebug",
          "chrome://layoutdebug/content/layoutdebug.xhtml"
        );
        break;
      case "appMenu_feedbackPage":
        openFeedbackPage();
        break;
      case "appMenu_helpSafeMode":
        safeModeRestart();
        break;
      case "appMenu_troubleShooting":
        openTroubleshootingPage();
        break;
      case "appMenu_aboutName":
        openAboutDialog();
        break;
    }
  },

  _onLibraryCommand(aEvent) {
    let button = aEvent.target;
    let { BookmarkingUI, DownloadsPanel } = button.documentGlobal;
    switch (button.id) {
      case "appMenu-library-bookmarks-button":
        BookmarkingUI.showSubView(button);
        break;
      case "appMenu-library-history-button":
        this.showSubView("PanelUI-history", button);
        break;
      case "appMenu-library-downloads-button":
        DownloadsPanel.showDownloadsHistory();
        break;
    }
  },

  async selectAndMarkItem(itemIds) {
    if (document.documentElement.hasAttribute("customizing")) {
      return;
    }

    if (this.panel.state == "hiding") {
      await new Promise(resolve => {
        this.panel.addEventListener("popuphidden", resolve, { once: true });
      });
    }

    if (this.panel.state != "open") {
      await new Promise(resolve => {
        this.panel.addEventListener("ViewShown", resolve, { once: true });
        this.show();
      });
    }

    let currentView;

    let viewShownCB = event => {
      viewHidingCB();

      if (itemIds.length) {
        let subItem = window.document.getElementById(itemIds[0]);
        if (event.target.id == subItem?.closest("panelview")?.id) {
          Services.tm.dispatchToMainThread(() => {
            markItem(event.target);
          });
        } else {
          itemIds = [];
        }
      }
    };

    let viewHidingCB = () => {
      if (currentView) {
        currentView.ignoreMouseMove = false;
      }
      currentView = null;
    };

    let popupHiddenCB = () => {
      viewHidingCB();
      this.panel.removeEventListener("ViewShown", viewShownCB);
    };

    let markItem = viewNode => {
      let id = itemIds.shift();
      let item = window.document.getElementById(id);
      item.setAttribute("tabindex", "-1");

      currentView = PanelView.forNode(viewNode);
      currentView.selectedElement = item;
      currentView.focusSelectedElement(true);

      currentView.ignoreMouseMove = true;

      if (itemIds.length) {
        this.panel.addEventListener("ViewShown", viewShownCB, { once: true });
      }
      this.panel.addEventListener("ViewHiding", viewHidingCB, { once: true });
    };

    this.panel.addEventListener("popuphidden", popupHiddenCB, { once: true });
    markItem(this.mainView);
  },

  get mainView() {
    if (!this._mainView) {
      this._mainView = PanelMultiView.getViewNode(document, "appMenu-mainView");
    }
    return this._mainView;
  },

  _showTabGroupsMenuItem() {
    const button = PanelMultiView.getViewNode(
      document,
      "appMenu-tab-groups-button"
    );
    button.hidden = !this.tabGroupsAlternateMenu;
  },

  _getPanelAnchor(candidate) {
    let iconAnchor = candidate.badgeStack || candidate.icon;
    return iconAnchor || candidate;
  },

  _ensureShortcutsShown(view = this.mainView) {
    if (view.hasAttribute("added-shortcuts")) {
      return;
    }
    view.setAttribute("added-shortcuts", "true");
    for (let button of view.querySelectorAll("toolbarbutton[key]")) {
      let keyId = button.getAttribute("key");
      let key = document.getElementById(keyId);
      if (!key) {
        continue;
      }
      button.setAttribute("shortcut", ShortcutUtils.prettifyShortcut(key));
    }
  },
};

XPCOMUtils.defineConstant(this, "PanelUI", PanelUI);
