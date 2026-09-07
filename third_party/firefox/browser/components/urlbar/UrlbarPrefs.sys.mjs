/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  CustomizableUI:
    "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs",
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
});

const PREF_URLBAR_BRANCH = "browser.urlbar.";


const PREF_URLBAR_DEFAULTS =  ([
  ["autoFill", true],
  ["autoFill.adaptiveHistory.enabled", false],
  ["autoFill.backspaceBlockDurationMs", 172800000],
  ["autoFill.backspaceThreshold", 3],
  ["autoFill.dismissalBlockDurationMs", 604800000],
  ["autoFill.adaptiveHistory.minCharsThreshold", 0],
  ["autoFill.adaptiveHistory.useCountThreshold", [0.47, "float"]],
  ["closeOtherPanelsOnOpen", true],
  ["contextMenu.featureGate", false],
  ["contextualSearch.enabled", true],
  ["ctrlCanonizesURLs", true],
  ["decodeURLsOnCopy", false],
  ["delay", 50],
  ["dnsResolveFullyQualifiedNames", true],
  ["dnsResolveSingleWordsAfterSearch", 0],
  ["events.disableSuggest.maxSecondsFromLastSearch", 300],
  ["events.bounce.maxSecondsFromLastSearch", 10],
  ["filter.javascript", true],
  ["focusContentDocumentOnEsc", true],
  ["formatting.enabled", true],
  ["groupLabels.enabled", true],
  ["intentThreshold", [0.5, "float"]],
  ["ipc.chromeMessagePassing", false],
  ["keepPanelOpenDuringImeComposition", false],
  ["loglevel", "Error"],
  ["maxRichResults", 10],
  ["openintab", false],
  ["restyleSearches", false],
  ["resultMenu.keyboardAccessible", true],
  ["removeStaleRowsTimeout", 400],
  ["searchRestrictKeywords.featureGate", false],
  ["secondaryActions.featureGate", false],
  ["secondaryActions.maxActionsShown", 3],
  ["secondaryActions.switchToTab", false],
  ["shortcuts.bookmarks", true],
  ["shortcuts.tabs", true],
  ["shortcuts.history", true],
  ["shortcuts.actions", true],
  ["showSearchTerms.enabled", true],
  ["showSearchTerms.featureGate", false],
  ["speculativeConnect.enabled", true],
  ["suggest.bookmark", true],
  ["suggest.engines", true],
  ["suggest.history", true],
  ["suggest.openpage", true],
  ["deduplication.enabled", true],
  ["deduplication.thresholdDays", 0],
  ["switchTabs.adoptIntoActiveWindow", false],
  ["tabGroups.minSearchLength", 1],
  ["trustPanel.featureGate", false],
  ["trimHttps", false],
  ["trimURLs", true],
  ["untrimOnUserInteraction.featureGate", false],
  ["unifiedSearchButton.always", false],
  ["unifiedSearchButton.historyInSearchMode", false],
]);

const PREF_URLBAR_DEFAULTS_MAP = new Map(PREF_URLBAR_DEFAULTS);

const PREF_OTHER_DEFAULTS =  ([
  ["browser.fixup.dns_first_for_single_words", false],
  ["browser.search.openintab", false],
  ["browser.search.suggest.enabled", true],
  ["browser.search.suggest.enabled.private", false],
  ["browser.search.widget.new", true],
  ["keyword.enabled", true],
  ["security.insecure_connection_text.enabled", true],
  ["ui.popup.disable_autohide", false],
]);

const PREF_OTHER_DEFAULTS_MAP = new Map(PREF_OTHER_DEFAULTS);

const SUGGEST_PREF_TO_BEHAVIOR = {
  history: "history",
  bookmark: "bookmark",
  openpage: "openpage",
  searches: "search",
};

const PREF_TYPES = new Map([
  ["boolean", "Bool"],
  ["float", "Float"],
  ["number", "Int"],
  ["string", "Char"],
]);

