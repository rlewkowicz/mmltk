/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


const XPCOMUtils = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
).XPCOMUtils;

const lazy = XPCOMUtils.declareLazy({
  srdPromoDismissed: {
    pref: "browser.settings-redesign.promo.dismissed",
    default: false,
  },
  srdEnabled: { pref: "browser.settings-redesign.enabled", default: false },
});

export function friendlyPrefCategoryNameToInternalName(categoryName) {
  if (categoryName.startsWith("pane")) {
    return categoryName;
  }
  return (
    "pane" + categoryName.substring(0, 1).toUpperCase() + categoryName.substr(1)
  );
}

export const SettingPaneManager = {
  _data: new Map(),

  get(id) {
    if (!this._data.has(id)) {
      throw new Error(`Setting pane "${id}" not found`);
    }
    return this._data.get(id);
  },

  getWithParents(id) {
    let configs = [this.get(id)];
    while (configs[0].parent) {
      configs.unshift(this.get(configs[0].parent));
    }
    return configs;
  },

  importPane(id) {
    for (let config of this.getWithParents(id)) {
      if (config.module) {
        ChromeUtils.importESModule(config.module, { global: "current" });
      }
    }
  },

  registerPane(id, config) {
    if (this._data.has(id)) {
      throw new Error(`Setting pane "${id}" already registered`);
    }
    let fullConfig = { ...config, id };
    this._data.set(id, fullConfig);
    if (!fullConfig.groupIds.length) {
      return;
    }
    let subPane = friendlyPrefCategoryNameToInternalName(id);
    let settingPane =  (
      document.createElement("setting-pane")
    );
    settingPane.name = subPane;
    settingPane.config = fullConfig;
    settingPane.isSubPane = !!config.parent;

    settingPane.showRedesignPromo = this.shouldShowRedesignPromo;

    document.getElementById("mainPrefPane").append(settingPane);
    window.register_module(subPane, {
      init() {
        settingPane.init();
      },
    });
  },

  registerPanes(paneConfigs) {
    for (let id in paneConfigs) {
      this.registerPane(id, paneConfigs[id]);
    }
  },

  get shouldShowRedesignPromo() {
    return lazy.srdEnabled && !lazy.srdPromoDismissed;
  },
};

document.addEventListener("settings-redesign-promo-dismiss", () => {
  Services.prefs.setBoolPref("browser.settings-redesign.promo.dismissed", true);
});
