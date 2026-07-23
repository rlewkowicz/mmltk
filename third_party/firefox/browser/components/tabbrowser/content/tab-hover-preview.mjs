/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

var { XPCOMUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
);
const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  PageWireframes: "resource:///modules/sessionstore/PageWireframes.sys.mjs",
});

const ZERO_DELAY_ACTIVATION_TIME = 300;

const HOVER_PANEL_STICKY_TIME = 100;

export default class TabHoverPanelSet {
  #win;

  #openPopups;

  #deactivateTimers;

  #activePanel;

  constructor(win) {
    XPCOMUtils.defineLazyPreferenceGetter(
      this,
      "_prefDisableAutohide",
      "ui.popup.disable_autohide",
      false
    );

    this.#win = win;
    this.#deactivateTimers = new WeakMap();
    this.#activePanel = null;

    this.panelOpener = new TabPreviewPanelTimedFunction(
      ZERO_DELAY_ACTIVATION_TIME,
      this.#win
    );

    const tabPreviewPanel =
      this.#win.document.getElementById("tab-preview-panel");

    this.tabPanel = new TabPanel(tabPreviewPanel, this);
    this.tabGroupPanel = new TabGroupPanel(
      this.#win.document.getElementById("tabgroup-preview-panel"),
      this
    );
    this.#setExternalPopupListeners();
    this.#win.gBrowser.tabContainer.addEventListener("dragstart", event => {
      const target = event.target.closest?.("tab, .tab-group-label");
      if (
        target &&
        (this.#win.gBrowser.isTab(target) ||
          this.#win.gBrowser.isTabGroupLabel(target))
      ) {
        this.deactivate(null, { force: true });
      }
    });
  }

  activate(tabOrGroup) {
    if (!this.shouldActivate()) {
      return;
    }

    if (this.#win.gBrowser.isTab(tabOrGroup)) {
      this.#setActivePanel(this.tabPanel);
      this.tabPanel.activate(tabOrGroup);
    } else if (this.#win.gBrowser.isTabGroup(tabOrGroup)) {
      if (!tabOrGroup.collapsed) {
        return;
      }

      this.#setActivePanel(this.tabGroupPanel);
      this.tabGroupPanel.activate(tabOrGroup);
    } else {
      throw new Error("Received activate call from unknown element");
    }
  }

  deactivate(tabOrGroup, { force = false } = {}) {
    if (this._prefDisableAutohide) {
      return;
    }

    if (this.#win.gBrowser.isTab(tabOrGroup) || !tabOrGroup) {
      this.tabPanel.deactivate(tabOrGroup, { force });
    }

    if (this.#win.gBrowser.isTabGroup(tabOrGroup) || !tabOrGroup) {
      this.tabGroupPanel.deactivate({ force });
    }
  }

  #setActivePanel(panel) {
    if (this.#activePanel && this.#activePanel != panel) {
      this.requestDeactivate(this.#activePanel, { force: true });
    }

    this.#activePanel = panel;
    this.#clearDeactivateTimer(panel);
  }

  requestDeactivate(panel, { force = false } = {}) {
    this.#clearDeactivateTimer(panel);
    if (force) {
      this.#doDeactivate(panel);
      return;
    }

    const timer = this.#win.setTimeout(() => {
      this.#deactivateTimers.delete(panel);
      if (panel.hoverTargets?.some(t => t.matches(":hover"))) {
        return;
      }
      this.#doDeactivate(panel);
    }, HOVER_PANEL_STICKY_TIME);
    this.#deactivateTimers.set(panel, timer);
  }

  #clearDeactivateTimer(panel) {
    const timer = this.#deactivateTimers.get(panel);
    if (timer) {
      this.#win.clearTimeout(timer);
      this.#deactivateTimers.delete(panel);
    }
  }

  #doDeactivate(panel) {
    panel.onBeforeHide();
    panel.panelElement.hidePopup();
    this.panelOpener.clear(panel);
    this.panelOpener.setZeroDelay();

    if (this.#activePanel == panel) {
      this.#activePanel = null;
    }
  }

  shouldActivate() {
    return (
      !this.#openPopups.size &&
      !this.#win.gBrowser.tabContainer.hasAttribute("movingtab") &&
      this.#win == Services.focus.activeWindow
    );
  }

  #setExternalPopupListeners() {

    const initialPopups = this.#win.document.querySelectorAll(
      `panel[panelopen=true]:not(#tab-preview-panel):not(#tabgroup-preview-panel),
       panel[animating=true]:not(#tab-preview-panel):not(#tabgroup-preview-panel),
       menupopup[open=true]`.trim()
    );
    this.#openPopups = new Set(initialPopups);

    const handleExternalPopupEvent = (eventName, setMethod) => {
      this.#win.addEventListener(eventName, ev => {
        const { target } = ev;
        if (
          target !== this.tabPanel.panelElement &&
          target !== this.tabGroupPanel.panelElement &&
          (target.nodeName == "panel" || target.nodeName == "menupopup")
        ) {
          this.#openPopups[setMethod](target);
        }
      });
    };
    handleExternalPopupEvent("popupshowing", "add");
    handleExternalPopupEvent("popuphiding", "delete");
  }
}

