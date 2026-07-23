/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const ENABLED_PREF = "browser.privatebrowsing.resetPBM.enabled";
const SHOW_CONFIRM_DIALOG_PREF =
  "browser.privatebrowsing.resetPBM.showConfirmationDialog";

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  CustomizableUI:
    "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  SessionStore: "resource:///modules/sessionstore/SessionStore.sys.mjs",
});

export const ResetPBMPanel = {
  _widgetConfig: null,

  init() {
    this._widgetConfig ??= {
      id: "reset-pbm-toolbar-button",
      l10nId: "reset-pbm-toolbar-button2",
      type: "view",
      viewId: "reset-pbm-panel",
      defaultArea: lazy.CustomizableUI.AREA_NAVBAR,
      onViewShowing(aEvent) {
        ResetPBMPanel.onViewShowing(aEvent);
      },
      onViewHiding(aEvent) {
        ResetPBMPanel.onViewHiding(aEvent);
      },
      hideInNonPrivateBrowsing: true,
    };

    if (this._enabled) {
      lazy.CustomizableUI.createWidget(this._widgetConfig);
    } else {
      lazy.CustomizableUI.destroyWidget(this._widgetConfig.id);
    }
  },

  async onViewShowing(event) {
    let panelview = event.target;
    let triggeringWindow = panelview.documentGlobal;

    if (!this._shouldConfirmClear) {
      event.preventDefault();

      lazy.CustomizableUI.hidePanelForNode(panelview);

      await this._restartPBM(triggeringWindow);

      return;
    }

    panelview.addEventListener("command", this);

    this._rememberCheck(triggeringWindow).checked = this._shouldConfirmClear;

  },

  onViewHiding(event) {
    let panelview = event.target;
    panelview.removeEventListener("command", this);
  },

  handleEvent(event) {
    let button = event.target;
    switch (button.id) {
      case "reset-pbm-panel-cancel-button":
        this.onCancel(button);
        break;
      case "reset-pbm-panel-confirm-button":
        this.onConfirm(button);
        break;
    }
  },

  onCancel(button) {
    if (!this._enabled) {
      throw new Error("Not initialized.");
    }
    lazy.CustomizableUI.hidePanelForNode(button);

  },

  async onConfirm(button) {
    if (!this._enabled) {
      throw new Error("Not initialized.");
    }
    let triggeringWindow = button.documentGlobal;

    Services.prefs.setBoolPref(
      SHOW_CONFIRM_DIALOG_PREF,
      this._rememberCheck(triggeringWindow).checked
    );

    lazy.CustomizableUI.hidePanelForNode(button);


    await this._restartPBM(triggeringWindow);

  },

  async _restartPBM(triggeringWindow) {
    if (
      !triggeringWindow ||
      !lazy.PrivateBrowsingUtils.isWindowPrivate(triggeringWindow)
    ) {
      throw new Error("Invalid triggering window.");
    }

    for (let w of Services.ww.getWindowEnumerator()) {
      if (
        w != triggeringWindow &&
        lazy.PrivateBrowsingUtils.isWindowPrivate(w)
      ) {
        w.closeWindow?.(true, null, "restart-pbm");
      }
    }

    let newTab = triggeringWindow.gBrowser.addTab(
      triggeringWindow.BROWSER_NEW_TAB_URL,
      {
        triggeringPrincipal:
          Services.scriptSecurityManager.getSystemPrincipal(),
      }
    );
    if (!newTab) {
      throw new Error("Could not open new tab.");
    }

    triggeringWindow.gBrowser.removeAllTabsBut(newTab, {
      skipPermitUnload: true,
      skipSessionStore: true,
      animate: false,
      skipWarnAboutClosingTabs: true,
      skipPinnedOrSelectedTabs: false,
    });

    triggeringWindow.SidebarController?.hide();

    lazy.SessionStore.purgeDataForPrivateWindow(triggeringWindow);

    await new Promise(resolve => {
      Services.clearData.clearPrivateBrowsingData({
        onDataDeleted(aFailedFlags) {
          if (aFailedFlags) {
            console.error("PBM cleanup failed with flags:", aFailedFlags);
          }
          resolve();
        },
      });
    });


    let toolbarButton = this._toolbarButton(triggeringWindow);

    let anchor;
    let anchorID = toolbarButton.getAttribute("cui-anchorid");
    if (anchorID) {
      anchor = triggeringWindow.document.getElementById(anchorID);
    }
    triggeringWindow.ConfirmationHint.show(
      anchor ?? toolbarButton,
      "reset-pbm-panel-complete",
      { position: "bottomright topright" }
    );
  },

  _toolbarButton(win) {
    return lazy.CustomizableUI.getWidget(this._widgetConfig.id).forWindow(win)
      .node;
  },

  _rememberCheck(win) {
    return win.document.getElementById("reset-pbm-panel-checkbox");
  },
};

XPCOMUtils.defineLazyPreferenceGetter(
  ResetPBMPanel,
  "_enabled",
  ENABLED_PREF,
  false,
  ResetPBMPanel.init.bind(ResetPBMPanel)
);
XPCOMUtils.defineLazyPreferenceGetter(
  ResetPBMPanel,
  "_shouldConfirmClear",
  SHOW_CONFIRM_DIALOG_PREF,
  true
);
