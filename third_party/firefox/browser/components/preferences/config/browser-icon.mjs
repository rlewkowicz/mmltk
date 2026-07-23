/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import { Preferences } from "chrome://global/content/preferences/Preferences.mjs";
import { SettingGroupManager } from "chrome://browser/content/preferences/config/SettingGroupManager.mjs";
import { DefaultBrowserHelper } from "chrome://browser/content/preferences/DefaultBrowserHelper.mjs";

const { XPCOMUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
);

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  CustomIconManager:
    "moz-src:///browser/components/shell/CustomIconManager.sys.mjs",
  ICON_CATALOG: "moz-src:///browser/components/shell/CustomIconManager.sys.mjs",
  resolvePreview:
    "moz-src:///browser/components/shell/CustomIconManager.sys.mjs",
});

const COLOR_SCHEME_QUERY = matchMedia("(prefers-color-scheme: dark)");
function currentScheme() {
  return COLOR_SCHEME_QUERY.matches ? "dark" : "light";
}
function watchScheme(emitChange) {
  COLOR_SCHEME_QUERY.addEventListener("change", emitChange);
  return () => COLOR_SCHEME_QUERY.removeEventListener("change", emitChange);
}
XPCOMUtils.defineLazyServiceGetters(lazy, {
  WinTaskbar: ["@mozilla.org/windows-taskbar;1", Ci.nsIWinTaskbar],
});

const PREF_ICON_ID = "browser.shell.customIcon.id";

let gIsDefault = false;
let gIsPinned = false;
let gIconsUnlocked = false;

const gUnlockListeners = new Set();

let gPollUnsubscribe = null;

async function refreshUnlockState() {
  let isDefault = false;
  let isPinned = false;
  try {
    isDefault = DefaultBrowserHelper.isBrowserDefault;
    isPinned = await window
      .getShellService()
      .shellService.isCurrentAppPinnedToTaskbar(lazy.WinTaskbar.defaultGroupId);
  } catch (ex) {
    isDefault = false;
    isPinned = false;
  }
  let unlocked = isDefault && isPinned;
  if (
    isDefault !== gIsDefault ||
    isPinned !== gIsPinned ||
    unlocked !== gIconsUnlocked
  ) {
    gIsDefault = isDefault;
    gIsPinned = isPinned;
    gIconsUnlocked = unlocked;
    for (let notify of gUnlockListeners) {
      notify();
    }
  }
}

function watchUnlockState(emitChange) {
  if (!gUnlockListeners.size && DefaultBrowserHelper.canCheck) {
    gPollUnsubscribe =
      DefaultBrowserHelper.pollForDefaultChanges(refreshUnlockState);
  }
  gUnlockListeners.add(emitChange);
  refreshUnlockState();
  return () => {
    gUnlockListeners.delete(emitChange);
    if (!gUnlockListeners.size && gPollUnsubscribe) {
      gPollUnsubscribe();
      gPollUnsubscribe = null;
    }
  };
}

document.addEventListener("paneshown", event => {
  if (event.detail?.category === "paneBrowserIcon") {
    refreshUnlockState();
  }
});

function iconOption(id, l10nId, preview) {
  return {
    value: id,
    key: id,
    l10nId,
    controlAttrs: {
      class: "browser-icon-item",
      imagesrc: preview,
    },
  };
}

function getOptions(isGated) {
  let scheme = currentScheme();
  let options = [];
  for (let [id, entry] of Object.entries(lazy.ICON_CATALOG)) {
    if (!!entry.gated == isGated) {
      options.push(
        iconOption(id, entry.l10nId, lazy.resolvePreview(entry, scheme))
      );
    }
  }
  return options;
}

function resolveOptionPreviews(config) {
  let scheme = currentScheme();
  for (let option of config.options) {
    let entry = lazy.ICON_CATALOG[option.value];
    if (entry) {
      option.controlAttrs = {
        ...option.controlAttrs,
        imagesrc: lazy.resolvePreview(entry, scheme),
      };
    }
  }
  return config;
}

function isBonusId(id) {
  return !!lazy.ICON_CATALOG[id]?.gated;
}
function isBasicId(id) {
  let entry = lazy.ICON_CATALOG[id];
  return !!entry && !entry.gated;
}

async function selectIcon(val) {
  if (val === "default") {
    await lazy.CustomIconManager.revert();
  } else {
    await lazy.CustomIconManager.apply(val);
  }
}

Preferences.addAll([{ id: PREF_ICON_ID, type: "string" }]);

Preferences.addSetting({
  id: "customIconIdPref",
  pref: PREF_ICON_ID,
});

Preferences.addSetting({
  id: "customBrowserIconBasic",
  deps: ["customIconIdPref"],
  get(_, { customIconIdPref }) {
    let id = customIconIdPref.value || "default";
    return isBasicId(id) ? id : "";
  },
  set: selectIcon,
  setup: watchScheme,
  getControlConfig: resolveOptionPreviews,
});

Preferences.addSetting({
  id: "customBrowserIconBonus",
  deps: ["customIconIdPref"],
  get(_, { customIconIdPref }) {
    let id = customIconIdPref.value || "default";
    return isBonusId(id) ? id : "";
  },
  set: selectIcon,
  setup(emitChange) {
    let stopUnlock = watchUnlockState(emitChange);
    let stopScheme = watchScheme(emitChange);
    return () => {
      stopUnlock();
      stopScheme();
    };
  },
  getControlConfig(config) {
    resolveOptionPreviews(config);
    for (let option of config.options) {
      option.disabled = !gIconsUnlocked;
    }
    return config;
  },
});

Preferences.addSetting({
  id: "customBrowserIconRequirement",
  setup: watchUnlockState,
  visible: () => !gIconsUnlocked,
});

Preferences.addSetting({
  id: "browserIconSetDefaultButton",
  setup: watchUnlockState,
  visible: () => !gIsDefault,
  async onUserClick() {
    await DefaultBrowserHelper.setDefaultBrowser();
    refreshUnlockState();
  },
});
Preferences.addSetting({
  id: "browserIconPinButton",
  setup: watchUnlockState,
  visible: () => !gIsPinned,
  async onUserClick() {
    await window.getShellService().pinToTaskbar();
    refreshUnlockState();
  },
});

SettingGroupManager.registerGroups({
  browserIconBasic: {
    l10nId: "appearance-browser-icon-basic-group",
    headingLevel: 2,
    items: [
      {
        id: "customBrowserIconBasic",
        control: "moz-visual-picker",
        controlAttrs: { orientation: "vertical" },
        options: getOptions(false ),
      },
    ],
  },
  browserIconBonus: {
    l10nId: "appearance-browser-icon-bonus-group",
    headingLevel: 2,
    items: [
      {
        id: "customBrowserIconRequirement",
        l10nId: "appearance-browser-icon-requirement",
        control: "moz-promo",
        controlAttrs: {
          imagesrc: "chrome://global/skin/illustrations/kit-holding-lock.svg",
          imagewidth: "small",
          imagedisplay: "cover",
        },
        items: [
          {
            id: "browserIconSetDefaultButton",
            l10nId: "appearance-browser-icon-set-default-button",
            control: "moz-button",
            slot: "actions",
          },
          {
            id: "browserIconPinButton",
            l10nId: "appearance-browser-icon-pin-button",
            control: "moz-button",
            slot: "actions",
          },
        ],
      },
      {
        id: "customBrowserIconBonus",
        control: "moz-visual-picker",
        controlAttrs: { orientation: "vertical" },
        options: getOptions(true ),
      },
    ],
  },
});
