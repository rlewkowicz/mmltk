/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  NimbusFeatures: "resource://nimbus/ExperimentAPI.sys.mjs",
  Preferences: "resource://gre/modules/Preferences.sys.mjs",
  Region: "resource://gre/modules/Region.sys.mjs",
  TelemetryEnvironment: "resource://gre/modules/TelemetryEnvironment.sys.mjs",
  TelemetryReportingPolicy:
    "resource://gre/modules/TelemetryReportingPolicy.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
});

const SETTINGS_UI = Object.freeze({
  FULL: 0,
  NONE: 1,
  OFFLINE_ONLY: 2,
});

const SUGGEST_TOU_TIMESTAMP = 1765800000000;

const EN_LOCALES = ["en-CA", "en-GB", "en-US", "en-ZA"];



const SUGGEST_PREFS = Object.freeze({
  "quicksuggest.enabled": {
    defaultValues: {
      DE: [["de", ...EN_LOCALES], true],
      FR: [["fr", ...EN_LOCALES], true],
      GB: [EN_LOCALES, true],
      IT: [["it", ...EN_LOCALES], true],
      US: [EN_LOCALES, true],
    },
  },
  "quicksuggest.online.available": {
    defaultValues: {
      US: [EN_LOCALES, shouldOnlineBeAvailable],
    },
  },
  "quicksuggest.settingsUi": {
    defaultValues: {
      DE: [["de"], SETTINGS_UI.OFFLINE_ONLY],
      FR: [["fr"], SETTINGS_UI.OFFLINE_ONLY],
      GB: [EN_LOCALES, SETTINGS_UI.OFFLINE_ONLY],
      IT: [["it"], SETTINGS_UI.OFFLINE_ONLY],
      US: [
        EN_LOCALES,
        () => {
          return shouldOnlineBeAvailable()
            ? SETTINGS_UI.FULL
            : SETTINGS_UI.OFFLINE_ONLY;
        },
      ],
    },
  },
  "suggest.quicksuggest.all": {
    defaultValues: {
      DE: [["de"], true],
      FR: [["fr"], true],
      GB: [EN_LOCALES, true],
      IT: [["it"], true],
      US: [EN_LOCALES, true],
    },
  },
  "suggest.quicksuggest.sponsored": {
    nimbusVariableIfExposedInUi: "quickSuggestSponsoredEnabled",
    defaultValues: {
      DE: [["de"], true],
      FR: [["fr"], true],
      GB: [EN_LOCALES, true],
      IT: [["it"], true],
      US: [EN_LOCALES, true],
    },
  },

  "addons.featureGate": {
    defaultValues: {
      US: [EN_LOCALES, true],
    },
  },
  "amp.featureGate": {
    defaultValues: {
      GB: [EN_LOCALES, true],
      US: [EN_LOCALES, true],
    },
  },
  "flightStatus.featureGate": {
    defaultValues: {
      US: [EN_LOCALES, shouldOnlineBeAvailable],
    },
  },
  "importantDates.featureGate": {
    defaultValues: {
      DE: [["de", ...EN_LOCALES], true],
      FR: [["fr", ...EN_LOCALES], true],
      GB: [EN_LOCALES, true],
      IT: [["it", ...EN_LOCALES], true],
      US: [EN_LOCALES, true],
    },
  },
  "market.featureGate": {
    defaultValues: {
      US: [EN_LOCALES, shouldOnlineBeAvailable],
    },
  },
  "mdn.featureGate": {
    defaultValues: {
      US: [EN_LOCALES, true],
    },
  },
  "sports.featureGate": {
    defaultValues: {
      US: [EN_LOCALES, shouldOnlineBeAvailable],
    },
  },
  "weather.featureGate": {
    defaultValues: {
      DE: [["de"], true],
      FR: [["fr"], true],
      GB: [EN_LOCALES, true],
      IT: [["it"], true],
      US: [EN_LOCALES, true],
    },
  },
  "wikipedia.featureGate": {
    defaultValues: {
      GB: [EN_LOCALES, true],
      US: [EN_LOCALES, true],
    },
  },
  "yelp.featureGate": {
    defaultValues: {
      US: [EN_LOCALES, true],
    },
  },
});

