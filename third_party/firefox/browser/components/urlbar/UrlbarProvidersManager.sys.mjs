/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */



const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ObjectUtils: "resource://gre/modules/ObjectUtils.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  Region: "resource://gre/modules/Region.sys.mjs",
  SkippableTimer: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
  UrlbarMuxer: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarProvider: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
  UrlbarSearchUtils:
    "moz-src:///browser/components/urlbar/UrlbarSearchUtils.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  UrlbarTokenizer:
    "moz-src:///browser/components/urlbar/UrlbarTokenizer.sys.mjs",
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "logger", () =>
  lazy.UrlbarShared.getLogger({ prefix: "ProvidersManager" })
);

var localProviderModules = [
  {
    name: "UrlbarProviderAboutPages",
    module:
      "moz-src:///browser/components/urlbar/UrlbarProviderAboutPages.sys.mjs",
    supportedSAPs: ["urlbar"],
  },
  {
    name: "UrlbarProviderActionsSearchMode",
    module:
      "moz-src:///browser/components/urlbar/UrlbarProviderActionsSearchMode.sys.mjs",
    supportedSAPs: ["urlbar"],
  },
  {
    name: "UrlbarProviderAliasEngines",
    module:
      "moz-src:///browser/components/urlbar/UrlbarProviderAliasEngines.sys.mjs",
    supportedSAPs: ["searchbar", "urlbar"],
  },
  {
    name: "UrlbarProviderAutofill",
    module:
      "moz-src:///browser/components/urlbar/UrlbarProviderAutofill.sys.mjs",
    supportedSAPs: ["urlbar"],
  },
  {
    name: "UrlbarProviderBookmarkKeywords",
    module:
      "moz-src:///browser/components/urlbar/UrlbarProviderBookmarkKeywords.sys.mjs",
    supportedSAPs: ["urlbar"],
  },
  {
    name: "UrlbarProviderHeuristicFallback",
    module:
      "moz-src:///browser/components/urlbar/UrlbarProviderHeuristicFallback.sys.mjs",
    supportedSAPs: ["searchbar", "urlbar"],
  },
  {
    name: "UrlbarProviderHistoryUrlHeuristic",
    module:
      "moz-src:///browser/components/urlbar/UrlbarProviderHistoryUrlHeuristic.sys.mjs",
    supportedSAPs: ["urlbar"],
  },
  {
    name: "UrlbarProviderPlaces",
    module: "moz-src:///browser/components/urlbar/UrlbarProviderPlaces.sys.mjs",
    supportedSAPs: ["urlbar"],
  },
  {
    name: "UrlbarProviderPrivateSearch",
    module:
      "moz-src:///browser/components/urlbar/UrlbarProviderPrivateSearch.sys.mjs",
    supportedSAPs: ["searchbar", "urlbar"],
  },
  {
    name: "UrlbarProviderRestrictKeywords",
    module:
      "moz-src:///browser/components/urlbar/UrlbarProviderRestrictKeywords.sys.mjs",
    supportedSAPs: ["urlbar"],
  },
  {
    name: "UrlbarProviderRestrictKeywordsAutofill",
    module:
      "moz-src:///browser/components/urlbar/UrlbarProviderRestrictKeywordsAutofill.sys.mjs",
    supportedSAPs: ["urlbar"],
  },
  {
    name: "UrlbarProviderTokenAliasEngines",
    module:
      "moz-src:///browser/components/urlbar/UrlbarProviderTokenAliasEngines.sys.mjs",
    supportedSAPs: ["searchbar", "urlbar"],
  },
];

var localMuxerModules = {
  UrlbarMuxerStandard:
    "moz-src:///browser/components/urlbar/UrlbarMuxerStandard.sys.mjs",
};

var gProvidersManagerPerSap = new Map();

const DEFAULT_MUXER = "UnifiedComplete";
const DEFAULT_CHUNK_RESULTS_DELAY_MS = 16;

export class ProvidersManager {
  static interruptLevel = 0;

