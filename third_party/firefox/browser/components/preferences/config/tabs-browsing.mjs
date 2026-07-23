/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import { Preferences } from "chrome://global/content/preferences/Preferences.mjs";
import { SettingGroupManager } from "chrome://browser/content/preferences/config/SettingGroupManager.mjs";

const XPCOMUtils = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
).XPCOMUtils;

const lazy = XPCOMUtils.declareLazy({
  AppConstants: "resource://gre/modules/AppConstants.sys.mjs",
  ContextualIdentityService:
    "moz-src:///toolkit/components/contextualidentity/ContextualIdentityService.sys.mjs",
  ShortcutUtils: "resource://gre/modules/ShortcutUtils.sys.mjs",
  TransientPrefs: "resource:///modules/TransientPrefs.sys.mjs",
});

Preferences.addAll([

  { id: "browser.link.open_newwindow", type: "int" },
  { id: "browser.link.open_newwindow.override.external", type: "int" },
  { id: "browser.tabs.loadInBackground", type: "bool", inverted: true },
  { id: "browser.tabs.warnOnClose", type: "bool" },
  { id: "browser.warnOnQuitShortcut", type: "bool" },
  { id: "browser.tabs.warnOnOpen", type: "bool" },
  { id: "browser.ctrlTab.sortByRecentlyUsed", type: "bool" },
  { id: "browser.tabs.hoverPreview.enabled", type: "bool" },
  { id: "browser.tabs.hoverPreview.showThumbnails", type: "bool" },
  { id: "browser.tabs.dragDrop.createGroup.enabled", type: "bool" },
  { id: "browser.tabs.groups.enabled", type: "bool" },
  { id: "privacy.userContext.ui.enabled", type: "bool" },

  { id: "privacy.userContext.enabled", type: "bool" },

  {
    id: "browser.preferences.defaultPerformanceSettings.enabled",
    type: "bool",
  },
  { id: "dom.ipc.processCount", type: "int" },
  { id: "dom.ipc.processCount.web", type: "int" },
  { id: "layers.acceleration.disabled", type: "bool", inverted: true },

  {
    id: "browser.newtabpage.activity-stream.asrouter.userprefs.cfr.addons",
    type: "bool",
  },
  {
    id: "browser.newtabpage.activity-stream.asrouter.userprefs.cfr.features",
    type: "bool",
  },

  { id: "sidebar.verticalTabs", type: "bool" },
  { id: "sidebar.revamp", type: "bool" },
]);

if (lazy.AppConstants.platform === "win") {
  Preferences.addAll([{ id: "browser.taskbar.previews.enable", type: "bool" }]);
}

let srdEnabled = Services.prefs.getBoolPref(
  "browser.settings-redesign.enabled",
  false
);

if (srdEnabled) {
  Preferences.addAll([
    { id: "accessibility.browsewithcaret", type: "bool" },
    { id: "accessibility.typeaheadfind", type: "bool" },
  ]);
}


Preferences.addSetting({
  id: "tabsOpening",
});
Preferences.addSetting({
  id: "linkTargeting",
  pref: "browser.link.open_newwindow",
  get: prefVal => {
    return prefVal != 2;
  },
  set: checked => {
    return checked ? 3 : 2;
  },
});
Preferences.addSetting({
  id: "switchToNewTabs",
  pref: "browser.tabs.loadInBackground",
});
Preferences.addSetting({
  id: "openAppLinksNextToActiveTab",
  pref: "browser.link.open_newwindow.override.external",
  get: prefVal => {
    return prefVal == Ci.nsIBrowserDOMWindow.OPEN_NEWTAB_AFTER_CURRENT;
  },
  set: (checked, _, setting) => {
    return checked
      ? Ci.nsIBrowserDOMWindow.OPEN_NEWTAB_AFTER_CURRENT
      : setting.pref.defaultValue;
  },
  onUserChange: checked => {
  },
});
Preferences.addSetting({
  id: "warnOpenMany",
  pref: "browser.tabs.warnOnOpen",
  visible: () =>
    lazy.TransientPrefs.prefShouldBeVisible("browser.tabs.warnOnOpen"),
});