function makeDefaultResultGroups() {
  let rootGroup = {
    children: [
      {
        maxResultCount: 1,
        children: [
          { group: lazy.UrlbarUtils.RESULT_GROUP.HEURISTIC_TEST },
          { group: lazy.UrlbarUtils.RESULT_GROUP.HEURISTIC_ENGINE_ALIAS },
          { group: lazy.UrlbarUtils.RESULT_GROUP.HEURISTIC_BOOKMARK_KEYWORD },
          { group: lazy.UrlbarUtils.RESULT_GROUP.HEURISTIC_AUTOFILL },
          { group: lazy.UrlbarUtils.RESULT_GROUP.HEURISTIC_TOKEN_ALIAS_ENGINE },
          {
            group:
              lazy.UrlbarUtils.RESULT_GROUP.HEURISTIC_RESTRICT_KEYWORD_AUTOFILL,
          },
          { group: lazy.UrlbarUtils.RESULT_GROUP.HEURISTIC_HISTORY_URL },
          { group: lazy.UrlbarUtils.RESULT_GROUP.HEURISTIC_FALLBACK },
        ],
      },
    ],
  };

  rootGroup.children.push({
    flexChildren: true,
    children: [
      {
        flex: 2,
        group: lazy.UrlbarUtils.RESULT_GROUP.GENERAL,
        orderBy: "frecency",
      },
      {
        flex: 2,
        group: lazy.UrlbarUtils.RESULT_GROUP.ABOUT_PAGES,
      },
      {
        flex: 99,
        group: lazy.UrlbarUtils.RESULT_GROUP.RESTRICT_SEARCH_KEYWORD,
      },
    ],
  });

  return rootGroup;
}


class Preferences {
  constructor() {
    this._map = new Map();
    this.QueryInterface = ChromeUtils.generateQI([
      "nsIObserver",
      "nsISupportsWeakReference",
    ]);

    Services.prefs.addObserver(PREF_URLBAR_BRANCH, this, true);
    for (let pref of PREF_OTHER_DEFAULTS_MAP.keys()) {
      Services.prefs.addObserver(pref, this, true);
    }
    this._observerWeakRefs = [];
    this.addObserver(this);

    this.shouldHandOffToSearchModePrefs = ["keyword.enabled"];

  }

  get(pref) {
    let value = this._map.get(pref);
    if (value === undefined) {
      value = this._getPrefValue(pref);
      this._map.set(pref, value);
    }
    return value;
  }

  set(pref, value) {
    let { defaultValue, set } = this._getPrefDescriptor(pref);
    if (typeof value != typeof defaultValue) {
      throw new Error(`Invalid value type ${typeof value} for pref ${pref}`);
    }
    set(pref, value);
  }

  add(pref, value) {
    let maybeSet = this._getPrefValue(pref);

    if (!(maybeSet instanceof Set)) {
      throw new Error(
        `The pref ${pref} should handle the values as Set but '${typeof maybeSet}'`
      );
    }

    maybeSet.add(value);
    this.set(pref, [...maybeSet].join(","));
  }

  clear(pref) {
    let { clear } = this._getPrefDescriptor(pref);
    clear(pref);
  }

  hasUserValue(pref) {
    let { hasUserValue } = this._getPrefDescriptor(pref);
    return hasUserValue(pref);
  }

  getResultGroups({ context }) {
    let key = context.sapName;
    switch (context.sapName) {
      case "urlbar": {
        return this.#getOrCacheResultGroups(key, makeDefaultResultGroups);
      }
      case "searchbar": {
        return this.#getOrCacheResultGroups(key, makeDefaultResultGroups);
      }
      default: {
        throw new Error(`Unknown SAP name: ${context.sapName}`);
      }
    }
  }

  addObserver(observer) {
    this._observerWeakRefs.push(Cu.getWeakReference(observer));
  }

  removeObserver(observer) {
    for (let i = 0; i < this._observerWeakRefs.length; i++) {
      let obs = this._observerWeakRefs[i].get();
      if (obs && obs == observer) {
        this._observerWeakRefs.splice(i, 1);
        break;
      }
    }
  }