const FEATURES = {
  AmpSuggestions:
    "moz-src:///browser/components/urlbar/private/AmpSuggestions.sys.mjs",
  DynamicSuggestions:
    "moz-src:///browser/components/urlbar/private/DynamicSuggestions.sys.mjs",
  FlightStatusSuggestions:
    "moz-src:///browser/components/urlbar/private/FlightStatusSuggestions.sys.mjs",
  ImportantDatesSuggestions:
    "moz-src:///browser/components/urlbar/private/ImportantDatesSuggestions.sys.mjs",
  ImpressionCaps:
    "moz-src:///browser/components/urlbar/private/ImpressionCaps.sys.mjs",
  MarketSuggestions:
    "moz-src:///browser/components/urlbar/private/MarketSuggestions.sys.mjs",
  MDNSuggestions:
    "moz-src:///browser/components/urlbar/private/MDNSuggestions.sys.mjs",
  SportsSuggestions:
    "moz-src:///browser/components/urlbar/private/SportsSuggestions.sys.mjs",
  SuggestBackendMerino:
    "moz-src:///browser/components/urlbar/private/SuggestBackendMerino.sys.mjs",
  SuggestBackendMl:
    "moz-src:///browser/components/urlbar/private/SuggestBackendMl.sys.mjs",
  SuggestBackendRust:
    "moz-src:///browser/components/urlbar/private/SuggestBackendRust.sys.mjs",
  WeatherSuggestions:
    "moz-src:///browser/components/urlbar/private/WeatherSuggestions.sys.mjs",
  WikipediaSuggestions:
    "moz-src:///browser/components/urlbar/private/WikipediaSuggestions.sys.mjs",
  YelpRealtimeSuggestions:
    "moz-src:///browser/components/urlbar/private/YelpRealtimeSuggestions.sys.mjs",
  YelpSuggestions:
    "moz-src:///browser/components/urlbar/private/YelpSuggestions.sys.mjs",
};


class _QuickSuggest {
  _testSkipTelemetryEnvironmentInit = false;

  get HELP_URL() {
    return (
      Services.urlFormatter.formatURLPref("app.support.baseURL") +
      this.HELP_TOPIC
    );
  }

  get HELP_TOPIC() {
    return "firefox-suggest";
  }

  get SETTINGS_UI() {
    return SETTINGS_UI;
  }

  get SUGGEST_TOU_TIMESTAMP() {
    return SUGGEST_TOU_TIMESTAMP;
  }

  get initPromise() {
    return this.#initResolvers.promise;
  }

  get enabledBackends() {
    return [
      this.rustBackend,
      this.#featuresByName.get("SuggestBackendMerino"),
      this.#featuresByName.get("SuggestBackendMl"),
    ].filter(b => b?.isEnabled);
  }

  get rustBackend() {
    return this.#featuresByName.get("SuggestBackendRust");
  }

  get config() {
    return this.rustBackend?.config || {};
  }

  get impressionCaps() {
    return this.#featuresByName.get("ImpressionCaps");
  }

  get rustFeatures() {
    return new Set([
      ...this.#featuresByRustSuggestionType.values(),
      ...this.#featuresByDynamicRustSuggestionType.values(),
    ]);
  }

  get mlFeatures() {
    return new Set(this.#featuresByMlIntent.values());
  }

  get logger() {
    if (!this._logger) {
      this._logger = lazy.UrlbarShared.getLogger({ prefix: "QuickSuggest" });
    }
    return this._logger;
  }

  async init(testOverrides = null) {
    if (this.#initStarted) {
      await this.initPromise;
      return;
    }
    this.#initStarted = true;

    await lazy.Region.init();

    await lazy.NimbusFeatures.urlbar.ready();

    if (!this._testSkipTelemetryEnvironmentInit) {
      await lazy.TelemetryEnvironment.onInitialized();
    }

    this.#initPrefs(testOverrides);

    for (let [name, uri] of Object.entries(FEATURES)) {
      let { [name]: ctor } = ChromeUtils.importESModule(uri);
      let feature = new ctor();
      this.#featuresByName.set(name, feature);
      if (feature.merinoProvider) {
        this.#featuresByMerinoProvider.set(feature.merinoProvider, feature);
      }
      if (feature.rustSuggestionType) {
        if (feature.dynamicRustSuggestionTypes?.length) {
          for (let t of feature.dynamicRustSuggestionTypes) {
            this.#featuresByDynamicRustSuggestionType.set(t, feature);
          }
        } else {
          this.#featuresByRustSuggestionType.set(
            feature.rustSuggestionType,
            feature
          );
        }
      }
      if (feature.mlIntent) {
        this.#featuresByMlIntent.set(feature.mlIntent, feature);
      }

      let prefs = feature.enablingPreferences;
      if (prefs) {
        for (let p of prefs) {
          let features = this.#featuresByEnablingPrefs.get(p);
          if (!features) {
            features = new Set();
            this.#featuresByEnablingPrefs.set(p, features);
          }
          features.add(feature);
        }
      }
    }

    this.#updateAll();
    lazy.UrlbarPrefs.addObserver(this);

    this.#initResolvers.resolve();
  }