Preferences.addSetting({
  id: "tabsInteraction",
});
Preferences.addSetting({
  id: "ctrlTabRecentlyUsedOrder",
  pref: "browser.ctrlTab.sortByRecentlyUsed",
  onUserClick: () => {
    Services.prefs.clearUserPref("browser.ctrlTab.migrated");
  },
});
Preferences.addSetting({
  id: "tabHoverPreview",
  pref: "browser.tabs.hoverPreview.enabled",
});
Preferences.addSetting({
  id: "tabPreviewShowThumbnails",
  pref: "browser.tabs.hoverPreview.showThumbnails",
  deps: ["tabHoverPreview"],
  visible: ({ tabHoverPreview }) => !!tabHoverPreview.value,
});
Preferences.addSetting({
  id: "tabGroups",
  pref: "browser.tabs.groups.enabled",
});
Preferences.addSetting({
  id: "tabGroupDragToCreate",
  pref: "browser.tabs.dragDrop.createGroup.enabled",
});
if (lazy.AppConstants.platform === "win") {
  Preferences.addSetting({
    id: "showTabsInTaskbar",
    pref: "browser.taskbar.previews.enable",
    visible: () => {
      try {
        let ver = parseFloat(Services.sysinfo.getProperty("version"));
        return ver >= 6.1;
      } catch (ex) {
        return false;
      }
    },
  });
} else {
  Preferences.addSetting({ id: "showTabsInTaskbar", visible: () => false });
}

Preferences.addSetting({
  id: "privacyUserContextUI",
  pref: "privacy.userContext.ui.enabled",
});
Preferences.addSetting({
  id: "browserContainersbox",
  deps: ["privacyUserContextUI"],
  visible: ({ privacyUserContextUI }) => !!privacyUserContextUI.value,
});
Preferences.addSetting({
  id: "browserContainersCheckbox",
  pref: "privacy.userContext.enabled",
  async promptToCloseTabsAndDisable(count, setting) {
    let [title, message, okButton, cancelButton] =
      await document.l10n.formatValues([
        { id: "containers-disable-alert-title" },
        { id: "containers-disable-alert-desc", args: { tabCount: count } },
        { id: "containers-disable-alert-ok-button", args: { tabCount: count } },
        { id: "containers-disable-alert-cancel-button" },
      ]);

    let buttonFlags =
      Ci.nsIPrompt.BUTTON_TITLE_IS_STRING * Ci.nsIPrompt.BUTTON_POS_0 +
      Ci.nsIPrompt.BUTTON_TITLE_IS_STRING * Ci.nsIPrompt.BUTTON_POS_1;

    let rv = Services.prompt.confirmEx(
      window,
      title,
      message,
      buttonFlags,
      okButton,
      cancelButton,
      null,
      null,
      {}
    );

    if (rv == 0) {
      await lazy.ContextualIdentityService.closeContainerTabs();
      setting.pref.value = false;
    }

    return true;
  },
  set(val, _, setting) {
    if (val) {
      return val;
    }

    let count = lazy.ContextualIdentityService.countContainerTabs();
    if (count == 0) {
      return false;
    }

    return this.promptToCloseTabsAndDisable(count, setting);
  },
});
Preferences.addSetting({
  id: "browserContainersSettings",
  deps: ["browserContainersCheckbox"],
  onUserClick: () => {
    window.gotoPref("containers");
  },
  disabled: ({ browserContainersCheckbox }) => !browserContainersCheckbox.value,
});

Preferences.addSetting({
  id: "tabsClosing",
});
Preferences.addSetting({
  id: "warnCloseMultiple",
  pref: "browser.tabs.warnOnClose",
});
Preferences.addSetting({
  id: "warnOnQuitKey",
  pref: "browser.warnOnQuitShortcut",
  setup() {
    let quitKeyElement =
      window.browsingContext.topChromeWindow.document.getElementById(
        "key_quitApplication"
      );
    if (quitKeyElement) {
      this.quitKey = lazy.ShortcutUtils.prettifyShortcut(quitKeyElement);
    }
  },
  visible() {
    return lazy.AppConstants.platform !== "win" && this.quitKey;
  },
  getControlConfig(config) {
    return {
      ...config,
      l10nArgs: { quitKey: this.quitKey ?? "" },
    };
  },
});

