/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  CustomizableUI:
    "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs",
  IgnoreLists: "resource://gre/modules/IgnoreLists.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

const kPrefName = "browser.startup.homepage";
const kDefaultHomePage = "about:home";
const kHomePageIgnoreListId = "homepage-urls";
const kWidgetId = "home-button";
const kWidgetRemovedPref = "browser.engagement.home-button.has-removed";

function getHomepagePref(useDefault) {
  let prefs = useDefault ? Services.prefs.getDefaultBranch("") : Services.prefs;
  let homePage = prefs.getStringPref(kPrefName, "");

  if (!homePage && !useDefault) {
    Services.prefs.clearUserPref(kPrefName);
    homePage = getHomepagePref(true);
  }

  return homePage;
}

export let HomePage = {
  _ignoreList: [],

  _initializationPromise: null,

  async delayedStartup() {
    if (this._initializationPromise) {
      await this._initializationPromise;
      return;
    }

    this._ignoreListListener = this._handleIgnoreListUpdated.bind(this);

    this._initializationPromise = lazy.IgnoreLists.getAndSubscribe(
      this._ignoreListListener
    );

    this._addCustomizableUiListener();

    const current = await this._initializationPromise;

    await this._handleIgnoreListUpdated({ data: { current } });
  },

  get(aWindow) {
    let homePages = getHomepagePref();
    if (homePages == "about:blank") {
      homePages = "chrome://browser/content/blanktab.html";
    }

    return homePages;
  },

  getForErrorPage(win) {
    if (lazy.PrivateBrowsingUtils.isWindowPrivate(win)) {
      return win.BROWSER_NEW_TAB_URL;
    }
    let url = this.get(win);
    if (url.includes("|")) {
      url = url.split("|")[0];
    }
    return url;
  },

  parseCustomHomepageURLs(urls) {
    return urls
      .split("|")
      .map(url => url.trim())
      .filter(Boolean);
  },

  isPreferencesOrSettingsTab(tab) {
    return (
      tab.linkedBrowser.currentURI.spec.startsWith("about:preferences") ||
      tab.linkedBrowser.currentURI.spec.startsWith("about:settings")
    );
  },

  getTabsForCustomHomepage(
    win = Services.wm.getMostRecentWindow("navigator:browser")
  ) {
    let tabs = [];

    if (
      win &&
      win.document.documentElement.getAttribute("windowtype") ===
        "navigator:browser"
    ) {
      tabs = win.gBrowser.visibleTabs.slice(win.gBrowser.pinnedTabCount);
      tabs = tabs.filter(tab => !this.isPreferencesOrSettingsTab(tab));

      tabs = tabs.filter(tab => !tab.closing);
    }

    return tabs;
  },

  getDefault() {
    return getHomepagePref(true);
  },

  getOriginalDefault() {
    return kDefaultHomePage;
  },

  get overridden() {
    return Services.prefs.prefHasUserValue(kPrefName);
  },

  get locked() {
    return Services.prefs.prefIsLocked(kPrefName);
  },

  get isDefault() {
    return HomePage.get() === kDefaultHomePage;
  },

  async set(value) {
    await this.delayedStartup();

    if (await this.shouldIgnore(value)) {
      console.error(
        `Ignoring homepage setting for ${value} as it is on the ignore list.`
      );
      return false;
    }
    Services.prefs.setStringPref(kPrefName, value);
    this._maybeAddHomeButtonToToolbar(value);
    return true;
  },

  safeSet(value) {
    Services.prefs.setStringPref(kPrefName, value);
  },

  clear() {
    Services.prefs.clearUserPref(kPrefName);
  },

  reset() {
    Services.prefs.setStringPref(kPrefName, kDefaultHomePage);
  },

  async shouldIgnore(url) {
    await this.delayedStartup();

    const lowerURL = url.toLowerCase();
    return this._ignoreList.some(code => lowerURL.includes(code.toLowerCase()));
  },

  async _handleIgnoreListUpdated({ data: { current } }) {
    for (const entry of current) {
      if (entry.id == kHomePageIgnoreListId) {
        this._ignoreList = [...entry.matches];
      }
    }

    if (this.overridden) {
      let homePages = getHomepagePref().toLowerCase();
      if (
        this._ignoreList.some(code => homePages.includes(code.toLowerCase()))
      ) {
        this.clear();
      }
    }
  },

  onWidgetRemoved(widgetId) {
    if (widgetId == kWidgetId) {
      Services.prefs.setBoolPref(kWidgetRemovedPref, true);
      lazy.CustomizableUI.removeListener(this);
    }
  },

  _maybeAddHomeButtonToToolbar(homePage) {
    if (
      homePage !== "about:home" &&
      homePage !== "about:blank" &&
      !Services.prefs.getBoolPref(kWidgetRemovedPref, false) &&
      !lazy.CustomizableUI.getWidget(kWidgetId).areaType
    ) {
      let navbarPlacements = lazy.CustomizableUI.getWidgetIdsInArea("nav-bar");
      let position = navbarPlacements.indexOf("urlbar-container");
      for (let i = position - 1; i >= 0; i--) {
        if (
          !navbarPlacements[i].startsWith("customizableui-special-spring") &&
          !navbarPlacements[i].includes("spacer")
        ) {
          position = i + 1;
          break;
        }
      }
      lazy.CustomizableUI.addWidgetToArea(kWidgetId, "nav-bar", position);
    }
  },

  _addCustomizableUiListener() {
    if (!Services.prefs.getBoolPref(kWidgetRemovedPref, false)) {
      lazy.CustomizableUI.addListener(this);
    }
  },
};