  constructor(sapName, muxerModules = localMuxerModules) {
    this.providers = [];

    this.providersByNotificationType = {
      onEngagement: new Set(),
      onImpression: new Set(),
      onAbandonment: new Set(),
      onSearchSessionEnd: new Set(),
    };

    for (let providerInfo of localProviderModules.filter(info =>
      info.supportedSAPs.includes(sapName)
    )) {
      let { [providerInfo.name]: providerClass } = ChromeUtils.importESModule(
        providerInfo.module
      );
      this.registerProvider(new providerClass());
    }

    this.queries = new Map();

    this.muxers = new Map();

    for (let [symbol, module] of Object.entries(muxerModules)) {
      let { [symbol]: muxer } = ChromeUtils.importESModule(module);
      this.registerMuxer(muxer);
    }
  }

  static chunkResultsDelayMs = DEFAULT_CHUNK_RESULTS_DELAY_MS;

  static getInstanceForSap(sapName) {
    let manager = gProvidersManagerPerSap.get(sapName);
    if (!manager) {
      manager = new ProvidersManager(sapName);
      gProvidersManagerPerSap.set(sapName, manager);
    }
    return manager;
  }

  registerProvider(provider) {
    if (!provider || !(provider instanceof lazy.UrlbarProvider)) {
      throw new Error(`Trying to register an invalid provider`);
    }
    if (
      !Object.values(lazy.UrlbarUtils.PROVIDER_TYPE).includes(provider.type)
    ) {
      throw new Error(`Unknown provider type ${provider.type}`);
    }
    lazy.logger.info(`Registering provider ${provider.name}`);
    let index = -1;
    if (provider.type == lazy.UrlbarUtils.PROVIDER_TYPE.HEURISTIC) {
      index = this.providers.findIndex(
        p => p.type != lazy.UrlbarUtils.PROVIDER_TYPE.HEURISTIC
      );
    }
    if (index < 0) {
      index = this.providers.length;
    }
    this.providers.splice(index, 0, provider);

    for (const notificationType of Object.keys(
      this.providersByNotificationType
    )) {
      if (typeof provider[notificationType] === "function") {
        this.providersByNotificationType[notificationType].add(provider);
      }
    }
  }

  unregisterProvider(provider) {
    lazy.logger.info(`Unregistering provider ${provider.name}`);
    let index = this.providers.findIndex(p => p.name == provider.name);
    if (index != -1) {
      this.providers.splice(index, 1);
    }

    Object.values(this.providersByNotificationType).forEach(providers =>
      providers.delete(provider)
    );
  }

  getProvider(name) {
    return this.providers.find(p => p.name == name);
  }

  registerMuxer(muxer) {
    if (!muxer || !(muxer instanceof lazy.UrlbarMuxer)) {
      throw new Error(`Trying to register an invalid muxer`);
    }
    lazy.logger.info(`Registering muxer ${muxer.name}`);
    this.muxers.set(muxer.name, muxer);
  }

  unregisterMuxer(muxer) {
    let muxerName = typeof muxer == "string" ? muxer : muxer.name;
    lazy.logger.info(`Unregistering muxer ${muxerName}`);
    this.muxers.delete(muxerName);
  }

  async startQuery(queryContext, controller = null) {
    lazy.logger.info(`Query start "${queryContext.searchString}"`);

    let muxerName = queryContext.muxer || DEFAULT_MUXER;
    lazy.logger.debug(`Using muxer ${muxerName}`);
    let muxer = this.muxers.get(muxerName);
    if (!muxer) {
      throw new Error(`Muxer with name ${muxerName} not found`);
    }

    let providers = queryContext.providers
      ? this.providers.filter(p => queryContext.providers.includes(p.name))
      : this.providers;

    queryContext.canceled = false;
    if (AppConstants.MOZ_PLACES) {
      try {
        await lazy.PlacesUtils.keywords.ensureCacheInitialized();
      } catch (ex) {
        lazy.logger.error(
          "Unable to ensure keyword cache is initialization. A keyword may not be \
           detected at the beginning of the search string.",
          ex
        );
      }
    }

    if (queryContext.canceled) {
      return;
    }

    let tokens = lazy.UrlbarTokenizer.tokenize(queryContext);
    queryContext.tokens = tokens;

    if (queryContext.sources && queryContext.sources.length == 1) {
      queryContext.restrictSource = queryContext.sources[0];
    }
    let restrictToken = updateSourcesIfEmpty(queryContext);
    if (restrictToken) {
      queryContext.restrictToken = restrictToken;
      if (lazy.UrlbarShared.SEARCH_MODE_RESTRICT.has(restrictToken.value)) {
        queryContext.restrictSource = queryContext.sources[0];
      }
    }
    lazy.logger.debug(`Context sources ${queryContext.sources}`);

    let query = new Query(queryContext, controller, muxer, providers);
    this.queries.set(queryContext, query);

    try {
      await lazy.UrlbarSearchUtils.init();
    } catch {
    }

    try {
      await lazy.Region.init();
    } catch (ex) {
    }

    if (query.canceled) {
      return;
    }

    await query.start();
  }