Preferences.addSetting({
  id: "useCursorNavigation",
  pref: "accessibility.browsewithcaret",
});

Preferences.addSetting({
  id: "searchStartTyping",
  pref: "accessibility.typeaheadfind",
});

Preferences.addSetting({
  id: "keyboardCustomkeysLinkTabs",
});

Preferences.addSetting({
  id: "contentProcessCount",
  pref: "dom.ipc.processCount",
});
Preferences.addSetting({
  id: "allowHWAccel",
  pref: "layers.acceleration.disabled",
  deps: ["useRecommendedPerformanceSettings"],
  visible({ useRecommendedPerformanceSettings }) {
    return !useRecommendedPerformanceSettings.value;
  },
});
Preferences.addSetting({
  id: "useRecommendedPerformanceSettings",
  pref: "browser.preferences.defaultPerformanceSettings.enabled",
  deps: ["contentProcessCount", "allowHWAccel"],
  get(val, { allowHWAccel, contentProcessCount }) {
    if (
      allowHWAccel.value != allowHWAccel.pref.defaultValue ||
      contentProcessCount.value != contentProcessCount.pref.defaultValue
    ) {
      return false;
    }
    return val;
  },
  set(val, { allowHWAccel, contentProcessCount }) {
    if (val) {
      contentProcessCount.value = contentProcessCount.pref.defaultValue;
      allowHWAccel.value = allowHWAccel.pref.defaultValue;
    }
    return val;
  },
});

Preferences.addSetting({
  id: "cfrRecommendations",
  pref: "browser.newtabpage.activity-stream.asrouter.userprefs.cfr.addons",
});
Preferences.addSetting({
  id: "cfrRecommendations-features",
  pref: "browser.newtabpage.activity-stream.asrouter.userprefs.cfr.features",
});

Preferences.addSetting({
  id: "browserLayoutRadioGroup",
  pref: "sidebar.verticalTabs",
  get: prefValue => (prefValue ? "true" : "false"),
  set: value => value === "true",
});

Preferences.addSetting({
  id: "browserLayoutShowSidebar",
  pref: "sidebar.revamp",
  onUserChange(checked) {
    if (checked) {
      window.browsingContext.topChromeWindow.SidebarController?.enabledViaSettings(
        true
      );
    }
  },
});

