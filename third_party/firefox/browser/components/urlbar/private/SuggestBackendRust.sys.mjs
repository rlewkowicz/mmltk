/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { SuggestBackend } from "moz-src:///browser/components/urlbar/private/SuggestFeature.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AsyncShutdown: "resource://gre/modules/AsyncShutdown.sys.mjs",
  InterruptKind:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSuggest.sys.mjs",
  ObjectUtils: "resource://gre/modules/ObjectUtils.sys.mjs",
  QuickSuggest: "moz-src:///browser/components/urlbar/QuickSuggest.sys.mjs",
  SharedRemoteSettingsService:
    "resource://gre/modules/RustSharedRemoteSettingsService.sys.mjs",
  SuggestIngestionConstraints:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSuggest.sys.mjs",
  SuggestStoreBuilder:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSuggest.sys.mjs",
  Suggestion:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSuggest.sys.mjs",
  SuggestionProvider:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSuggest.sys.mjs",
  SuggestionProviderConstraints:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSuggest.sys.mjs",
  SuggestionQuery:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSuggest.sys.mjs",
  TaskQueue: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  Utils: "resource://services-settings/Utils.sys.mjs",
});


XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "timerManager",
  "@mozilla.org/updates/timer-manager;1",
  Ci.nsIUpdateTimerManager
);

const SUGGEST_DATA_STORE_BASENAME = "suggest.sqlite";

const INGEST_TIMER_ID = "suggest-ingest";
const INGEST_TIMER_LAST_UPDATE_PREF = `app.update.lastUpdateTime.${INGEST_TIMER_ID}`;

const gSuggestionTypesByCtor = new WeakMap();

export class SuggestBackendRust extends SuggestBackend {
  constructor() {
    super();
    this.#ingestQueue = new lazy.TaskQueue();

    if (!lazy.Utils.shouldSkipRemoteActivity) {
      this.#remoteSettingsService =
        lazy.SharedRemoteSettingsService.rustService();
    }
  }

  get enablingPreferences() {
    return ["quicksuggest.rustEnabled"];
  }

  get config() {
    return this.#config || {};
  }

  get ingestPromise() {
    return this.#ingestQueue.emptyPromise;
  }

  enable(enabled) {
    if (enabled) {
      this.#init();
    } else {
      this.#uninit();
    }
  }

  async query(searchString, { _queryContext, types = null } = {}) {
    if (!this.#store) {
      return [];
    }

    this.logger.debug("Handling query", { searchString });

    let uniqueProviders = new Set();
    let allProviderConstraints = {};
    let typeItems = types
      ? types.map(type => ({ type }))
      : this.#enabledSuggestionTypes;
    for (let { feature, type, provider } of typeItems) {
      if (!provider) {
        provider = this.#providerFromSuggestionType(type);
        if (!provider) {
          throw new Error("Unknown Rust suggestion type: " + type);
        }
      }
      this.logger.debug("Adding type to query", { type, provider });
      uniqueProviders.add(provider);
      if (feature) {
        allProviderConstraints = SuggestBackendRust.mergeProviderConstraints(
          allProviderConstraints,
          feature.rustProviderConstraints
        );
      }
    }

    const { suggestions, queryTimes } = await this.#store.queryWithMetrics(
      new lazy.SuggestionQuery({
        providers: [...uniqueProviders],
        keyword: searchString,
        providerConstraints: new lazy.SuggestionProviderConstraints(
          allProviderConstraints
        ),
      })
    );

    for (let { label, value } of queryTimes) {
    }

    let liftedSuggestions = [];
    for (let s of suggestions) {
      let type = getSuggestionType(s);
      if (!type) {
        continue;
      }

      let suggestion = liftSuggestion(s);
      if (!suggestion) {
        continue;
      }

      suggestion.source = "rust";
      suggestion.provider = type;

      if (suggestion.icon) {
        suggestion.icon_blob = new Blob([suggestion.icon], {
          type: suggestion.iconMimetype ?? "",
        });
        delete suggestion.icon;
        delete suggestion.iconMimetype;
      }

      liftedSuggestions.push(suggestion);
    }

