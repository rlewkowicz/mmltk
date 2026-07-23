/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */



import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import {
  AboutNewTabComponentRegistry,
  BaseAboutNewTabComponentRegistrant,
} from "moz-src:///browser/components/newtab/AboutNewTabComponents.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  ConfigSearchEngine:
    "moz-src:///toolkit/components/search/ConfigSearchEngine.sys.mjs",
  CustomizableUI:
    "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  SearchEngineInstallError:
    "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  SearchUtils: "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  SearchUIUtilsL10n: () => {
    return new Localization(["browser/search.ftl", "branding/brand.ftl"]);
  },
});

export var SearchUIUtils = {
  initialized: false,

  init() {
    if (!this.initialized) {
      Services.obs.addObserver(this, "browser-search-engine-modified");
      this.initialized = true;
    }
  },

  observe(subject, topic, data) {
    switch (data) {
      case "engine-default":
        this.updatePlaceholderNamePreference(subject.wrappedJSObject, false);
        break;
      case "engine-default-private":
        this.updatePlaceholderNamePreference(subject.wrappedJSObject, true);
        break;
    }
  },

  showSearchServiceNotification(notificationType, ...args) {
    switch (notificationType) {
      case "search-engine-removal": {
        let [oldEngine, newEngine] = args;
        this.removalOfSearchEngineNotificationBox(oldEngine, newEngine);
        break;
      }
      case "search-settings-reset": {
        let [newEngine] = args;
        this.searchSettingsResetNotificationBox(newEngine);
        break;
      }
    }
  },

  async removalOfSearchEngineNotificationBox(oldEngine, newEngine) {
    let win = lazy.BrowserWindowTracker.getTopWindow({
      allowFromInactiveWorkspace: true,
    });

    let buttons = [
      {
        "l10n-id": "remove-search-engine-button",
        primary: true,
        callback() {
          const notificationBox = win.gNotificationBox.getNotificationWithValue(
            "search-engine-removal"
          );
          win.gNotificationBox.removeNotification(notificationBox);
        },
      },
      {
        supportPage: "search-engine-removal",
      },
    ];

    await win.gNotificationBox.appendNotification(
      "search-engine-removal",
      {
        label: {
          "l10n-id": "removed-search-engine-message2",
          "l10n-args": { oldEngine, newEngine },
        },
        priority: win.gNotificationBox.PRIORITY_SYSTEM,
      },
      buttons
    );

    SearchUIUtils.updatePlaceholderNamePreference(
      await lazy.SearchService.getDefault(),
      false
    );
    SearchUIUtils.updatePlaceholderNamePreference(
      await lazy.SearchService.getDefaultPrivate(),
      true
    );

    for (let openWin of lazy.BrowserWindowTracker.orderedWindows) {
      openWin.gURLBar
        ?._updatePlaceholderFromDefaultEngine()
        .catch(console.error);
    }
  },

  async searchSettingsResetNotificationBox(newEngine) {
    let win = lazy.BrowserWindowTracker.getTopWindow({
      allowFromInactiveWorkspace: true,
    });

    let buttons = [
      {
        "l10n-id": "reset-search-settings-button",
        primary: true,
        callback() {
          const notificationBox = win.gNotificationBox.getNotificationWithValue(
            "search-settings-reset"
          );
          win.gNotificationBox.removeNotification(notificationBox);
        },
      },
      {
        supportPage: "prefs-search",
      },
    ];

    await win.gNotificationBox.appendNotification(
      "search-settings-reset",
      {
        label: {
          "l10n-id": "reset-search-settings-message",
          "l10n-args": { newEngine },
        },
        priority: win.gNotificationBox.PRIORITY_SYSTEM,
      },
      buttons
    );
  },

  async addOpenSearchEngine(locationURL, image, browsingContext) {
    try {
      await lazy.SearchService.addOpenSearchEngine(
        locationURL,
        image,
        browsingContext?.embedderElement?.contentPrincipal?.originAttributes
      );
    } catch (ex) {
      let titleMsgName = "opensearch-error-download-title";
      let descMsgName = "opensearch-error-download-desc";

      if (ex instanceof lazy.SearchEngineInstallError) {
        switch (ex.type) {
          case "duplicate-title":
            titleMsgName = "opensearch-error-duplicate-title";
            descMsgName = "opensearch-error-duplicate-desc";
            break;
          case "corrupted":
            titleMsgName = "opensearch-error-format-title";
            descMsgName = "opensearch-error-format-desc";
            break;
          default:
        }
      }

      let [title, text] = await lazy.SearchUIUtilsL10n.formatValues([
        {
          id: titleMsgName,
        },
        {
          id: descMsgName,
          args: {
            "location-url": locationURL,
          },
        },
      ]);

      Services.prompt.alertBC(
        browsingContext,
        Ci.nsIPrompt.MODAL_TYPE_CONTENT,
        title,
        text
      );
      return false;
    }
    return true;
  },

  get searchEnginesURL() {
    return Services.urlFormatter.formatURLPref(
      "browser.search.searchEnginesURL"
    );
  },

  updatePlaceholderNamePreference(engine, isPrivate) {
    const prefName =
      "browser.urlbar.placeholderName" + (isPrivate ? ".private" : "");
    if (engine instanceof lazy.ConfigSearchEngine) {
      Services.prefs.setStringPref(prefName, engine.name);
    } else {
      Services.prefs.clearUserPref(prefName);
    }
  },

  webSearch(window) {
    if (
      window.location.href != AppConstants.BROWSER_CHROME_URL ||
      window.gURLBar.readOnly
    ) {
      let topWindow = lazy.BrowserWindowTracker.getTopWindow();
      if (topWindow && !topWindow.gURLBar.readOnly) {
        topWindow.focus();
        SearchUIUtils.webSearch(topWindow);
      } else {
        let newWindow = window.openDialog(
          AppConstants.BROWSER_CHROME_URL,
          "_blank",
          "chrome,all,dialog=no",
          "about:blank"
        );

        let observer = subject => {
          if (subject == newWindow) {
            SearchUIUtils.webSearch(newWindow);
            Services.obs.removeObserver(
              observer,
              "browser-delayed-startup-finished"
            );
          }
        };
        Services.obs.addObserver(observer, "browser-delayed-startup-finished");
      }
      return;
    }

    let focusUrlBarIfSearchFieldIsNotActive = function (searchBar) {
      if (!searchBar || window.document.activeElement != searchBar.inputField) {
        window.gURLBar.searchModeShortcut();
      }
    };

    let searchBar =  (
      window.document.getElementById(
        Services.prefs.getBoolPref("browser.search.widget.new")
          ? "searchbar-new"
          : "searchbar"
      )
    );
    let placement =
      lazy.CustomizableUI.getPlacementOfWidget("search-container");
    let focusSearchBar = () => {
      searchBar =  (
        window.document.getElementById(
          Services.prefs.getBoolPref("browser.search.widget.new")
            ? "searchbar-new"
            : "searchbar"
        )
      );
      searchBar.select();
      focusUrlBarIfSearchFieldIsNotActive(searchBar);
    };
    if (
      placement &&
      searchBar &&
      ((searchBar.parentElement.getAttribute("overflowedItem") == "true" &&
        placement.area == lazy.CustomizableUI.AREA_NAVBAR) ||
        placement.area == lazy.CustomizableUI.AREA_FIXED_OVERFLOW_PANEL)
    ) {
      let navBar = window.document.getElementById(
        lazy.CustomizableUI.AREA_NAVBAR
      );
      // @ts-expect-error - Navbar receives the overflowable property upon registration.
      navBar.overflowable.show().then(focusSearchBar);
      return;
    }
    if (searchBar) {
      if (window.fullScreen) {
        window.FullScreen.showNavToolbox();
      }
      searchBar.select();
    }
    focusUrlBarIfSearchFieldIsNotActive(searchBar);
  },

  async loadSearch({
    window,
    searchText,
    where,
    usePrivateWindow = lazy.PrivateBrowsingUtils.isWindowPrivate(window),
    triggeringPrincipal,
    policyContainer,
    inBackground = false,
    engine,
    tab,
    searchUrlType,
    sapSource,
    avoidBrowserFocus = false,
  }) {
    if (!triggeringPrincipal) {
      throw new Error(
        "Required argument triggeringPrincipal missing within loadSearch"
      );
    }

    if (!engine) {
      engine = usePrivateWindow
        ? await lazy.SearchService.getDefaultPrivate()
        : await lazy.SearchService.getDefault();
    }

    let submission = engine.getSubmission(searchText, searchUrlType);

    if (!submission) {
      throw new Error(`No submission URL found for ${searchUrlType}`);
    }

    window.openLinkIn(submission.uri.spec, where || "current", {
      private: usePrivateWindow,
      postData: submission.postData,
      inBackground,
      relatedToCurrent: true,
      triggeringPrincipal,
      policyContainer,
      targetBrowser: tab?.linkedBrowser,
      avoidBrowserFocus,
      globalHistoryOptions: {
        triggeringSearchEngine: engine.name,
      },
    });

  },

  async loadSearchFromContext({
    window,
    engine,
    searchText,
    usePrivateWindow,
    triggeringPrincipal,
    policyContainer,
    event,
    searchUrlType = null,
  }) {
    event = lazy.BrowserUtils.getRootEvent(event);
    let where = lazy.BrowserUtils.whereToOpenLink(event);
    if (where == "current") {
      where = "tab";
    }
    if (
      usePrivateWindow &&
      !lazy.PrivateBrowsingUtils.isWindowPrivate(window)
    ) {
      where = "window";
    }
    let inBackground = Services.prefs.getBoolPref(
      "browser.search.context.loadInBackground"
    );
    if (event.button == 1 || event.ctrlKey) {
      inBackground = !inBackground;
    }

    return this.loadSearch({
      window,
      engine,
      searchText,
      searchUrlType,
      where,
      usePrivateWindow,
      triggeringPrincipal: Services.scriptSecurityManager.createNullPrincipal(
        triggeringPrincipal.originAttributes
      ),
      policyContainer,
      inBackground,
      sapSource:
        searchUrlType == lazy.SearchUtils.URL_TYPE.VISUAL_SEARCH
          ? "contextmenu_visual"
          : "contextmenu",
    });
  },
};

export class SearchNewTabComponentsRegistrant extends BaseAboutNewTabComponentRegistrant {
  constructor() {
    super();
    this.lazy = XPCOMUtils.declareLazy({
      prefHandoffToAwesomebar: {
        pref: "browser.newtabpage.activity-stream.improvesearch.handoffToAwesomebar",
        default: true,
        onUpdate: () => {
          this.updated();
        },
      },
    });
  }

  getComponents() {
    const { caretBlinkCount, caretBlinkTime } = Services.appinfo;

    return [
      {
        type: AboutNewTabComponentRegistry.TYPES.SEARCH,
        l10nURLs: [],
        componentURL: "chrome://browser/content/contentSearchHandoffUI.mjs",
        tagName: "content-search-handoff-ui",
        cssVariables: {
          "--caret-blink-count":
            caretBlinkCount > -1 ? caretBlinkCount : "infinite",
          "--caret-blink-time":
            caretBlinkTime > 0 ? `${caretBlinkTime * 2}ms` : `${1134}ms`,
        },
        attributes: {
          nonhandoff: !this.lazy.prefHandoffToAwesomebar,
        },
      },
    ];
  }
}
