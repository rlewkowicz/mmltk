/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ASRouter: "resource:///modules/asrouter/ASRouter.sys.mjs",
  CustomizableUI:
    "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs",
  IPProtectionPanel:
    "moz-src:///browser/components/ipprotection/IPProtectionPanel.sys.mjs",
  IPProtectionService:
    "moz-src:///toolkit/components/ipprotection/IPProtectionService.sys.mjs",
  IPProtectionToolbarButton:
    "moz-src:///browser/components/ipprotection/IPProtectionToolbarButton.sys.mjs",
  IPPProxyManager:
    "moz-src:///toolkit/components/ipprotection/IPPProxyManager.sys.mjs",
  requestIdleCallback: "resource://gre/modules/Timer.sys.mjs",
  cancelIdleCallback: "resource://gre/modules/Timer.sys.mjs",
});

const FXA_WIDGET_ID = "fxa-toolbar-menu-button";
const EXT_WIDGET_ID = "unified-extensions-button";

class IPProtectionWidget {
  static WIDGET_ID = "ipprotection-button";
  static PANEL_ID = "PanelUI-ipprotection";

  static ENABLED_PREF = "browser.ipProtection.enabled";
  static ADDED_PREF = "browser.ipProtection.added";

  #inited = false;
  created = false;
  #panels = new WeakMap();
  #toolbarButtons = new WeakMap();

  constructor() {
    this.sendReadyTrigger = this.#sendReadyTrigger.bind(this);
  }

  init() {
    if (this.#inited) {
      return;
    }
    this.#inited = true;

    if (!this.created) {
      this.#createWidget();
    }

    lazy.CustomizableUI.addListener(this);
  }

  uninit() {
    if (!this.#inited) {
      return;
    }
    this.#destroyWidget();
    this.#uninitPanels();

    lazy.CustomizableUI.removeListener(this);

    this.#inited = false;
  }

  get isInitialized() {
    return this.#inited;
  }

  #createWidget() {
    const onViewShowing = this.#onViewShowing.bind(this);
    const onViewHiding = this.#onViewHiding.bind(this);
    const onBeforeCreated = this.#onBeforeCreated.bind(this);
    const onCreated = this.#onCreated.bind(this);
    const onDestroyed = this.#onDestroyed.bind(this);
    const item = {
      id: IPProtectionWidget.WIDGET_ID,
      l10nId: "ipprotection-button",
      type: "view",
      viewId: IPProtectionWidget.PANEL_ID,
      onViewShowing,
      onViewHiding,
      onBeforeCreated,
      onCreated,
      onDestroyed,
      disallowSubView: true, 
    };
    lazy.CustomizableUI.createWidget(item);

    this.#placeWidget();