    this.logger.debug("Got suggestions", liftedSuggestions);

    return liftedSuggestions;
  }

  cancelQuery() {
    this.#store?.interrupt(lazy.InterruptKind.READ);
  }

  getConfigForSuggestionType(type) {
    return this.#configsBySuggestionType.get(type);
  }

  ingestEnabledSuggestions(feature, { evenIfFresh = false } = {}) {
    let type = feature.rustSuggestionType;
    if (!type) {
      return;
    }

    if (!this.isEnabled || !feature.isEnabled) {
      this.#providerConstraintsOnLastIngestByFeature.delete(feature);
    } else {
      let providerConstraints = feature.rustProviderConstraints;
      if (
        evenIfFresh ||
        !this.#providerConstraintsOnLastIngestByFeature.has(feature) ||
        !lazy.ObjectUtils.deepEqual(
          providerConstraints,
          this.#providerConstraintsOnLastIngestByFeature.get(feature)
        )
      ) {
        this.#providerConstraintsOnLastIngestByFeature.set(
          feature,
          providerConstraints
        );
        this.#ingestSuggestionType({ type, providerConstraints });
      }
    }
  }

  async dismissRustSuggestion(suggestion) {
    let lowered = lowerSuggestion(suggestion);
    try {
      await this.#store?.dismissBySuggestion(lowered);
    } catch (error) {
      this.logger.error("Error: dismissRustSuggestion", { error, suggestion });
    }
  }

  async dismissByKey(dismissalKey) {
    try {
      await this.#store?.dismissByKey(dismissalKey);
    } catch (error) {
      this.logger.error("Error: dismissByKey", { error, dismissalKey });
    }
  }

  async isRustSuggestionDismissed(suggestion) {
    let lowered = lowerSuggestion(suggestion);
    try {
      return await this.#store?.isDismissedBySuggestion(lowered);
    } catch (error) {
      this.logger.error("Error: isDismissedBySuggestion", {
        error,
        suggestion,
      });
    }
    return false;
  }

  async isDismissedByKey(dismissalKey) {
    try {
      return await this.#store?.isDismissedByKey(dismissalKey);
    } catch (error) {
      this.logger.error("Error: isDismissedByKey", { error, dismissalKey });
    }
    return false;
  }

  async anyDismissedSuggestions() {
    try {
      return await this.#store?.anyDismissedSuggestions();
    } catch (error) {
      this.logger.error("Error: anyDismissedSuggestions", error);
    }
    return true;
  }

  async clearDismissedSuggestions() {
    try {
      await this.#store?.clearDismissedSuggestions();
    } catch (error) {
      this.logger.error("Error clearing dismissed suggestions", error);
    }
  }

  async fetchGeonames(searchString, matchNamePrefix, geonameType, filter) {
    if (!this.#store) {
      return [];
    }
    let geonames = await this.#store.fetchGeonames(
      searchString,
      matchNamePrefix,
      geonameType,
      filter
    );
    return geonames;
  }

  async fetchGeonameAlternates(geoname) {
    let alts = await this.#store?.fetchGeonameAlternates(geoname);
    return alts;
  }

  notify() {
    this.logger.info("Ingest timer fired");
    this.#ingestAll();
  }

  static mergeProviderConstraints(a, b) {
    if (!a || !b) {
      return a ?? b;
    }

    let merged = { ...a, ...b };

    if (
      a.hasOwnProperty("dynamicSuggestionTypes") ||
      b.hasOwnProperty("dynamicSuggestionTypes")
    ) {
      if (!a.dynamicSuggestionTypes || !b.dynamicSuggestionTypes) {
        merged.dynamicSuggestionTypes =
          a.dynamicSuggestionTypes ?? b.dynamicSuggestionTypes;
      } else {
        merged.dynamicSuggestionTypes = [
          ...new Set(
            a.dynamicSuggestionTypes.concat(b.dynamicSuggestionTypes).sort()
          ),
        ];
      }
    }

    return merged;
  }

  get #storeDataPath() {
    return PathUtils.join(
      Services.dirsvc.get("ProfD", Ci.nsIFile).path,
      SUGGEST_DATA_STORE_BASENAME
    );
  }

  get #enabledSuggestionTypes() {
    let items = [];
    for (let feature of lazy.QuickSuggest.rustFeatures) {
      if (feature.isEnabled) {
        let type = feature.rustSuggestionType;
        let provider = this.#providerFromSuggestionType(type);
        if (provider) {
          items.push({ feature, type, provider });
        }
      }
    }
    return items;
  }

  #init() {
    this.#store = this.#makeStore();
    if (!this.#store) {
      return;
    }

    let lastIngestSecs = Services.prefs.getIntPref(
      INGEST_TIMER_LAST_UPDATE_PREF,
      0
    );
    this.logger.debug("Last ingest time (seconds)", lastIngestSecs);

    this.#shutdownBlocker = () => {
      this.#store?.interrupt(lazy.InterruptKind.READ_WRITE);

      this.#store = null;
      this.#shutdownBlocker = null;
    };
    lazy.AsyncShutdown.profileChangeTeardown.addBlocker(
      "QuickSuggest: Interrupt the Rust component",
      this.#shutdownBlocker
    );

    lazy.timerManager.registerTimer(
      INGEST_TIMER_ID,
      this,
      lazy.UrlbarPrefs.get("quicksuggest.rustIngestIntervalSeconds"),
      true 
    );

    this.#ingestAll();

    this.#migrateBlockedDigests().then(() => {
      Services.obs.notifyObservers(null, "quicksuggest-dismissals-changed");
    });
  }

  #makeStore() {
    this.logger.info("Creating SuggestStore");
    if (!this.#remoteSettingsService) {
      return null;
    }

    let builder;
    try {
      builder = lazy.SuggestStoreBuilder.init()
        .dataPath(this.#storeDataPath)
        .remoteSettingsService(this.#remoteSettingsService)
        .loadExtension(
          AppConstants.SQLITE_LIBRARY_FILENAME,
          "sqlite3_fts5_init"
        );
    } catch (error) {
      this.logger.error("Error creating SuggestStoreBuilder", error);
      return null;
    }

    let store;
    try {
      store = builder.build();
    } catch (error) {
      this.logger.error("Error creating SuggestStore", error);
      return null;
    }

    return store;
  }

  #uninit() {
    this.#store = null;
    this.#providerConstraintsOnLastIngestByFeature.clear();
    this.#configsBySuggestionType.clear();
    lazy.timerManager.unregisterTimer(INGEST_TIMER_ID);

    lazy.AsyncShutdown.profileChangeTeardown.removeBlocker(
      this.#shutdownBlocker
    );
    this.#shutdownBlocker = null;
  }

  #ingestSuggestionType({ type, providerConstraints }) {
    this.#ingestQueue.queueIdleCallback(async () => {
      if (!this.#store) {
        return;
      }

      let provider = this.#providerFromSuggestionType(type);
      if (!provider) {
        return;
      }

      this.logger.debug("Starting ingest", { type });
      try {
        const metrics = await this.#store.ingest(
          new lazy.SuggestIngestionConstraints({
            providers: [provider],
            providerConstraints: providerConstraints
              ? new lazy.SuggestionProviderConstraints(providerConstraints)
              : null,
          })
        );
        for (let { label, value } of metrics.downloadTimes) {
        }
        for (let { label, value } of metrics.ingestionTimes) {
        }
      } catch (error) {
        this.logger.error("Ingest error", {
          type,
          error,
          reason: error.reason,
        });
      }
      this.logger.debug("Finished ingest", { type });

      if (!this.#store) {
        return;
      }

      this.logger.debug("Fetching provider config", { type });
      let config = await this.#store.fetchProviderConfig(provider);
      this.logger.debug("Got provider config", { type, config });
      this.#configsBySuggestionType.set(type, config);
      this.logger.debug("Finished fetching provider config", { type });
    });
  }

  #ingestAll() {
    for (let feature of lazy.QuickSuggest.rustFeatures) {
      this.ingestEnabledSuggestions(feature, { evenIfFresh: true });
    }

    this.#ingestQueue.queueIdleCallback(async () => {
      if (!this.#store) {
        return;
      }
      this.logger.debug("Fetching global config");
      this.#config = await this.#store.fetchGlobalConfig();
      this.logger.debug("Got global config", this.#config);
    });
  }

  #providerFromSuggestionType(type) {
    let key = type.toUpperCase();
    if (!lazy.SuggestionProvider.hasOwnProperty(key)) {
      this.logger.error("SuggestionProvider[key] not defined!", { key });
      return null;
    }
    return lazy.SuggestionProvider[key];
  }

  async #migrateBlockedDigests() {
    if (!this.#store) {
      return;
    }

    let pref = "browser.urlbar.quicksuggest.blockedDigests";
    this.logger.debug("Checking blockedDigests migration", { pref });

    let json;
    // eslint-disable-next-line mozilla/use-default-preference-values
    try {
      json = Services.prefs.getCharPref(pref);
    } catch (error) {
      if (error.result != Cr.NS_ERROR_UNEXPECTED) {
        throw error;
      }
      this.logger.debug(
        "blockedDigests pref does not exist, migration not necessary"
      );
      return;
    }

    await this.#migrateBlockedDigestsJson(json);

    Services.prefs.clearUserPref(pref);
  }

  async #migrateBlockedDigestsJson(json) {
    let digests;
    try {
      digests = JSON.parse(json);
    } catch (error) {
      this.logger.debug("blockedDigests is not valid JSON, discarding it");
      return;
    }

    if (!digests) {
      this.logger.debug("blockedDigests is falsey, discarding it");
      return;
    }

    if (!Array.isArray(digests)) {
      this.logger.debug("blockedDigests is not an array, discarding it");
      return;
    }

    let promises = [];
    for (let digest of digests) {
      if (typeof digest != "string") {
        continue;
      }
      promises.push(this.#store.dismissByKey(digest));
    }
    await Promise.all(promises);
  }

  get _test_store() {
    return this.#store;
  }

  get _test_enabledSuggestionTypes() {
    return this.#enabledSuggestionTypes;
  }

  async _test_setRemoteSettingsService(remoteSettingsService) {
    this.#remoteSettingsService = remoteSettingsService;
    if (this.isEnabled) {
      Services.prefs.clearUserPref(INGEST_TIMER_LAST_UPDATE_PREF);
      this.#uninit();
      this.#init();
      await this.ingestPromise;
    }
  }

  async _test_ingest() {
    this.#ingestAll();
    await this.ingestPromise;
  }

  #store;

  #config = {};

  #configsBySuggestionType = new Map();

  #providerConstraintsOnLastIngestByFeature = new Map();

  #ingestQueue;
  #shutdownBlocker;
  #remoteSettingsService;
}