  observe(subject, topic, data) {
    let pref = data.replace(PREF_URLBAR_BRANCH, "");
    if (
      !PREF_URLBAR_DEFAULTS_MAP.has(pref) &&
      !PREF_OTHER_DEFAULTS_MAP.has(pref)
    ) {
      return;
    }
    this.#notifyObservers("onPrefChanged", pref);
  }

  onPrefChanged(pref) {
    this._map.delete(pref);

    switch (pref) {
      case "autoFill.adaptiveHistory.useCountThreshold":
        this._map.delete("autoFillAdaptiveHistoryUseCountThreshold");
        return;
    }

    if (pref.startsWith("suggest.")) {
      this._map.delete("defaultBehavior");
    }

    if (this.shouldHandOffToSearchModePrefs.includes(pref)) {
      this._map.delete("shouldHandOffToSearchMode");
    }
  }

  _readPref(pref) {
    let { defaultValue, get } = this._getPrefDescriptor(pref);
    return get(pref, defaultValue);
  }

  _getPrefValue(pref) {
    switch (pref) {
      case "shortcuts.actions": {
        return this._readPref(pref);
      }
      case "defaultBehavior": {
        let val = 0;
        for (let type of Object.keys(SUGGEST_PREF_TO_BEHAVIOR)) {
          let behavior = `BEHAVIOR_${SUGGEST_PREF_TO_BEHAVIOR[
            type
          ].toUpperCase()}`;
          val |=
            this.get("suggest." + type) && Ci.mozIPlacesAutoComplete[behavior];
        }
        return val;
      }
      case "shouldHandOffToSearchMode":
        return this.shouldHandOffToSearchModePrefs.some(
          prefName => !this.get(prefName)
        );
      case "autoFillAdaptiveHistoryUseCountThreshold": {
        return parseFloat(
          this._readPref("autoFill.adaptiveHistory.useCountThreshold")
        );
      }
    }
    return this._readPref(pref);
  }

  _getPrefDescriptor(pref) {
    let branch = Services.prefs.getBranch(PREF_URLBAR_BRANCH);
    let defaultValue = PREF_URLBAR_DEFAULTS_MAP.get(pref);
    if (defaultValue === undefined) {
      branch = Services.prefs;
      defaultValue = PREF_OTHER_DEFAULTS_MAP.get(pref);
      if (defaultValue === undefined) {
        throw new Error("Trying to access an unknown pref " + pref);
      }
    }

    let type;
    if (!Array.isArray(defaultValue)) {
      type = PREF_TYPES.get(typeof defaultValue);
    } else {
      if (defaultValue.length != 2) {
        throw new Error("Malformed pref def: " + pref);
      }
      [defaultValue, type] = defaultValue;
      type = PREF_TYPES.get(type);
    }
    if (!type) {
      throw new Error("Unknown pref type: " + pref);
    }
    return {
      defaultValue,
      get: branch[`get${type}Pref`],
      set: branch[`set${type == "Float" ? "Char" : type}Pref`],
      clear: branch.clearUserPref,
      hasUserValue: branch.prefHasUserValue,
    };
  }

  isPersistedSearchTermsEnabled() {
    return (
      this.get("showSearchTerms.featureGate") &&
      this.get("showSearchTerms.enabled") &&
      !lazy.CustomizableUI.getPlacementOfWidget("search-container")
    );
  }

  #cachedResultGroups = new Map();
  #getOrCacheResultGroups(key, builder) {
    let groups = this.#cachedResultGroups.get(key);
    if (!groups) {
      groups = builder();
      this.#cachedResultGroups.set(key, groups);
    }
    return groups;
  }

  #notifyObservers(method, changed, ...rest) {
    for (let i = 0; i < this._observerWeakRefs.length; ) {
      let observer = this._observerWeakRefs[i].get();
      if (!observer) {
        this._observerWeakRefs.splice(i, 1);
        continue;
      }
      if (method in observer) {
        try {
          observer[method](changed, ...rest);
        } catch (ex) {
          console.error(ex);
        }
      }
      ++i;
    }
  }
}

export var UrlbarPrefs = new Preferences();