  cancelQuery(queryContext) {
    lazy.logger.info(`Query cancel "${queryContext.searchString}"`);
    queryContext.canceled = true;

    let query = this.queries.get(queryContext);
    if (!query) {
      return;
    }
    query.cancel();
    if (AppConstants.MOZ_PLACES && !ProvidersManager.interruptLevel) {
      try {
        let db = lazy.PlacesUtils.promiseLargeCacheDBConnection();
        db.interrupt();
      } catch (ex) {}
    }
    this.queries.delete(queryContext);
  }

  static async runInCriticalSection(taskFn) {
    this.interruptLevel++;
    try {
      await taskFn();
    } finally {
      this.interruptLevel--;
    }
  }

  notifyEngagementChange(state, queryContext, details = {}, controller) {
    if (!["engagement", "abandonment"].includes(state)) {
      lazy.logger.error(`Unsupported state for engagement change: ${state}`);
      return;
    }

    const visibleResults = controller.view?.visibleResults ?? [];
    const visibleResultsByProviderName = new Map();

    visibleResults.forEach((result, index) => {
      const providerName = result.providerName;
      let results = visibleResultsByProviderName.get(providerName);
      if (!results) {
        results = [];
        visibleResultsByProviderName.set(providerName, results);
      }
      results.push({ index, result });
    });

    if (!details.isSessionOngoing) {
      this.#notifyImpression(
        this.providersByNotificationType.onImpression,
        state,
        queryContext,
        controller,
        visibleResultsByProviderName,
        state == "engagement" && details.result ? details : null
      );
    }

    if (state === "engagement") {
      if (details.result) {
        this.#notifyEngagement(
          this.providersByNotificationType.onEngagement,
          queryContext,
          controller,
          details
        );
      }
    } else {
      this.#notifyAbandonment(
        this.providersByNotificationType.onAbandonment,
        queryContext,
        controller,
        visibleResultsByProviderName
      );
    }

    if (!details.isSessionOngoing) {
      this.#notifySearchSessionEnd(
        this.providersByNotificationType.onSearchSessionEnd,
        queryContext,
        controller,
        details
      );
    }
  }

  #notifyEngagement(engagementProviders, queryContext, controller, details) {
    for (const provider of engagementProviders) {
      if (details.result.providerName == provider.name) {
        provider.tryMethod("onEngagement", queryContext, controller, details);
        break;
      }
    }
  }

  #notifyImpression(
    impressionProviders,
    state,
    queryContext,
    controller,
    visibleResultsByProviderName,
    details
  ) {
    for (const provider of impressionProviders) {
      const providerVisibleResults =
        visibleResultsByProviderName.get(provider.name) ?? [];

      if (providerVisibleResults.length) {
        provider.tryMethod(
          "onImpression",
          state,
          queryContext,
          controller,
          providerVisibleResults,
          details
        );
      }
    }
  }

  #notifyAbandonment(
    abandomentProviders,
    queryContext,
    controller,
    visibleResultsByProviderName
  ) {
    for (const provider of abandomentProviders) {
      if (visibleResultsByProviderName.has(provider.name)) {
        provider.tryMethod("onAbandonment", queryContext, controller);
      }
    }
  }

  #notifySearchSessionEnd(
    searchSessionEndProviders,
    queryContext,
    controller,
    details
  ) {
    for (const provider of searchSessionEndProviders) {
      provider.tryMethod(
        "onSearchSessionEnd",
        queryContext,
        controller,
        details
      );
    }
  }
}

export class Query {
  constructor(queryContext, controller, muxer, providers) {
    this.context = queryContext;
    this.context.results = [];
    this.context.pendingHeuristicProviders.clear();
    this.context.deferUserSelectionProviders.clear();
    this.unsortedResults = [];
    this.muxer = muxer;
    this.controller = controller;
    this.providers = providers;
    this.started = false;
    this.canceled = false;

    this.acceptableSources = queryContext.sources.slice();
  }