function getSuggestionType(suggestion) {
  let type = gSuggestionTypesByCtor.get(suggestion.constructor);
  if (!type) {
    type = Object.keys(lazy.Suggestion).find(
      key => suggestion instanceof lazy.Suggestion[key]
    );
    if (type) {
      gSuggestionTypesByCtor.set(suggestion.constructor, type);
    } else {
      console.error(
        "Unexpected error: Suggestion class not found on `Suggestion`. " +
          "Did the Rust component or its JS bindings change? ",
        { suggestion }
      );
    }
  }
  return type;
}

function liftSuggestion(suggestion) {
  if (suggestion instanceof lazy.Suggestion.Dynamic) {
    let { data } = suggestion;
    if (typeof data == "string") {
      try {
        data = JSON.parse(data);
      } catch (error) {
        return null;
      }
    }
    return new lazy.Suggestion.Dynamic({
      suggestionType: suggestion.suggestionType,
      data,
      dismissalKey: suggestion.dismissalKey,
      score: suggestion.score,
    });
  }

  return suggestion;
}

function lowerSuggestion(suggestion) {
  if (suggestion.provider == "Dynamic") {
    let { data } = suggestion;
    if (data !== null && data !== undefined) {
      data = JSON.stringify(data);
    }
    return new lazy.Suggestion.Dynamic({
      suggestionType: suggestion.suggestionType,
      data,
      dismissalKey: suggestion.dismissalKey,
      score: suggestion.score,
    });
  }

  return suggestion;
}