    this.created = true;
  }

  #placeWidget() {
    let wasAddedToToolbar = Services.prefs.getBoolPref(
      IPProtectionWidget.ADDED_PREF,
      false
    );
    let alreadyPlaced = lazy.CustomizableUI.getPlacementOfWidget(
      IPProtectionWidget.WIDGET_ID,
      false,
      true
    );
    if (wasAddedToToolbar || alreadyPlaced) {
      return;
    }

    let prevWidget =
      lazy.CustomizableUI.getPlacementOfWidget(FXA_WIDGET_ID) ||
      lazy.CustomizableUI.getPlacementOfWidget(EXT_WIDGET_ID);
    let pos = prevWidget ? prevWidget.position : null;

    lazy.CustomizableUI.addWidgetToArea(
      IPProtectionWidget.WIDGET_ID,
      lazy.CustomizableUI.AREA_NAVBAR,
      pos
    );
    Services.prefs.setBoolPref(IPProtectionWidget.ADDED_PREF, true);
  }

  #destroyWidget() {
    if (!this.created) {
      return;
    }
    this.#destroyPanels();
    lazy.CustomizableUI.destroyWidget(IPProtectionWidget.WIDGET_ID);
    this.created = false;
    if (this.readyTriggerIdleCallback) {
      lazy.cancelIdleCallback(this.readyTriggerIdleCallback);
    }
  }

  getPanel(window) {
    if (!this.created || !window?.PanelUI) {
      return null;
    }

    if (!this.#panels.has(window)) {
      let panel = new lazy.IPProtectionPanel(window);
      this.#panels.set(window, panel);
    }

    return this.#panels.get(window);
  }

  getToolbarButton(window) {
    if (!this.created) {
      return null;
    }

    return this.#toolbarButtons.get(window);
  }

  #destroyPanels() {
    let panels = ChromeUtils.nondeterministicGetWeakMapKeys(this.#panels);
    for (let panel of panels) {
      this.#panels.get(panel).destroy();
    }
  }

  #uninitPanels() {
    let panels = ChromeUtils.nondeterministicGetWeakMapKeys(this.#panels);
    for (let panel of panels) {
      this.#panels.get(panel).uninit();
    }

    let toolbarButtons = ChromeUtils.nondeterministicGetWeakMapKeys(
      this.#toolbarButtons
    );
    for (let toolbarButton of toolbarButtons) {
      this.#toolbarButtons.get(toolbarButton).uninit();
    }

    this.#panels = new WeakMap();
    this.#toolbarButtons = new WeakMap();
  }

  #onViewShowing(event) {
    let { documentGlobal } = event.target;
    if (this.#panels.has(documentGlobal)) {
      let panel = this.#panels.get(documentGlobal);
      panel.showing(event.target);
    }
  }

  #onViewHiding(event) {
    let { documentGlobal } = event.target;
    if (this.#panels.has(documentGlobal)) {
      let panel = this.#panels.get(documentGlobal);
      panel.hiding();
    }
  }

  #onBeforeCreated(doc) {
    let { documentGlobal } = doc;
    if (documentGlobal && !this.#panels.has(documentGlobal)) {
      let panel = new lazy.IPProtectionPanel(documentGlobal);
      this.#panels.set(documentGlobal, panel);
    }
  }

  #onCreated(toolbaritem) {
    let window = toolbaritem.documentGlobal;
    if (window && !this.#toolbarButtons.has(window)) {
      let toolbarButton = new lazy.IPProtectionToolbarButton(
        window,
        IPProtectionWidget.WIDGET_ID,
        toolbaritem
      );
      this.#toolbarButtons.set(window, toolbarButton);
    }

    this.readyTriggerIdleCallback = lazy.requestIdleCallback(
      this.sendReadyTrigger
    );

    lazy.IPProtectionService.addEventListener(
      "IPProtectionService:StateChanged",
      this.handleEvent
    );
    lazy.IPPProxyManager.addEventListener(
      "IPPProxyManager:StateChanged",
      this.handleEvent
    );
  }

  #onDestroyed() {
    lazy.IPPProxyManager.removeEventListener(
      "IPPProxyManager:StateChanged",
      this.handleEvent
    );
    lazy.IPProtectionService.removeEventListener(
      "IPProtectionService:StateChanged",
      this.handleEvent
    );
  }

  async onWidgetRemoved(widgetId) {
    if (widgetId != IPProtectionWidget.WIDGET_ID) {
      return;
    }

    await Promise.resolve();
    let moved = !!lazy.CustomizableUI.getPlacementOfWidget(widgetId);
    if (!moved) {
      lazy.IPPProxyManager.stop();
      let toolbarButtons = ChromeUtils.nondeterministicGetWeakMapKeys(
        this.#toolbarButtons
      );
      for (let win of toolbarButtons) {
        this.#toolbarButtons.get(win)?.updateState();
      }
    }
  }

  async #sendReadyTrigger() {
    await lazy.ASRouter.waitForInitialized;
    const win = Services.wm.getMostRecentBrowserWindow();
    const browser = win?.gBrowser?.selectedBrowser;
    await lazy.ASRouter.sendTriggerMessage({
      browser,
      id: "ipProtectionReady",
    });
  }
}

const IPProtection = new IPProtectionWidget();

export { IPProtection, IPProtectionWidget };
