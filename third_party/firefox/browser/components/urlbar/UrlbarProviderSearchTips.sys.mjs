/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

import {
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AppMenuNotifications: "resource://gre/modules/AppMenuNotifications.sys.mjs",
  DefaultBrowserCheck:
    "moz-src:///browser/components/DefaultBrowserCheck.sys.mjs",
  LaterRun: "resource:///modules/LaterRun.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  SearchStaticData:
    "moz-src:///toolkit/components/search/SearchStaticData.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarProviderTopSites:
    "moz-src:///browser/components/urlbar/UrlbarProviderTopSites.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  setTimeout: "resource://gre/modules/Timer.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "cfrFeaturesUserPref",
  "browser.newtabpage.activity-stream.asrouter.userprefs.cfr.features",
  true
);

const TIPS = {
  NONE: "",
  ONBOARD: "searchTip_onboard",
  REDIRECT: "searchTip_redirect",
};

ChromeUtils.defineLazyGetter(lazy, "SUPPORTED_ENGINES", () => {
  const googleTLDs = lazy.SearchStaticData.getAlternateDomains("www.google.com")
    .map(str => str.slice("www.google.".length).replaceAll(".", "\\."))
    .join("|");

  return new Map([
    ["Bing", { domainPath: /^www\.bing\.com\/$/ }],
    [
      "DuckDuckGo",
      {
        domainPath: /^(start\.)?duckduckgo\.com\/$/,
        prohibitedSearchParams: ["q"],
      },
    ],
    [
      "Google",
      {
        domainPath: new RegExp(`^www\.google\.(?:${googleTLDs})\/(webhp)?$`),
      },
    ],
  ]);
});

const MAX_SHOWN_COUNT = 4;

const SHOW_TIP_DELAY_MS = 200;

const LAST_UPDATE_THRESHOLD_HOURS = 24;

export class UrlbarProviderSearchTips extends UrlbarProvider {
  static #instance = null;

  constructor() {
    super();
    if (UrlbarProviderSearchTips.#instance) {
      throw new Error("Can only have one instance of UrlbarProviderSearchTips");
    }
    UrlbarProviderSearchTips.#instance = this;

    this.disableTipsForCurrentSession = true;
    for (let tip of Object.values(TIPS)) {
      if (
        tip &&
        lazy.UrlbarPrefs.get(`tipShownCount.${tip}`) < MAX_SHOWN_COUNT
      ) {
        this.disableTipsForCurrentSession = false;
        break;
      }
    }

    this.showedTipTypeInCurrentEngagement = TIPS.NONE;

    this._seenWindows = new WeakSet();
  }

  static get TIP_TYPE() {
    return TIPS;
  }

  static get PRIORITY() {
    return AppConstants.MOZ_PLACES
      ? lazy.UrlbarProviderTopSites.PRIORITY + 1
      : 2;
  }

  get type() {
    return UrlbarUtils.PROVIDER_TYPE.PROFILE;
  }

  async isActive() {
    return !!this.currentTip && lazy.cfrFeaturesUserPref;
  }

  getPriority() {
    return UrlbarProviderSearchTips.PRIORITY;
  }

