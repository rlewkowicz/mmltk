/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


const { BLANK_HOMEPAGE_URL } = ChromeUtils.importESModule(
  "chrome://browser/content/preferences/config/home-startup.mjs",
  { global: "current" }
);

ChromeUtils.defineESModuleGetters(this, {
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  HomePage: "resource:///modules/HomePage.sys.mjs",
});

const RESET_DEFAULTS_BUTTON_ENABLED = false;

var gHomePane = {
  HOME_MODE_FIREFOX_HOME: "0",
  HOME_MODE_BLANK: "1",
  HOME_MODE_CUSTOM: "2",
  HOMEPAGE_PREF: "browser.startup.homepage",
  NEWTAB_ENABLED_PREF: "browser.newtabpage.enabled",
  ACTIVITY_STREAM_PREF_BRANCH: "browser.newtabpage.activity-stream.",

  get homePanePrefs() {
    return Preferences.getAll().filter(pref =>
      pref.id.includes(this.ACTIVITY_STREAM_PREF_BRANCH)
    );
  },

  get isResetDefaultsButtonEnabled() {
    return RESET_DEFAULTS_BUTTON_ENABLED;
  },

  async syncToNewTabPref() {
    let menulist = document.getElementById("newTabMode");

    if (["0", "1"].includes(menulist.value)) {
      let newtabEnabledPref = Services.prefs.getBoolPref(
        this.NEWTAB_ENABLED_PREF,
        true
      );
      let newValue = menulist.value !== this.HOME_MODE_BLANK;
      if (newtabEnabledPref !== newValue) {
        Services.prefs.setBoolPref(this.NEWTAB_ENABLED_PREF, newValue);
      }
    }
  },

  async syncFromNewTabPref() {
    let menulist = document.getElementById("newTabMode");

    if (
      AboutNewTab.newTabURL === "about:newtab" ||
      AboutNewTab.newTabURL === "about:blank" ||
      AboutNewTab.newTabURL === BLANK_HOMEPAGE_URL
    ) {
      let newtabEnabledPref = Services.prefs.getBoolPref(
        this.NEWTAB_ENABLED_PREF,
        true
      );
      let newValue = newtabEnabledPref
        ? this.HOME_MODE_FIREFOX_HOME
        : this.HOME_MODE_BLANK;
      if (newValue !== menulist.value) {
        menulist.value = newValue;
      }
      menulist.disabled = Preferences.get(this.NEWTAB_ENABLED_PREF).locked;
    } else {
      menulist.value = this.HOME_MODE_FIREFOX_HOME;
    }
  },

  async _updateMenuInterface(selectId) {
    let selects;
    if (selectId) {
      selects = [document.getElementById(selectId)];
    } else {
      let newTabSelect = document.getElementById("newTabMode");
      let homeSelect = document.getElementById("homeMode");
      selects = [homeSelect, newTabSelect];
    }

    for (let select of selects) {
      let menuOptions = Array.from(select.menupopup.childNodes);

      for (let option of menuOptions) {
        if (!/^\d+$/.test(option.value)) {
          option.remove();
        }
      }
    }
  },

  watchNewTab() {
    let newTabObserver = () => {
      this.syncFromNewTabPref();
      this._updateMenuInterface("newTabMode");
    };
    Services.obs.addObserver(newTabObserver, "newtab-url-changed");
    window.addEventListener("unload", () => {
      Services.obs.removeObserver(newTabObserver, "newtab-url-changed");
    });
  },

  watchHomePrefChange() {
    const homePrefObserver = (subject, topic, data) => {
      if (data && data != this.HOMEPAGE_PREF) {
        return;
      }
      this._updateUseCurrentButton();
      this._renderCustomSettings();
      this._handleHomePageOverrides();
      this._updateMenuInterface("homeMode");
    };

    Services.prefs.addObserver(this.HOMEPAGE_PREF, homePrefObserver);
    window.addEventListener("unload", () => {
      Services.prefs.removeObserver(this.HOMEPAGE_PREF, homePrefObserver);
    });
  },

  watchHomeTabPrefChange() {
    const observer = () => this.toggleRestoreDefaultsBtn();
    Services.prefs.addObserver(this.ACTIVITY_STREAM_PREF_BRANCH, observer);
    Services.prefs.addObserver(this.HOMEPAGE_PREF, observer);
    Services.prefs.addObserver(this.NEWTAB_ENABLED_PREF, observer);

    window.addEventListener("unload", () => {
      Services.prefs.removeObserver(this.ACTIVITY_STREAM_PREF_BRANCH, observer);
      Services.prefs.removeObserver(this.HOMEPAGE_PREF, observer);
      Services.prefs.removeObserver(this.NEWTAB_ENABLED_PREF, observer);
    });
  },

  _renderCustomSettings(options = {}) {
    let { shouldShow, isControlled } = options;
    const customSettingsContainerEl = document.getElementById("customSettings");
    const customUrlEl = document.getElementById("homePageUrl");
    const homePage = HomePage.get();
    const isHomePageCustom =
      (!this._isHomePageDefaultValue() &&
        !this.isHomePageBlank() &&
        !isControlled) ||
      homePage.locked;

    if (typeof shouldShow === "undefined") {
      shouldShow = isHomePageCustom;
    }
    customSettingsContainerEl.hidden = !shouldShow;

    let newValue;
    if (
      this._isBlankPage(homePage) ||
      (HomePage.isDefault && !HomePage.locked)
    ) {
      newValue = "";
    } else {
      newValue = homePage;
    }
    if (customUrlEl.value !== newValue) {
      customUrlEl.value = newValue;
    }
  },

  _isHomePageDefaultValue() {
    const startupPref = Preferences.get("browser.startup.page");
    return (
      startupPref.value !== gMainPane.STARTUP_PREF_BLANK && HomePage.isDefault
    );
  },

  isHomePageBlank() {
    const startupPref = Preferences.get("browser.startup.page");
    return (
      ["about:blank", BLANK_HOMEPAGE_URL, ""].includes(HomePage.get()) ||
      startupPref.value === gMainPane.STARTUP_PREF_BLANK
    );
  },

  _isTabAboutPreferencesOrSettings(aTab) {
    return (
      aTab.linkedBrowser.currentURI.spec.startsWith("about:preferences") ||
      aTab.linkedBrowser.currentURI.spec.startsWith("about:settings")
    );
  },

  _getTabsForHomePage() {
    let tabs = [];
    let win = Services.wm.getMostRecentWindow("navigator:browser");

    if (
      win &&
      win.document.documentElement.getAttribute("windowtype") ===
        "navigator:browser"
    ) {
      tabs = win.gBrowser.visibleTabs.slice(win.gBrowser.pinnedTabCount);
      tabs = tabs.filter(tab => !this._isTabAboutPreferencesOrSettings(tab));
      tabs = tabs.filter(tab => !tab.closing);
    }

    return tabs;
  },

  _renderHomepageMode() {
    const isDefault = this._isHomePageDefaultValue();
    const isBlank = this.isHomePageBlank();
    const el = document.getElementById("homeMode");
    let newValue;

    if (isDefault) {
      newValue = this.HOME_MODE_FIREFOX_HOME;
    } else if (isBlank) {
      newValue = this.HOME_MODE_BLANK;
    } else {
      newValue = this.HOME_MODE_CUSTOM;
    }
    if (el.value !== newValue) {
      el.value = newValue;
    }
  },

  _setInputDisabledStates() {
    let tabCount = this._getTabsForHomePage().length;

    document
      .querySelectorAll(".check-home-page-locked")
      .forEach(element => {
        let isDisabled;
        let pref =
          element.getAttribute("preference") ||
          element.getAttribute("data-preference-related");
        if (!pref) {
          throw new Error(
            `Element with id ${element.id} did not have preference or data-preference-related attribute defined.`
          );
        }

        if (pref === this.HOMEPAGE_PREF) {
          isDisabled = HomePage.locked;
        } else {
          isDisabled = Preferences.get(pref).locked;
        }

        if (pref === "pref.browser.disable_button.current_page") {
          isDisabled = isDisabled || tabCount < 1;
        }

        element.disabled = isDisabled;
      });
  },

  async _handleHomePageOverrides() {
    if (HomePage.locked) {
      this._renderCustomSettings();
      this._setInputDisabledStates();
    } else {
      this._setInputDisabledStates();
      this._renderCustomSettings();
    }
    this._renderHomepageMode();
  },

  onMenuChange(event) {
    const { value } = event.target;
    const startupPref = Preferences.get("browser.startup.page");
    switch (value) {
      case this.HOME_MODE_FIREFOX_HOME:
        if (startupPref.value === gMainPane.STARTUP_PREF_BLANK) {
          startupPref.value = gMainPane.STARTUP_PREF_HOMEPAGE;
        }
        if (!HomePage.isDefault) {
          HomePage.reset();
        } else {
          this._renderCustomSettings({ shouldShow: false });
        }
        break;
      case this.HOME_MODE_BLANK:
        if (!this._isBlankPage(HomePage.get())) {
          HomePage.safeSet(BLANK_HOMEPAGE_URL);
        } else {
          this._renderCustomSettings({ shouldShow: false });
        }
        break;
      case this.HOME_MODE_CUSTOM:
        if (startupPref.value === gMainPane.STARTUP_PREF_BLANK) {
          Services.prefs.clearUserPref(startupPref.id);
        }
        if (HomePage.getDefault() != HomePage.getOriginalDefault()) {
          HomePage.clear();
        }
        this._renderCustomSettings({ shouldShow: true });
        break;
      default:
        this._renderCustomSettings({ shouldShow: false });
    }
  },

  async _updateUseCurrentButton() {
    let useCurrent = document.getElementById("useCurrentBtn");
    let tabs = this._getTabsForHomePage();
    const tabCount = tabs.length;
    document.l10n.setAttributes(useCurrent, "use-current-pages", { tabCount });

    let prefName = "pref.browser.homepage.disable_button.current_page";
    if (Preferences.get(prefName).locked) {
      return;
    }

    useCurrent.disabled = tabCount < 1;
  },

  setHomePageToCurrent() {
    let tabs = this._getTabsForHomePage();
    function getTabURI(t) {
      return t.linkedBrowser.currentURI.spec;
    }

    if (tabs.length) {
      HomePage.set(tabs.map(getTabURI).join("|")).catch(console.error);
    }
  },

  _setHomePageToBookmarkClosed(rv, aEvent) {
    if (aEvent.detail.button != "accept") {
      return;
    }
    if (rv.urls && rv.names) {
      HomePage.set(rv.urls.join("|")).catch(console.error);
    }
  },

  setHomePageToBookmark() {
    const rv = { urls: null, names: null };
    gSubDialog.open(
      "chrome://browser/content/preferences/dialogs/selectBookmark.xhtml",
      {
        features: "resizable=yes, modal=yes",
        closingCallback: this._setHomePageToBookmarkClosed.bind(this, rv),
      },
      rv
    );
  },

  restoreDefaultHomePage() {
    HomePage.reset();
    this._handleHomePageOverrides();
    Services.prefs.clearUserPref(this.NEWTAB_ENABLED_PREF);
    AboutNewTab.resetNewTabURL();
  },

  onCustomHomePageChange(event) {
    const value = event.target.value || HomePage.getDefault();
    HomePage.set(value).catch(console.error);
  },

  _changedHomeTabDefaultPrefs() {
    const homeContentChanged =
      this.isResetDefaultsButtonEnabled &&
      this.homePanePrefs.some(pref => pref.hasUserValue);
    const newtabPref = Preferences.get(this.NEWTAB_ENABLED_PREF);
    return (
      homeContentChanged ||
      HomePage.overridden ||
      newtabPref.hasUserValue ||
      AboutNewTab.newTabURLOverridden
    );
  },

  _isBlankPage(url) {
    return url == "about:blank" || url == BLANK_HOMEPAGE_URL;
  },

  toggleRestoreDefaultsBtn() {
    const btn = document.getElementById("restoreDefaultHomePageBtn");
    const prefChanged = this._changedHomeTabDefaultPrefs();
    if (prefChanged) {
      btn.style.removeProperty("visibility");
    } else {
      btn.style.visibility = "hidden";
    }
  },

  restoreDefaultPrefsForHome() {
    this.restoreDefaultHomePage();
    if (this.isResetDefaultsButtonEnabled) {
      this.homePanePrefs.forEach(pref => Services.prefs.clearUserPref(pref.id));
    }
  },

  init() {
    if (Services.prefs.getBoolPref("browser.settings-redesign.enabled")) {
      return;
    }
    document
      .getElementById("homePageUrl")
      .addEventListener("change", this.onCustomHomePageChange.bind(this));
    document
      .getElementById("useCurrentBtn")
      .addEventListener("command", this.setHomePageToCurrent.bind(this));
    document
      .getElementById("useBookmarkBtn")
      .addEventListener("command", this.setHomePageToBookmark.bind(this));
    document
      .getElementById("restoreDefaultHomePageBtn")
      .addEventListener("command", this.restoreDefaultPrefsForHome.bind(this));

    this._updateMenuInterface();
    document
      .getElementById("newTabMode")
      .addEventListener("command", this.syncToNewTabPref.bind(this));
    document
      .getElementById("homeMode")
      .addEventListener("command", this.onMenuChange.bind(this));

    this._updateUseCurrentButton();
    this._handleHomePageOverrides();
    this.syncFromNewTabPref();
    window.addEventListener("focus", this._updateUseCurrentButton.bind(this));

    this.watchNewTab();
    this.watchHomePrefChange();
    this.watchHomeTabPrefChange();
    Services.obs.notifyObservers(window, "home-pane-loaded");
  },
};