  getFeature(name) {
    return this.#featuresByName.get(name);
  }

  getFeatureByMlIntent(intent) {
    return this.#featuresByMlIntent.get(intent);
  }

  getFeatureByResult(result) {
    return this.getFeatureBySource(result.payload);
  }

  getFeatureBySource({ source, provider, suggestionType }) {
    switch (source) {
      case "merino":
        return this.#featuresByMerinoProvider.get(provider);
      case "rust":
        if (provider == "Dynamic" && suggestionType) {
          let dynamicFeature =
            this.#featuresByDynamicRustSuggestionType.get(suggestionType);
          if (dynamicFeature) {
            return dynamicFeature;
          }
        }
        return this.#featuresByRustSuggestionType.get(provider);
      case "ml":
        return this.getFeatureByMlIntent(provider);
    }
    return null;
  }

  async dismissResult(result) {
    if (result.payload.source == "rust") {
      await this.rustBackend?.dismissRustSuggestion(
        result.payload.suggestionObject
      );
    } else {
      let key = getDismissalKey(result);
      if (key) {
        await this.rustBackend?.dismissByKey(key);
      }
    }

    Services.obs.notifyObservers(null, "quicksuggest-dismissals-changed");
  }

  async isResultDismissed(result) {
    let promises = [
      getDigest(result.payload.originalUrl || result.payload.url).then(digest =>
        this.rustBackend?.isDismissedByKey(digest)
      ),
    ];

    if (result.payload.source == "rust") {
      promises.push(
        this.rustBackend?.isRustSuggestionDismissed(
          result.payload.suggestionObject
        )
      );
    } else {
      let key = getDismissalKey(result);
      if (key) {
        promises.push(this.rustBackend?.isDismissedByKey(key));
      }
    }

    let values = await Promise.all(promises);
    return values.some(v => !!v);
  }

  async clearDismissedSuggestions() {
    for (let [name, feature] of this.#featuresByName) {
      for (let pref of feature.primaryUserControlledPreferences) {
        try {
          if (pref && !lazy.UrlbarPrefs.get(pref)) {
            lazy.UrlbarPrefs.clear(pref);
          }
        } catch (error) {
          this.logger.error("Error clearing primaryEnablingPreference", {
            "feature.name": name,
            pref,
            error,
          });
        }
      }
    }

    await this.rustBackend?.clearDismissedSuggestions();

    Services.obs.notifyObservers(null, "quicksuggest-dismissals-changed");
    Services.obs.notifyObservers(null, "quicksuggest-dismissals-cleared");
  }

  async canClearDismissedSuggestions() {
    for (let [name, feature] of this.#featuresByName) {
      for (let pref of feature.primaryUserControlledPreferences) {
        try {
          if (
            pref &&
            !lazy.UrlbarPrefs.get(pref) &&
            lazy.UrlbarPrefs.hasUserValue(pref)
          ) {
            return true;
          }
        } catch (error) {
          this.logger.error(
            "Error accessing primaryUserControlledPreferences",
            {
              "feature.name": name,
              pref,
              error,
            }
          );
        }
      }
    }

    if (await this.rustBackend?.anyDismissedSuggestions()) {
      return true;
    }

    return false;
  }

  intendedDefaultPrefs(region, locale) {
    let regionLocalePrefs = Object.fromEntries(
      Object.entries(SUGGEST_PREFS)
        .map(([prefName, { defaultValues }]) => {
          if (defaultValues?.hasOwnProperty(region)) {
            let [enablingLocales, prefValue] = defaultValues[region];
            if (enablingLocales.includes(locale)) {
              if (typeof prefValue == "function") {
                prefValue = prefValue();
              }
              return [prefName, prefValue];
            }
          }
          return null;
        })
        .filter(entry => !!entry)
    );
    return {
      ...this.#unmodifiedDefaultPrefs,
      ...regionLocalePrefs,
    };
  }