class HoverPanel {
  constructor(panelElement, panelSet) {
    this.panelElement = panelElement;
    this.panelSet = panelSet;
    this.win = this.panelElement.documentGlobal;
  }

  get isActive() {
    return this.panelElement.state == "open";
  }

  deactivate({ force = false } = {}) {
    this.panelSet.requestDeactivate(this, { force });
  }

  get hoverTargets() {
    return [this.panelElement];
  }

  onBeforeHide() {}
}

class TabPanel extends HoverPanel {
  #tab;

  #thumbnailElement;

  constructor(panel, panelSet) {
    super(panel, panelSet);

    XPCOMUtils.defineLazyPreferenceGetter(
      this,
      "_prefDisplayThumbnail",
      "browser.tabs.hoverPreview.showThumbnails",
      false
    );
    XPCOMUtils.defineLazyPreferenceGetter(
      this,
      "_prefCollectWireframes",
      "browser.history.collectWireframes"
    );
    this.#tab = null;
    this.#thumbnailElement = null;
  }

  async handleEvent(e) {
    switch (e.type) {
      case "popupshowing":
        await this.#updatePreview();
        break;
      case "TabAttrModified":
        this.#updatePreview(e.target);
        break;
      case "TabSelect":
        this.deactivate(null, { force: true });
        break;
    }
  }

  activate(tab) {
    if (this.#tab === tab && this.panelElement.state == "open") {
      return;
    }
    let originalTab = this.#tab;
    this.#tab = tab;

    this.#movePanel();

    originalTab?.removeEventListener("TabAttrModified", this);
    this.#tab.addEventListener("TabAttrModified", this);

    this.#thumbnailElement = null;
    this.#maybeRequestThumbnail();
    if (
      this.panelElement.state == "open" ||
      this.panelElement.state == "showing"
    ) {
      this.panelElement.removeEventListener("popupshowing", this);
      this.#updatePreview();
    } else {
      this.panelSet.panelOpener.execute(() => {
        if (!this.panelSet.shouldActivate()) {
          return;
        }
        this.panelElement.openPopup(this.#tab, this.popupOptions);
      }, this);
      this.win.addEventListener("TabSelect", this);
      this.panelElement.addEventListener("popupshowing", this);
    }
  }

  deactivate(leavingTab = null, { force = false } = {}) {
    if (leavingTab) {
      if (this.#tab != leavingTab) {
        return;
      }
      this.win.requestAnimationFrame(() => {
        if (this.#tab == leavingTab) {
          this.deactivate(null, { force });
        }
      });
      return;
    }
    super.deactivate({ force });
  }

  onBeforeHide() {
    this.panelElement.removeEventListener("popupshowing", this);
    this.win.removeEventListener("TabSelect", this);
    this.#tab?.removeEventListener("TabAttrModified", this);
    this.#tab = null;
    this.#thumbnailElement = null;
  }

  get hoverTargets() {
    return this.#tab ? [this.#tab] : [];
  }

  getPrettyURI(uri) {
    let url = URL.parse(uri);
    if (!url) {
      return uri;
    }

    if (url.protocol == "about:" && url.pathname == "reader") {
      url = URL.parse(url.searchParams.get("url"));
    }

    if (url?.protocol === "about:") {
      return url.href;
    }
    return url ? url.hostname.replace(/^w{3}\./, "") : uri;
  }

  #hasValidWireframeState(tab) {
    return (
      this._prefCollectWireframes &&
      this._prefDisplayThumbnail &&
      tab &&
      !tab.selected &&
      !!lazy.PageWireframes.getWireframeState(tab)
    );
  }