SettingGroupManager.registerGroups({
  browserLayout: {
    subcategory: "layout",
    l10nId: "browser-layout-header2",
    iconSrc: "chrome://browser/skin/sidebar-expanded.svg",
    headingLevel: 2,
    items: [
      {
        id: "browserLayoutRadioGroup",
        control: "moz-visual-picker",
        options: [
          {
            id: "browserLayoutHorizontalTabs",
            value: "false",
            l10nId: "browser-layout-horizontal-tabs2",
            controlAttrs: {
              class: "setting-chooser-item",
              imagesrc:
                "chrome://browser/content/preferences/browser-layout-horizontal.svg",
            },
          },
          {
            id: "browserLayoutVerticalTabs",
            value: "true",
            l10nId: "browser-layout-vertical-tabs2",
            controlAttrs: {
              class: "setting-chooser-item",
              imagesrc:
                "chrome://browser/content/preferences/browser-layout-vertical.svg",
            },
          },
        ],
      },
      {
        id: "browserLayoutShowSidebar",
        l10nId: "browser-layout-show-sidebar2",
      },
    ],
  },
  tabs: {
    l10nId: "tabs-group-header2",
    headingLevel: 2,
    iconSrc: "chrome://browser/skin/tabs.svg",
    items: [
      {
        id: "tabsOpening",
        control: "moz-fieldset",
        l10nId: "tabs-opening-heading",
        headingLevel: 3,
        items: [
          {
            id: "linkTargeting",
            l10nId: "open-new-link-as-tabs",
          },
          {
            id: "switchToNewTabs",
            l10nId: "switch-to-new-tabs-2",
          },
          {
            id: "openAppLinksNextToActiveTab",
            l10nId: "open-external-link-next-to-active-tab",
          },
          {
            id: "warnOpenMany",
            l10nId: "warn-on-open-many-tabs",
          },
        ],
      },
      {
        id: "tabsInteraction",
        control: "moz-fieldset",
        l10nId: "tabs-interaction-heading",
        headingLevel: 3,
        items: [
          {
            id: "ctrlTabRecentlyUsedOrder",
            l10nId: "ctrl-tab-recently-used-order",
          },
          {
            id: "tabPreviewShowThumbnails",
            l10nId: "settings-tabs-show-image-in-preview",
          },
          {
            id: "tabGroupDragToCreate",
            l10nId: "settings-tabs-drag-to-create-tab-groups",
          },
          {
            id: "showTabsInTaskbar",
            l10nId: "show-tabs-in-taskbar",
          },
        ],
      },
      {
        id: "browserContainersbox",
        control: "moz-fieldset",
        l10nId: "tabs-containers-heading",
        headingLevel: 3,
        items: [
          {
            id: "browserContainersCheckbox",
            l10nId: "browser-containers-enabled-2",
            supportPage: "containers",
          },
          {
            id: "browserContainersSettings",
            loadPane: "containers",
            l10nId: "browser-containers-settings-2",
            control: "moz-box-button",
          },
        ],
      },
      {
        id: "tabsClosing",
        control: "moz-fieldset",
        l10nId: "tabs-closing-heading",
        headingLevel: 3,
        items: [
          {
            id: "warnCloseMultiple",
            l10nId: "ask-on-close-multiple-tabs",
          },
          {
            id: "warnOnQuitKey",
            l10nId: "ask-on-quit-with-key",
          },
        ],
      },
    ],
  },
  pageNavigation: {
    l10nId: "page-navigation-group",
    headingLevel: 2,
    iconSrc: "chrome://global/skin/icons/cursor-arrow.svg",
    items: [
      { id: "useCursorNavigation", l10nId: "browsing-use-cursor-navigation" },
      { id: "searchStartTyping", l10nId: "browsing-search-on-start-typing" },
    ],
  },
  keyboardShortcuts: {
    l10nId: "settings-keyboard-shortcuts-group",
    headingLevel: 2,
    iconSrc: "chrome://browser/skin/preferences/category-accessibility.svg",
    items: [
      {
        id: "keyboardCustomkeysLinkTabs",
        l10nId: "settings-keyboard-shortcuts-customkeys-link",
        control: "moz-box-link",
        controlAttrs: {
          href: "about:keyboard",
        },
      },
    ],
  },
  performance: {
    l10nId: "performance-group",
    headingLevel: 2,
    iconSrc: "chrome://global/skin/icons/chevron.svg",
    items: [
      {
        id: "useRecommendedPerformanceSettings",
        l10nId: "performance-use-recommended-settings-checkbox-2",
        supportPage: "performance",
      },
      {
        id: "allowHWAccel",
        l10nId: "performance-allow-hw-accel",
      },
    ],
  },
  recommendations: {
    l10nId: "recommendations-group",
    headingLevel: 2,
    iconSrc: "chrome://browser/skin/trending.svg",
    items: [
      {
        id: "cfrRecommendations",
        l10nId: "browsing-cfr-recommendations",
        supportPage: "extensionrecommendations",
        subcategory: "cfraddons",
      },
      {
        id: "cfrRecommendations-features",
        l10nId: "browsing-cfr-features",
        supportPage: "extensionrecommendations",
        subcategory: "cfrfeatures",
      },
    ],
  },
  browsing: {
    l10nId: "browsing-group",
    headingLevel: 1,
    hidden: srdEnabled,
    items: [
      {
        id: "cfrRecommendations",
        l10nId: "browsing-cfr-recommendations",
        supportPage: "extensionrecommendations",
        subcategory: "cfraddons",
      },
      {
        id: "cfrRecommendations-features",
        l10nId: "browsing-cfr-features",
        supportPage: "extensionrecommendations",
        subcategory: "cfrfeatures",
      },
    ],
  },
});