  onPrefChanged(pref) {
    if (pref == lazy.TelemetryReportingPolicy.TOU_ACCEPTED_DATE_PREF) {
      this.#initPrefs();
    }

    let features = this.#featuresByEnablingPrefs.get(pref);
    if (!features) {
      return;
    }

    let isPrimaryUserControlledPref = false;

    for (let f of features) {
      f.update();
      if (f.primaryUserControlledPreferences.includes(pref)) {
        isPrimaryUserControlledPref = true;
      }
    }

    if (isPrimaryUserControlledPref) {
      Services.obs.notifyObservers(null, "quicksuggest-dismissals-changed");
    }
  }

  onNimbusChanged(variable) {
    this.#syncNimbusVariablesToUiPrefs(variable);

    this.#updateAll();
  }

  isUrlEquivalentToResultUrl(url, result) {
    let feature = this.getFeatureByResult(result);
    return feature
      ? feature.isUrlEquivalentToResultUrl(url, result)
      : url == result.payload.url;
  }

  getFullKeywordTitleAndHighlights({
    tokens,
    highlightType,
    fullKeyword,
    title,
  }) {
    return {
      value: fullKeyword ? `${fullKeyword} — ${title}` : title,
      highlights: fullKeyword
        ? lazy.UrlbarUtils.getTokenMatches(tokens, fullKeyword, highlightType)
        : [],
    };
  }