  async startQuery(queryContext, addCallback) {
    let instance = this.queryInstance;

    let tip = this.currentTip;
    this.showedTipTypeInCurrentEngagement = this.currentTip;
    this.currentTip = TIPS.NONE;

    let defaultEngine = await lazy.SearchService.getDefault();
    let icon = await defaultEngine.getIconURL();
    if (instance != this.queryInstance) {
      return;
    }

    let result;
    switch (tip) {
      case TIPS.ONBOARD:
        result = this.#makeResult({
          tip,
          icon,
          titleL10n: {
            id: "urlbar-search-tips-onboard",
            args: {
              engineName: defaultEngine.name,
            },
          },
          heuristic: true,
        });
        break;
      case TIPS.REDIRECT:
        result = this.#makeResult({
          tip,
          icon,
          titleL10n: {
            id: "urlbar-search-tips-redirect-2",
            args: {
              engineName: defaultEngine.name,
            },
          },
        });
        break;
    }
    addCallback(this, result);
  }

  #pickResult(result, window) {
    window.gURLBar.value = "";
    window.gURLBar.setPageProxyState("invalid");
    window.gURLBar.removeAttribute("suppress-focus-border");
    window.gURLBar.focus();

    lazy.UrlbarPrefs.set(
      `tipShownCount.${result.payload.type}`,
      MAX_SHOWN_COUNT
    );
  }

  onEngagement(queryContext, controller, details) {
    this.#pickResult(details.result, controller.browserWindow);
  }

  onSearchSessionEnd() {
    this.showedTipTypeInCurrentEngagement = TIPS.NONE;
  }

  static async onLocationChange(window, uri, webProgress, flags) {
    if (UrlbarProviderSearchTips.#instance) {
      UrlbarProviderSearchTips.#instance.onLocationChange(
        window,
        uri,
        webProgress,
        flags
      );
    }
  }

  async onLocationChange(window, uri, webProgress, flags) {
    let instance = (this._onLocationChangeInstance = {});

    if (!this._seenWindows.has(window)) {
      this._seenWindows.add(window);

      await window.gBrowserInit.firstContentWindowPaintPromise;
      if (instance != this._onLocationChangeInstance) {
        return;
      }

      await new Promise(resolve => {
        let timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
        timer.initWithCallback(resolve, 500, Ci.nsITimer.TYPE_ONE_SHOT);
      });
      if (instance != this._onLocationChangeInstance) {
        return;
      }
    }

    if (
      flags & Ci.nsIWebProgressListener.LOCATION_CHANGE_SAME_DOCUMENT ||
      !webProgress.isTopLevel
    ) {
      return;
    }

    if (this.showedTipTypeInCurrentEngagement != TIPS.NONE) {
      window.gURLBar.view.close();
    }

    if (
      !lazy.cfrFeaturesUserPref ||
      (this.disableTipsForCurrentSession &&
        !lazy.UrlbarPrefs.get("searchTips.test.ignoreShowLimits"))
    ) {
      return;
    }

    this._maybeShowTipForUrl(uri.spec, window).catch(ex =>
      this.logger.error(ex)
    );
  }

  async _maybeShowTipForUrl(urlStr, window) {
    let instance = {};
    this._maybeShowTipForUrlInstance = instance;

    let ignoreShowLimits = lazy.UrlbarPrefs.get(
      "searchTips.test.ignoreShowLimits"
    );

    let tip;
    let isNewtab = ["about:newtab", "about:home"].includes(urlStr);
    let isSearchHomepage = !isNewtab && (await isDefaultEngineHomepage(urlStr));

    if (isNewtab) {
      tip = TIPS.ONBOARD;
    } else if (isSearchHomepage) {
      tip = TIPS.REDIRECT;
    } else {
      return;
    }

    let shownCount = lazy.UrlbarPrefs.get(`tipShownCount.${tip}`);
    if (shownCount >= MAX_SHOWN_COUNT && !ignoreShowLimits) {
      return;
    }

    let hoursSinceUpdate = Math.min(
      lazy.LaterRun.hoursSinceInstall,
      lazy.LaterRun.hoursSinceUpdate
    );
    if (hoursSinceUpdate < LAST_UPDATE_THRESHOLD_HOURS && !ignoreShowLimits) {
      return;
    }

    lazy.setTimeout(async () => {
      if (this._maybeShowTipForUrlInstance != instance) {
        return;
      }

      if (
        window.gURLBar.getAttribute("pageproxystate") == "invalid" &&
        window.gURLBar.value != ""
      ) {
        return;
      }

      if (
        (!ignoreShowLimits && (await isBrowserShowingNotification(window))) ||
        this._maybeShowTipForUrlInstance != instance
      ) {
        return;
      }

      this.disableTipsForCurrentSession = true;

      lazy.UrlbarPrefs.set(`tipShownCount.${tip}`, shownCount + 1);

      this.currentTip = tip;

      window.gURLBar.search("", { focus: tip == TIPS.ONBOARD });
    }, SHOW_TIP_DELAY_MS);
  }

  #makeResult({ tip, icon, titleL10n, heuristic = false }) {
    return new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.TIP,
      source: lazy.UrlbarShared.RESULT_SOURCE.OTHER_LOCAL,
      heuristic,
      payload: {
        type: tip,
        buttons: [{ l10n: { id: "urlbar-search-tips-confirm" } }],
        icon,
        titleL10n,
      },
    });
  }
}

async function isBrowserShowingNotification(window) {
  if (
    window.gURLBar.view.isOpen ||
    window.gNotificationBox.currentNotification ||
    window.gBrowser.getNotificationBox().currentNotification
  ) {
    return true;
  }

  if (
    lazy.AppMenuNotifications.activeNotification &&
    !lazy.AppMenuNotifications.activeNotification.dismissed &&
    !lazy.AppMenuNotifications.activeNotification.options.badgeOnly
  ) {
    return true;
  }

  if (window.PopupNotifications.isPanelOpen) {
    return true;
  }

  let pageActions = window.document.getElementById("page-action-buttons");
  if (pageActions) {
    for (let child of pageActions.childNodes) {
      if (child.getAttribute("open") == "true") {
        return true;
      }
    }
  }

  let navbar = window.document.getElementById("nav-bar-customization-target");
  for (let node of navbar.querySelectorAll("toolbarbutton")) {
    if (node.getAttribute("open") == "true") {
      return true;
    }
  }

  if (window.gDialogBox.isOpen) {
    return true;
  }

  const willPrompt = await lazy.DefaultBrowserCheck.willCheckDefaultBrowser(
     false
  );
  if (willPrompt) {
    return true;
  }

  return false;
}

async function isDefaultEngineHomepage(urlStr) {
  let defaultEngine = await lazy.SearchService.getDefault();
  if (!defaultEngine) {
    return false;
  }

  let homepageMatches = lazy.SUPPORTED_ENGINES.get(defaultEngine.name);
  if (!homepageMatches) {
    return false;
  }

  let url = URL.parse(urlStr);
  if (!url) {
    return false;
  }

  if (url.searchParams.has(homepageMatches.prohibitedSearchParams)) {
    return false;
  }

  urlStr = url.hostname.concat(url.pathname);

  return homepageMatches.domainPath.test(urlStr);
}