  #hasValidThumbnailState(tab) {
    return (
      this._prefDisplayThumbnail &&
      tab &&
      tab.linkedBrowser &&
      !tab.getAttribute("pending") &&
      !tab.selected
    );
  }

  #maybeRequestThumbnail() {
    let tab = this.#tab;

    if (!this.#hasValidThumbnailState(tab)) {
      let wireframeElement = lazy.PageWireframes.getWireframeElementForTab(tab);
      if (wireframeElement) {
        this.#thumbnailElement = wireframeElement;
        this.#updatePreview();
      }
      return;
    }
    let thumbnailCanvas = this.win.document.createElement("canvas");
    thumbnailCanvas.width = 280 * this.win.devicePixelRatio;
    thumbnailCanvas.height = 140 * this.win.devicePixelRatio;

    this.win.PageThumbs.captureTabPreviewThumbnail(
      tab.linkedBrowser,
      thumbnailCanvas
    )
      .then(() => {
        if (this.#tab == tab && this.#hasValidThumbnailState(tab)) {
          this.#thumbnailElement = thumbnailCanvas;
          this.#updatePreview();
        }
      })
      .catch(e => {
        console.error(e);
      });
  }

  get #displayTitle() {
    if (!this.#tab) {
      return "";
    }
    return this.#tab.textLabel.textContent;
  }

  get #displayURI() {
    if (!this.#tab || !this.#tab.linkedBrowser) {
      return "";
    }
    return this.getPrettyURI(this.#tab.linkedBrowser.currentURI.spec);
  }

  get #displayPids() {
    const pids = this.win.gBrowser.getTabPids(this.#tab);
    if (!pids.length) {
      return "";
    }

    let pidLabel = pids.length > 1 ? "pids" : "pid";
    return `${pidLabel}: ${pids.join(", ")}`;
  }

  get #displayActiveness() {
    return this.#tab?.linkedBrowser?.docShellIsActive ? "[A]" : "";
  }

  async #updatePreview(tab = null) {
    if (tab) {
      this.#tab = tab;
    }

    this.panelElement.querySelector(".tab-preview-title").textContent =
      this.#displayTitle;
    this.panelElement.querySelector(".tab-preview-uri").textContent =
      this.#displayURI;

    if (this.win.gBrowser.showPidAndActiveness) {
      this.panelElement.querySelector(".tab-preview-pid").textContent =
        this.#displayPids;
      this.panelElement.querySelector(".tab-preview-activeness").textContent =
        this.#displayActiveness;
    } else {
      this.panelElement.querySelector(".tab-preview-pid").textContent = "";
      this.panelElement.querySelector(".tab-preview-activeness").textContent =
        "";
    }

    let thumbnailContainer = this.panelElement.querySelector(
      ".tab-preview-thumbnail-container"
    );
    thumbnailContainer.classList.toggle(
      "hide-thumbnail",
      !this.#hasValidThumbnailState(this.#tab) &&
        !this.#hasValidWireframeState(this.#tab)
    );
    if (thumbnailContainer.firstChild != this.#thumbnailElement) {
      thumbnailContainer.replaceChildren();
      if (this.#thumbnailElement) {
        thumbnailContainer.appendChild(this.#thumbnailElement);
      }
      this.panelElement.dispatchEvent(
        new CustomEvent("previewThumbnailUpdated", {
          detail: {
            thumbnail: this.#thumbnailElement,
          },
        })
      );
    }

    this.#movePanel();
  }

  #movePanel() {
    if (this.#tab) {
      this.panelElement.moveToAnchor(
        this.#tab,
        this.popupOptions.position,
        this.popupOptions.x,
        this.popupOptions.y
      );
    }
  }

  get popupOptions() {
    let tabContainer = this.win.gBrowser.tabContainer;
    if (!tabContainer.verticalMode) {
      return {
        position: "bottomleft topleft",
        x: 0,
        y: 0,
      };
    }

    let sidebarAtStart = this.win.SidebarController._positionStart;

    let positionFromAnchor = sidebarAtStart ? "topright" : "topleft";
    let positionFromPanel = sidebarAtStart ? "topleft" : "topright";
    let positionX = 0;
    let positionY = 3;

    if (tabContainer.isContainerVerticalPinnedGrid(this.#tab)) {
      positionFromAnchor = sidebarAtStart ? "bottomright" : "bottomleft";
      positionX = sidebarAtStart ? -6 : 6;
      positionY = -10;
    }

    return {
      position: `${positionFromAnchor} ${positionFromPanel}`,
      x: positionX,
      y: positionY,
    };
  }
}