  async start() {
    if (this.started) {
      throw new Error("This Query has been started already");
    }
    this.started = true;

    let activeProviders = [];
    let activePromises = [];
    let maxPriority = -1;
    for (let provider of this.providers) {
      provider.queryInstance = this;
      activePromises.push(
        provider
          .isActive(this.context, this.controller)
          .then(isActive => {
            if (isActive && !this.canceled) {
              let priority = provider.tryMethod("getPriority", this.context);
              if (priority >= maxPriority) {
                if (priority > maxPriority) {
                  activeProviders.length = 0;
                  maxPriority = priority;
                }
                activeProviders.push(provider);
                if (provider.deferUserSelection) {
                  this.context.deferUserSelectionProviders.add(provider.name);
                }
              }
            }
          })
          .catch(ex => lazy.logger.error(ex))
      );
    }

    await Promise.all(activePromises);

    if (this.canceled) {
      this.controller = null;
      return;
    }

    let startQuery = async provider => {
      provider.logger.debug(
        `Starting query for "${this.context.searchString}"`
      );
      let addedResult = false;
      await provider.tryMethod(
        "startQuery",
        this.context,
        (innerProvider, result) => {
          addedResult = true;
          this.add(innerProvider, result);
        },
        this.controller
      );
      if (!addedResult) {
        this.context.deferUserSelectionProviders.delete(provider.name);
      }
    };

    let queryPromises = [];
    for (let provider of activeProviders) {
      if (provider.type == lazy.UrlbarUtils.PROVIDER_TYPE.HEURISTIC) {
        this.context.pendingHeuristicProviders.add(provider.name);
        queryPromises.push(
          startQuery(provider).finally(() => {
            this.context.pendingHeuristicProviders.delete(provider.name);
          })
        );
        continue;
      }
      if (!this._sleepTimer) {
        this._sleepTimer = new lazy.SkippableTimer({
          name: "Query provider timer",
          time: lazy.UrlbarPrefs.get("delay"),
          logger: provider.logger,
        });
      }
      queryPromises.push(
        this._sleepTimer.promise.then(() =>
          this.canceled ? undefined : startQuery(provider)
        )
      );
    }

    lazy.logger.info(
      `Queried ${queryPromises.length} providers: ${activeProviders.map(
        p => p.name
      )}`
    );

    let cancelPromise = new Promise(resolve => {
      this._cancelQueries = resolve;
    });
    await Promise.race([Promise.all(queryPromises), cancelPromise]);

    if (!this.canceled) {
      await this._chunkTimer?.fire();
    }

    this.controller = null;
  }

  cancel() {
    if (this.canceled) {
      return;
    }
    this.canceled = true;
    this.context.deferUserSelectionProviders.clear();
    for (let provider of this.providers) {
      provider.logger.debug(
        `Canceling query for "${this.context.searchString}"`
      );
      provider.queryInstance = null;
      provider.tryMethod("cancelQuery", this.context);
    }
    this._chunkTimer?.cancel().catch(ex => lazy.logger.error(ex));
    this._sleepTimer?.fire().catch(ex => lazy.logger.error(ex));
    this._cancelQueries?.();
  }

  add(provider, result) {
    if (!(provider instanceof lazy.UrlbarProvider)) {
      throw new Error("Invalid provider passed to the add callback");
    }

    this.context.pendingHeuristicProviders.delete(provider.name);

    if (this.canceled) {
      return;
    }

    if (
      result.heuristic &&
      this.context.searchMode &&
      (!this.context.trimmedSearchString ||
        (!this.context.searchMode.engineName && !result.autofill))
    ) {
      return;
    }

    if (
      !this.acceptableSources.includes(result.source) &&
      !result.heuristic &&
      (result.type != lazy.UrlbarShared.RESULT_TYPE.SEARCH ||
        result.source != lazy.UrlbarShared.RESULT_SOURCE.HISTORY ||
        !this.acceptableSources.includes(
          lazy.UrlbarShared.RESULT_SOURCE.SEARCH
        )) &&
      !(
        result.source == lazy.UrlbarShared.RESULT_SOURCE.ACTIONS &&
        this.acceptableSources.includes(lazy.UrlbarShared.RESULT_SOURCE.TABS)
      )
    ) {
      return;
    }

    if (
      result.type != lazy.UrlbarShared.RESULT_TYPE.KEYWORD &&
      result.payload.url &&
      result.payload.url.startsWith("javascript:") &&
      !this.context.searchString.startsWith("javascript:") &&
      lazy.UrlbarPrefs.get("filter.javascript")
    ) {
      return;
    }

    result.providerName = provider.name;
    result.providerType = provider.type;

    if (result.type == lazy.UrlbarShared.RESULT_TYPE.DYNAMIC) {
      result.viewTemplate = provider.getViewTemplate(result);
    }
    let commands = provider.tryMethod(
      "getResultCommands",
      result,
      this.context.isPrivate
    );
    if (commands) {
      result.commands = commands;
    }

    this.unsortedResults.push(result);

    this._notifyResultsFromProvider(provider);
  }