  get #uiPrefsByNimbusVariable() {
    return Object.fromEntries(
      Object.entries(SUGGEST_PREFS)
        .map(([prefName, { nimbusVariableIfExposedInUi }]) =>
          nimbusVariableIfExposedInUi
            ? [nimbusVariableIfExposedInUi, prefName]
            : null
        )
        .filter(entry => !!entry)
    );
  }

  #initPrefs(testOverrides = null) {

    let defaults = new lazy.Preferences({
      branch: "browser.urlbar.",
      defaultBranch: true,
    });

    if (!this.#unmodifiedDefaultPrefs) {
      this.#unmodifiedDefaultPrefs = Object.fromEntries(
        Object.keys(SUGGEST_PREFS).map(name => [name, defaults.get(name)])
      );
    }

    if (testOverrides?.defaultPrefs) {
      this.#intendedDefaultPrefs = testOverrides.defaultPrefs;
    } else {
      let region = testOverrides?.region ?? lazy.Region.home;
      let locale = testOverrides?.locale ?? Services.locale.appLocaleAsBCP47;
      this.#intendedDefaultPrefs = this.intendedDefaultPrefs(region, locale);
    }

    for (let [name, value] of Object.entries(this.#intendedDefaultPrefs)) {
      defaults.set(name, value);
    }

    this.#syncNimbusVariablesToUiPrefs();

    let shouldEnableSuggest =
      !!this.#intendedDefaultPrefs["quicksuggest.enabled"];
    this.#ensureUserPrefsMigrated(shouldEnableSuggest, testOverrides);
  }

  #syncNimbusVariablesToUiPrefs(variable = null) {
    let prefsByVariable = this.#uiPrefsByNimbusVariable;

    if (variable) {
      if (!prefsByVariable.hasOwnProperty(variable)) {
        return;
      }
      prefsByVariable = { [variable]: prefsByVariable[variable] };
    }

    let defaults = new lazy.Preferences({
      branch: "browser.urlbar.",
      defaultBranch: true,
    });

    for (let [v, pref] of Object.entries(prefsByVariable)) {
      let value = lazy.NimbusFeatures.urlbar.getVariable(v);
      if (value === undefined) {
        value = this.#intendedDefaultPrefs[pref];
      }
      defaults.set(pref, value);
    }
  }

  #updateAll() {

    for (let feature of this.#featuresByName.values()) {
      feature.update();
    }
  }

  get MIGRATION_VERSION() {
    return 7;
  }

  #ensureUserPrefsMigrated(shouldEnableSuggest, testOverrides) {
    let currentVersion =
      testOverrides?.migrationVersion !== undefined
        ? testOverrides.migrationVersion
        : this.MIGRATION_VERSION;
    let lastSeenVersion = Math.max(
      0,
      lazy.UrlbarPrefs.get("quicksuggest.migrationVersion")
    );
    if (currentVersion <= lastSeenVersion) {
      return;
    }

    let userBranch = Services.prefs.getBranch("browser.urlbar.");
    let version = lastSeenVersion;
    for (; version < currentVersion; version++) {
      let nextVersion = version + 1;
      let methodName = "_migrateUserPrefsTo_" + nextVersion;
      try {
        this[methodName](userBranch, shouldEnableSuggest);
      } catch (error) {
        console.error(
          `Error migrating Firefox Suggest prefs to version ${nextVersion}:`,
          error
        );
        break;
      }
    }

    lazy.UrlbarPrefs.set("quicksuggest.migrationVersion", version);
  }

  _migrateUserPrefsTo_1(userBranch, shouldEnableSuggest) {

    if (userBranch.prefHasUserValue("suggest.quicksuggest")) {
      userBranch.setBoolPref(
        "suggest.quicksuggest.nonsponsored",
        userBranch.getBoolPref("suggest.quicksuggest")
      );
      userBranch.clearUserPref("suggest.quicksuggest");
    }

    if (
      shouldEnableSuggest &&
      userBranch.prefHasUserValue("suggest.quicksuggest.nonsponsored") &&
      !userBranch.getBoolPref("suggest.quicksuggest.nonsponsored")
    ) {
      userBranch.setBoolPref("suggest.quicksuggest.sponsored", false);
    }
  }

  _migrateUserPrefsTo_2(userBranch) {

    let scenario = userBranch.getCharPref("quicksuggest.scenario", "");
    if (scenario == "online") {
      if (!userBranch.prefHasUserValue("suggest.quicksuggest.nonsponsored")) {
        userBranch.setBoolPref("suggest.quicksuggest.nonsponsored", false);
      }
      if (!userBranch.prefHasUserValue("suggest.quicksuggest.sponsored")) {
        userBranch.setBoolPref("suggest.quicksuggest.sponsored", false);
      }
    }
  }

  _migrateUserPrefsTo_3() {
  }

  _migrateUserPrefsTo_4(userBranch) {
    userBranch.clearUserPref("quicksuggest.settingsUi");
  }

  _migrateUserPrefsTo_5(userBranch) {
    if (
      ["DE", "FR", "IT"].includes(lazy.Region.home) &&
      EN_LOCALES.includes(Services.locale.appLocaleAsBCP47)
    ) {
      userBranch.clearUserPref("suggest.quicksuggest.sponsored");
    }
  }

  _migrateUserPrefsTo_6(userBranch) {
    if (userBranch.prefHasUserValue("suggest.quicksuggest.nonsponsored")) {
      userBranch.setBoolPref(
        "suggest.quicksuggest.all",
        userBranch.getBoolPref("suggest.quicksuggest.nonsponsored")
      );
    }
  }

  _migrateUserPrefsTo_7(userBranch) {
    if (
      userBranch.prefHasUserValue("addons.minKeywordLength") &&
      !userBranch.prefHasUserValue("addons.showLessFrequentlyCount")
    ) {
      userBranch.setIntPref("addons.showLessFrequentlyCount", 1);
    } else if (
      !userBranch.prefHasUserValue("addons.minKeywordLength") &&
      userBranch.prefHasUserValue("addons.showLessFrequentlyCount")
    ) {
      userBranch.setIntPref("addons.minKeywordLength", 20);
    }
  }

  async _test_reset(testOverrides = null) {
    if (this.#initStarted) {
      await this.initPromise;
    }

    if (this.rustBackend) {
      await this.rustBackend.ingestPromise;
    }

    this.#initPrefs(testOverrides);
    this.#updateAll();
    if (this.rustBackend) {
      await this.rustBackend.ingestPromise;
    }
  }

  #initStarted = false;
  #initResolvers = Promise.withResolvers();

  #featuresByName = new Map();

  #featuresByMerinoProvider = new Map();

  #featuresByRustSuggestionType = new Map();

  #featuresByDynamicRustSuggestionType = new Map();

  #featuresByMlIntent = new Map();

  #featuresByEnablingPrefs = new Map();

  #intendedDefaultPrefs;

  #unmodifiedDefaultPrefs;
}

function userAcceptedSuggestToU() {
  let date = lazy.TelemetryReportingPolicy.termsOfUseAcceptedDate;
  return !!date && SUGGEST_TOU_TIMESTAMP <= date.getTime();
}

function shouldOnlineBeAvailable() {
  return userAcceptedSuggestToU();
}

function getDismissalKey(result) {
  return (
    result.payload.dismissalKey ||
    result.payload.originalUrl ||
    result.payload.url
  );
}

async function getDigest(string) {
  let stringArray = new TextEncoder().encode(string);
  let hashBuffer = await crypto.subtle.digest("SHA-1", stringArray);
  let hashArray = new Uint8Array(hashBuffer);
  return Array.from(hashArray, b => b.toString(16).padStart(2, "0")).join("");
}

export const QuickSuggest = new _QuickSuggest();