class TabGroupPanel extends HoverPanel {
  #group;

  static PANEL_UPDATE_EVENTS = [
    "TabAttrModified",
    "TabClose",
    "TabGrouped",
    "TabMove",
    "TabOpen",
    "TabSelect",
    "TabUngrouped",
  ];

  constructor(panel, panelSet) {
    super(panel, panelSet);

    this.panelContent = panel.querySelector("#tabgroup-panel-content");
    this.#group = null;
  }

  activate(group) {
    if (this.#group && this.#group != group) {
      this.#removeGroupListeners();
    }

    this.#group = group;
    this.#movePanel();
    this.#updatePanelContent();

    if (this.panelElement.state == "closed") {
      this.panelSet.panelOpener.execute(() => {
        if (!this.panelSet.shouldActivate() || !this.#group.collapsed) {
          return;
        }
        this.#doOpenPanel();
      }, this);
    } else {
      this.#addGroupListeners();
    }
  }

  focusPanel(dir = 1) {
    let childIndex = dir > 0 ? 0 : this.panelContent.children.length - 1;
    this.panelContent.children[childIndex].focus();
  }

  #doOpenPanel() {
    this.panelElement.addEventListener("mouseout", this);
    this.panelElement.addEventListener("command", this);

    this.#addGroupListeners();

    this.panelElement.openPopup(this.#popupTarget, this.popupOptions);
  }