  _notifyResultsFromProvider(provider) {
    if (!this._chunkTimer || this._chunkTimer.done) {
      this._chunkTimer = new lazy.SkippableTimer({
        name: "chunking",
        callback: () => this._notifyResults(),
        time: ProvidersManager.chunkResultsDelayMs,
        logger: provider.logger,
      });
    } else if (
      !this.context.pendingHeuristicProviders.size &&
      provider.type == lazy.UrlbarUtils.PROVIDER_TYPE.HEURISTIC
    ) {
      this._chunkTimer.fire().catch(ex => lazy.logger.error(ex));
    }

  }

  _notifyResults() {
    this.muxer.sort(this.context, this.unsortedResults);
    if (!this.context.results.length) {
      return;
    }

    this.context.firstResultChanged = !lazy.ObjectUtils.deepEqual(
      this.context.firstResult,
      this.context.results[0]
    );
    this.context.firstResult = this.context.results[0];

    if (this.controller) {
      this.controller.receiveResults(this.context);
    }
  }

  getProvider(name) {
    return this.providers.find(p => p.name == name);
  }
}

function updateSourcesIfEmpty(context) {
  if (context.sources && context.sources.length) {
    return undefined;
  }
  let acceptedSources = [];
  let restrictToken =
    context.sapName != "urlbar"
      ? undefined
      : context.tokens.find(t =>
          [
            lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_HISTORY,
            lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_BOOKMARK,
            lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_TAG,
            lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_OPENPAGE,
            lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_SEARCH,
            lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_TITLE,
            lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_URL,
            lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_ACTION,
          ].includes(t.type)
        );

  let restrictTokenType =
    restrictToken &&
    restrictToken.type != lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_TITLE &&
    restrictToken.type != lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_URL
      ? restrictToken.type
      : undefined;

  for (let source of Object.values(lazy.UrlbarShared.RESULT_SOURCE)) {
    switch (source) {
      case lazy.UrlbarShared.RESULT_SOURCE.BOOKMARKS:
        if (
          restrictTokenType ===
            lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_BOOKMARK ||
          restrictTokenType === lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_TAG ||
          (!restrictTokenType && lazy.UrlbarPrefs.get("suggest.bookmark"))
        ) {
          acceptedSources.push(source);
        }
        break;
      case lazy.UrlbarShared.RESULT_SOURCE.HISTORY:
        if (
          restrictTokenType === lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_HISTORY ||
          (!restrictTokenType && lazy.UrlbarPrefs.get("suggest.history"))
        ) {
          acceptedSources.push(source);
        }
        break;
      case lazy.UrlbarShared.RESULT_SOURCE.SEARCH:
        if (
          restrictTokenType === lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_SEARCH ||
          !restrictTokenType
        ) {
          acceptedSources.push(source);
        }
        break;
      case lazy.UrlbarShared.RESULT_SOURCE.TABS:
        if (
          restrictTokenType ===
            lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_OPENPAGE ||
          (!restrictTokenType && lazy.UrlbarPrefs.get("suggest.openpage"))
        ) {
          acceptedSources.push(source);
        }
        break;
      case lazy.UrlbarShared.RESULT_SOURCE.OTHER_NETWORK:
        if (!context.isPrivate && !restrictTokenType) {
          acceptedSources.push(source);
        }
        break;
      case lazy.UrlbarShared.RESULT_SOURCE.OTHER_LOCAL:
      case lazy.UrlbarShared.RESULT_SOURCE.ADDON:
      default:
        if (!restrictTokenType) {
          acceptedSources.push(source);
        }
        break;
    }
  }
  context.sources = acceptedSources;
  return restrictToken;
}