  #updatePanelContent() {
    const fragment = this.win.document.createDocumentFragment();
    for (let tab of this.#group.tabs) {
      let tabbutton = this.win.document.createXULElement("toolbarbutton");
      tabbutton.setAttribute("role", "button");
      tabbutton.setAttribute("keyNav", false);
      tabbutton.setAttribute("tabindex", 0);
      tabbutton.setAttribute("label", tab.label);
      if (tab.linkedBrowser) {
        tabbutton.setAttribute(
          "image",
          "page-icon:" + tab.linkedBrowser.currentURI.spec
        );
      }
      tabbutton.setAttribute("tooltiptext", tab.label);
      tabbutton.classList.add(
        "subviewbutton",
        "subviewbutton-iconic",
        "group-preview-button"
      );
      if (tab == this.win.gBrowser.selectedTab) {
        tabbutton.classList.add("active-tab");
      }
      tabbutton.tab = tab;
      fragment.appendChild(tabbutton);
    }
    this.panelContent.replaceChildren(fragment);
  }

  handleEvent(event) {
    if (event.type == "command") {
      if (this.win.gBrowser.selectedTab == event.target.tab) {
        this.deactivate({ force: true });
        return;
      }

      let switchingTabs = [this.win.gBrowser.selectedTab, event.target.tab];
      if (switchingTabs.every(tab => tab.group == this.#group)) {
        for (let tab of switchingTabs) {
          tab.animationsEnabled = false;
        }

        this.win.addEventListener(
          "TabSwitchDone",
          () => {
            this.win.requestAnimationFrame(() => {
              for (let tab of switchingTabs) {
                tab.animationsEnabled = true;
              }
            });
          },
          { once: true }
        );
      }

      this.win.gBrowser.selectedTab = event.target.tab;
      this.deactivate({ force: true });
    } else if (
      event.type == "mouseout" &&
      this.hoverTargets.every(target => !target.contains(event.relatedTarget))
    ) {
      this.deactivate();
    } else if (TabGroupPanel.PANEL_UPDATE_EVENTS.includes(event.type)) {
      this.#updatePanelContent();
    }
  }

  onBeforeHide() {
    this.panelElement.removeEventListener("mouseout", this);
    this.panelElement.removeEventListener("command", this);

    this.#removeGroupListeners();
  }

  get hoverTargets() {
    let targets = [this.panelElement];
    if (this.#popupTarget) {
      targets.push(this.#popupTarget);
    }
    return targets;
  }

  get popupOptions() {
    if (!this.win.gBrowser.tabContainer.verticalMode) {
      return {
        position: "bottomleft topleft",
        x: 0,
        y: 0,
      };
    }
    if (!this.win.SidebarController._positionStart) {
      return {
        position: "topleft topright",
        x: 0,
        y: -5,
      };
    }
    return {
      position: "topright topleft",
      x: 0,
      y: -5,
    };
  }

  get #popupTarget() {
    return this.#group?.labelContainerElement;
  }

  #addGroupListeners() {
    if (!this.#group) {
      return;
    }
    this.#group.hoverPreviewPanelActive = true;
    for (let event of TabGroupPanel.PANEL_UPDATE_EVENTS) {
      this.#group.addEventListener(event, this);
    }
  }

  #removeGroupListeners() {
    if (!this.#group) {
      return;
    }
    this.#group.hoverPreviewPanelActive = false;
    for (let event of TabGroupPanel.PANEL_UPDATE_EVENTS) {
      this.#group.removeEventListener(event, this);
    }
  }

  #movePanel() {
    if (!this.#popupTarget) {
      return;
    }
    this.panelElement.moveToAnchor(
      this.#popupTarget,
      this.popupOptions.position,
      this.popupOptions.x,
      this.popupOptions.y
    );
  }
}

class TabPreviewPanelTimedFunction {
  #zeroDelayTime;

  #win;

  #timer;

  #useZeroDelay;

  #target;

  #from;

  constructor(zeroDelayTime, win) {
    XPCOMUtils.defineLazyPreferenceGetter(
      this,
      "_prefPreviewDelay",
      "ui.tooltip.delay_ms"
    );

    this.#zeroDelayTime = zeroDelayTime;
    this.#win = win;

    this.#timer = null;
    this.#useZeroDelay = false;

    this.#target = null;
    this.#from = null;
  }

  execute(target, from) {
    this.#target = target;
    this.#from = from;

    if (this.delayActive) {
      return;
    }

    this.#timer = this.#win.setTimeout(
      () => {
        this.#timer = null;
        this.#target();
      },
      this.#useZeroDelay ? 0 : this._prefPreviewDelay
    );
  }

  clear(from) {
    if (from == this.#from && this.#timer) {
      this.#win.clearTimeout(this.#timer);
      this.#timer = null;
      this.#from = null;
    }
  }

  setZeroDelay() {
    if (this.#useZeroDelay) {
      this.#win.clearTimeout(this.#useZeroDelay);
    }

    this.#useZeroDelay = this.#win.setTimeout(() => {
      this.#useZeroDelay = null;
    }, this.#zeroDelayTime);
  }

  get delayActive() {
    return this.#timer !== null;
  }
}
