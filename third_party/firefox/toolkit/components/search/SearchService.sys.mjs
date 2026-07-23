/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  AppProvidedConfigEngine:
    "moz-src:///toolkit/components/search/ConfigSearchEngine.sys.mjs",
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  ConfigSearchEngine:
    "moz-src:///toolkit/components/search/ConfigSearchEngine.sys.mjs",
  IgnoreLists: "resource://gre/modules/IgnoreLists.sys.mjs",
  loadAndParseOpenSearchEngine:
    "moz-src:///toolkit/components/search/OpenSearchLoader.sys.mjs",
  OpenSearchEngine:
    "moz-src:///toolkit/components/search/OpenSearchEngine.sys.mjs",
  Region: "resource://gre/modules/Region.sys.mjs",
  RemoteSettings: "resource://services-settings/remote-settings.sys.mjs",
  SearchEngine: "moz-src:///toolkit/components/search/SearchEngine.sys.mjs",
  SearchEngineInstallError:
    "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  SearchEngineSelector:
    "moz-src:///toolkit/components/search/SearchEngineSelector.sys.mjs",
  SearchSettings: "moz-src:///toolkit/components/search/SearchSettings.sys.mjs",
  SearchStaticData:
    "moz-src:///toolkit/components/search/SearchStaticData.sys.mjs",
  SearchUtils: "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  UserInstalledConfigEngine:
    "moz-src:///toolkit/components/search/ConfigSearchEngine.sys.mjs",
  UserSearchEngine:
    "moz-src:///toolkit/components/search/UserSearchEngine.sys.mjs",
  logConsole: () =>
    console.createInstance({
      prefix: "SearchService",
      maxLogLevel: lazy.SearchUtils.loggingEnabled ? "Debug" : "Warn",
    }),
  timerManager: {
    service: "@mozilla.org/updates/timer-manager;1",
    iid: Ci.nsIUpdateTimerManager,
  },
  idleService: {
    service: "@mozilla.org/widget/useridleservice;1",
    iid: Ci.nsIUserIdleService,
  },
  defaultOverrideAllowlist: () => {
    return new SearchDefaultOverrideAllowlistHandler();
  },
});


const TOPIC_LOCALES_CHANGE = "intl:app-locales-changed";
const QUIT_APPLICATION_TOPIC = "quit-application";

const OPENSEARCH_UPDATE_TIMER_TOPIC = "search-engine-update-timer";
const OPENSEARCH_UPDATE_TIMER_INTERVAL = 60 * 60 * 24;

const RECONFIG_IDLE_TIME_SEC = 5 * 60;

const ENGINES_SEEN_KEY = "contextual-engines-seen";

const DONT_SHOW_PROMPT = -1;

const ENGINES_SEEN_FOR_PROMPT = 1;

export const SearchService = new (class SearchService {
  constructor() {
    this._settings = new lazy.SearchSettings(this);
  }

  CHANGE_REASON = Object.freeze({
    UNKNOWN: "unknown",
    USER: "user",
    USER_PRIVATE_SPLIT: "user_private_split",
    USER_SEARCHBAR: "user_searchbar",
    USER_SEARCHBAR_CONTEXT: "user_searchbar_context",
    CONFIG: "config",
    LOCALE: "locale",
    REGION: "region",
    EXPERIMENT: "experiment",
    UITOUR: "uitour",
    ENGINE_UPDATE: "engine-update",
    USER_PRIVATE_PREF_ENABLED: "user_private_pref_enabled",
    ENGINE_IGNORE_LIST_UPDATED: "ignore-list",
    NO_EXISTING_DEFAULT_ENGINE: "no-existing-default",
  });

  get defaultEngine() {
    this.#ensureInitialized();
    return this._getEngineDefault(false);
  }

  get defaultPrivateEngine() {
    this.#ensureInitialized();
    return this._getEngineDefault(this.#separatePrivateDefault);
  }

  async getDefault() {
    await this.init();
    return this.defaultEngine;
  }

  async setDefault(engine, changeReason) {
    await this.init();
    this.#setEngineDefault(false, engine, changeReason);
  }

  async getDefaultPrivate() {
    await this.init();
    return this.defaultPrivateEngine;
  }

  async setDefaultPrivate(engine, changeReason) {
    await this.init();
    if (!this.#lazyPrefs.separatePrivateDefaultPrefValue) {
      Services.prefs.setBoolPref(
        lazy.SearchUtils.BROWSER_SEARCH_PREF + "separatePrivateDefault",
        true
      );
    }
    this.#setEngineDefault(this.#separatePrivateDefault, engine, changeReason);
  }

  get appDefaultEngine() {
    return this.#appDefaultEngine();
  }

  get appPrivateDefaultEngine() {
    return this.#appDefaultEngine(this.#separatePrivateDefault);
  }

  get isInitialized() {
    return (
      this.#initializationStatus == "success" ||
      this.#initializationStatus == "failed"
    );
  }

  get hasSuccessfullyInitialized() {
    return this.#initializationStatus == "success";
  }

  get promiseInitialized() {
    return this.#initDeferredPromise.promise;
  }

  getEngineByName(engineName) {
    this.#ensureInitialized();
    return this.#getEngineByName(engineName);
  }

  #getEngineByName(engineName) {
    for (let engine of this._engines.values()) {
      if (engine.name == engineName) {
        return engine;
      }
    }

    return null;
  }

  getEngineById(engineId) {
    this.#ensureInitialized();
    return this._engines.get(engineId) || null;
  }

  async getEngineByAlias(alias) {
    await this.init();
    alias = alias.toLocaleLowerCase();

    for (let engine of this._engines.values()) {
      for (let engineAlias of engine.aliases) {
        if (engineAlias.toLocaleLowerCase() == alias) {
          return engine;
        }
      }
    }
    return null;
  }

  async getEngines() {
    await this.init();
    lazy.logConsole.debug("getEngines: getting all engines");
    return this.#sortedEngines;
  }

  async getVisibleEngines() {
    await this.init();
    lazy.logConsole.debug("getVisibleEngines: getting all visible engines");
    return this.#sortedVisibleEngines;
  }

  async getAppProvidedEngines() {
    await this.init();

    return lazy.SearchUtils.sortEnginesByDefaults({
      engines: this.#sortedEngines.filter(
        e => e instanceof lazy.AppProvidedConfigEngine
      ),
      appDefaultEngine: this.appDefaultEngine,
      appPrivateDefaultEngine: this.appPrivateDefaultEngine,
    });
  }

  async findContextualSearchEngineByHost(host) {
    await this.init();
    let settings = await this._settings.get();
    let config =
      await this.#engineSelector.findContextualSearchEngineByHost(host);
    if (config) {
      return new lazy.UserInstalledConfigEngine({ config, settings });
    }
    return null;
  }

  async shouldShowInstallPrompt(engine) {
    let identifer = engine._loadPath;
    let seenEngines =
      this._settings.getMetaDataAttribute(ENGINES_SEEN_KEY) ?? {};

    if (!(identifer in seenEngines)) {
      seenEngines[identifer] = 1;
      this._settings.setMetaDataAttribute(ENGINES_SEEN_KEY, seenEngines);
      return false;
    }

    let value = seenEngines[identifer];
    if (value == DONT_SHOW_PROMPT) {
      return false;
    }

    if (value == ENGINES_SEEN_FOR_PROMPT) {
      seenEngines[identifer] = DONT_SHOW_PROMPT;
      this._settings.setMetaDataAttribute(ENGINES_SEEN_KEY, seenEngines);
      return true;
    }

    console.error(`Unexpected value ${value} in seenEngines`);
    return false;
  }

  async init() {
    if (["started", "success", "failed"].includes(this.#initializationStatus)) {
      return this.promiseInitialized;
    }
    this.#initializationStatus = "started";
    return this.#init();
  }

  async runBackgroundChecks() {
    await this.init();
  }

  reset() {
    lazy.logConsole.debug("Resetting search service.");
    if (this.#earlyObserversAdded) {
      Services.obs.removeObserver(this, lazy.Region.REGION_TOPIC);
      this.#earlyObserversAdded = false;
    }
    this.#initializationStatus = "not initialized";
    this.#initDeferredPromise = Promise.withResolvers();
    this._engines.clear();
    this._cachedSortedEngines = null;
    this.#currentEngine = null;
    this.#currentPrivateEngine = null;
    this.#searchDefault = null;
    this.#searchPrivateDefault = null;
    this.#maybeReloadDebounce = false;
    this._settings._batchTask?.disarm();
    if (this.#engineSelector) {
      this.#engineSelector.reset();
      this.#engineSelector = null;
    }
  }

  resetToAppDefaultEngine() {
    let appDefaultEngine = this.appDefaultEngine;
    appDefaultEngine.hidden = false;
    this.#setEngineDefault(false, appDefaultEngine, this.CHANGE_REASON.USER);

    let appPrivateDefaultEngine = this.appPrivateDefaultEngine;
    appPrivateDefaultEngine.hidden = false;
    this.#setEngineDefault(
      true,
      appPrivateDefaultEngine,
      this.CHANGE_REASON.USER
    );
  }

  async addUserEngine(formInfo) {
    await this.init();

    let newEngine = new lazy.UserSearchEngine({ formInfo });
    lazy.logConsole.debug(`Adding ${formInfo.name}`);
    this.#addEngineToStore(newEngine);
    return newEngine;
  }

  async addSearchEngine(engine) {
    await this.init();
    this.#addEngineToStore(engine);
  }

  async addOpenSearchEngine(engineURL, iconURL, originAttributes) {
    lazy.logConsole.debug("addOpenSearchEngine: Adding", engineURL);
    await this.init();
    let engineData = await lazy.loadAndParseOpenSearchEngine(
      Services.io.newURI(engineURL),
      null,
      originAttributes
    );
    let engine = new lazy.OpenSearchEngine({
      engineData,
      faviconURL: iconURL,
      originAttributes,
    });
    this.#addEngineToStore(engine);
    this.#maybeStartOpenSearchUpdateTimer();
    return engine;
  }

  async removeEngine(engine, changeReason) {
    await this.init();
    if (!engine) {
      throw new TypeError("no engine passed");
    }

    var engineToRemove = null;
    for (var e of this._engines.values()) {
      if (engine == e) {
        engineToRemove = e;
      }
    }

    if (!engineToRemove) {
      throw new Error("Unable to find engine to remove");
    }

    this.#enginesPendingRemoval.add(engineToRemove);

    if (engineToRemove == this.defaultEngine) {
      this.#findAndSetNewDefaultEngine(
        {
          privateMode: false,
        },
        changeReason
      );
    }

    if (
      this.#separatePrivateDefault &&
      engineToRemove == this.defaultPrivateEngine
    ) {
      this.#findAndSetNewDefaultEngine(
        {
          privateMode: true,
        },
        changeReason
      );
    }

    let userInstalled =
      engineToRemove instanceof lazy.UserInstalledConfigEngine;
    if (engineToRemove.inMemory && !userInstalled) {
      engineToRemove.hidden = true;
      engineToRemove.alias = null;
      this.#enginesPendingRemoval.delete(engineToRemove);
    } else {
      if (engineToRemove._filePath) {
        let file = Cc["@mozilla.org/file/local;1"].createInstance(Ci.nsIFile);
        file.persistentDescriptor = engineToRemove._filePath;
        if (file.exists()) {
          file.remove(false);
        }
        engineToRemove._filePath = null;
      }

      if (userInstalled) {
        let seenEngines =
          this._settings.getMetaDataAttribute(ENGINES_SEEN_KEY) ?? {};
        delete seenEngines[engineToRemove._loadPath];
        this._settings.setMetaDataAttribute(ENGINES_SEEN_KEY, seenEngines);
      }

      this.#internalRemoveEngine(engineToRemove);
      if (!this.#dontSetUseSavedOrder) {
        this.#saveSortedEngineList();
      }
    }
    lazy.SearchUtils.notifyAction(
      engineToRemove,
      lazy.SearchUtils.MODIFIED_TYPE.REMOVED
    );
  }

  async moveEngine(engine, newIndex, skipHidden = false) {
    await this.init();
    if (newIndex >= this.#sortedEngines.length || newIndex < 0) {
      throw new RangeError("newIndex out of bounds");
    }
    if (!(engine instanceof lazy.SearchEngine)) {
      throw new TypeError("engine is not a SearchEngine instance");
    }
    if (skipHidden && engine.hidden) {
      throw new Error("Unable to move a hidden engine");
    }

    var currentIndex = this.#sortedEngines.indexOf(engine);
    if (currentIndex == -1) {
      throw new Error("Unable to find engine to move");
    }

    if (skipHidden) {
      let filteredEngines = this.#sortedEngines.filter(e => !e.hidden);
      var newIndexEngine = filteredEngines[newIndex];
      if (!newIndexEngine) {
        throw new Error("Unable to find engine to replace");
      }

      for (var i = 0; i < this.#sortedEngines.length; ++i) {
        if (newIndexEngine == this.#sortedEngines[i]) {
          break;
        }
        if (this.#sortedEngines[i].hidden) {
          newIndex++;
        }
      }
    }

    if (currentIndex == newIndex) {
      return;
    } 

    var movedEngine = this._cachedSortedEngines.splice(currentIndex, 1)[0];
    this._cachedSortedEngines.splice(newIndex, 0, movedEngine);

    lazy.SearchUtils.notifyAction(
      engine,
      lazy.SearchUtils.MODIFIED_TYPE.CHANGED
    );

    this.#saveSortedEngineList();
  }

  restoreDefaultEngines() {
    this.#ensureInitialized();
    for (let e of this._engines.values()) {
      if (e.hidden && e instanceof lazy.AppProvidedConfigEngine) {
        e.hidden = false;
      }
    }
  }


  parseSubmissionURL(url) {
    if (!this.hasSuccessfullyInitialized) {
      return { engine: null, terms: "", termsParameterName: "" };
    }

    if (!this.#parseSubmissionMap) {
      this.#buildParseSubmissionMap();
    }

    let soughtKey, soughtQuery;
    try {
      let soughtUrl = Services.io.newURI(url);

      if (!soughtUrl.schemeIs("http") && !soughtUrl.schemeIs("https")) {
        return { engine: null, terms: "", termsParameterName: "" };
      }

      soughtKey = soughtUrl.host + soughtUrl.filePath.toLowerCase();
      soughtQuery = soughtUrl.query;
    } catch (ex) {
      return { engine: null, terms: "", termsParameterName: "" };
    }

    let mapEntry = this.#parseSubmissionMap.get(soughtKey);
    if (!mapEntry) {
      return { engine: null, terms: "", termsParameterName: "" };
    }

    let encodedTerms = null;
    for (let param of soughtQuery.split("&")) {
      let equalPos = param.indexOf("=");
      if (
        equalPos != -1 &&
        param.substr(0, equalPos) == mapEntry.termsParameterName
      ) {
        encodedTerms = param.substr(equalPos + 1);
        break;
      }
    }
    if (encodedTerms === null) {
      return { engine: null, terms: "", termsParameterName: "" };
    }

    let terms;
    try {
      terms = Services.textToSubURI.UnEscapeAndConvert(
        mapEntry.engine.queryCharset,
        encodedTerms.replace(/\+/g, " ")
      );
    } catch (ex) {
      return { engine: null, terms: "", termsParameterName: "" };
    }

    return {
      engine: mapEntry.engine,
      terms,
      termsParameterName: mapEntry.termsParameterName,
    };
  }

  get separatePrivateDefaultUrlbarResultEnabled() {
    return this.#lazyPrefs.separatePrivateDefaultUrlbarResultEnabled;
  }

  async notify() {
    lazy.logConsole.debug("notify: checking for updates");

    for (let engine of this._engines.values()) {
      if (!(engine instanceof lazy.OpenSearchEngine)) {
        continue;
      }
      await engine.maybeUpdate();
    }
  }

  #currentEngine = null;
  #currentPrivateEngine = null;
  #queuedIdle = false;

  #initDeferredPromise = Promise.withResolvers();

  #initializationStatus = "not initialized";

  #maybeReloadDebounce = false;

  _reloadingEngines = false;

  #engineSelector = null;

  #submissionURLIgnoreList = [];

  #loadPathIgnoreList = [];

  _engines = new Map();

  _cachedSortedEngines = null;

  #dontSetUseSavedOrder = false;

  #searchDefault = null;

  #searchPrivateDefault = null;


  #parseSubmissionMap = null;

  #earlyObserversAdded = false;

  #observersAdded = false;

  #openSearchUpdateTimerStarted = false;

  #enginesPendingRemoval = new WeakSet();

  get #sortedEngines() {
    if (!this._cachedSortedEngines) {
      return this.#buildSortedEngineList();
    }
    return this._cachedSortedEngines;
  }
  get #separatePrivateDefault() {
    return (
      this.#lazyPrefs.separatePrivateDefaultPrefValue &&
      this.#lazyPrefs.separatePrivateDefaultEnabledPrefValue
    );
  }

  _getEngineDefault(privateMode) {
    let currentEngine = privateMode
      ? this.#currentPrivateEngine
      : this.#currentEngine;

    if (currentEngine && !currentEngine.hidden) {
      return currentEngine;
    }

    const attributeName = privateMode
      ? "privateDefaultEngineId"
      : "defaultEngineId";

    let engineId = this._settings.getMetaDataAttribute(attributeName);
    let engine = this._engines.get(engineId) || null;
    if (
      engine &&
      this._settings.getVerifiedMetaDataAttribute(
        attributeName,
        engine instanceof lazy.ConfigSearchEngine
      )
    ) {
      if (privateMode) {
        this.#currentPrivateEngine = engine;
      } else {
        this.#currentEngine = engine;
      }
    }
    if (!engineId) {
      if (privateMode) {
        this.#currentPrivateEngine = this.appPrivateDefaultEngine;
      } else {
        this.#currentEngine = this.appDefaultEngine;
      }
    }

    currentEngine = privateMode
      ? this.#currentPrivateEngine
      : this.#currentEngine;
    if (currentEngine && !currentEngine.hidden) {
      return currentEngine;
    }
    return this.#findAndSetNewDefaultEngine(
      { privateMode },
      this.CHANGE_REASON.NO_EXISTING_DEFAULT_ENGINE
    );
  }

  #ensureInitialized() {
    if (this.#initializationStatus === "success") {
      return;
    }

    if (this.#initializationStatus === "failed") {
      throw new Error("SearchService failed while it was initializing.");
    }

    let err = new Error(
      "Something tried to use the search service before it finished " +
        "initializing. Please examine the stack trace to figure out what and " +
        "where to fix it:\n"
    );
    err.message += err.stack;
    throw err;
  }

  #lazyPrefs = XPCOMUtils.declareLazy({
    separatePrivateDefaultPrefValue: {
      pref: "browser.search.separatePrivateDefault",
      default: false,
      onUpdate: this.#onSeparateDefaultPrefChanged.bind(this),
    },
    separatePrivateDefaultEnabledPrefValue: {
      pref: "browser.search.separatePrivateDefault.ui.enabled",
      default: false,
      onUpdate: this.#onSeparateDefaultPrefChanged.bind(this),
    },
    separatePrivateDefaultUrlbarResultEnabled: {
      pref: "browser.search.separatePrivateDefault.urlbarResult.enabled",
      default: false,
    },
    experimentPrefValue: {
      pref: "browser.search.experiment",
      default: "",
      onUpdate: () => this.#maybeReloadEngines(this.CHANGE_REASON.EXPERIMENT),
    },
  });

  #doPreInitWork() {
    if (!this.#earlyObserversAdded) {
      Services.obs.addObserver(this, lazy.Region.REGION_TOPIC);
      this.#earlyObserversAdded = true;
    }

    this.#getIgnoreListAndSubscribe().catch(ex =>
      console.error(ex, "Search Service could not get the ignore list.")
    );

    this.#engineSelector = new lazy.SearchEngineSelector(
      this.#handleConfigurationUpdated.bind(this)
    );
  }

  async #init() {
    lazy.logConsole.debug("init");

    this.#doPreInitWork();

    try {
      const settings = await this._settings.get();

      const refinedConfig = await this._fetchEngineSelectorEngines();

      await this.#loadEngines(settings, refinedConfig);
    } catch (ex) {
      lazy.logConsole.error("#init: failure initializing search:", ex);
      this.#initializationStatus = "failed";
      this.#initDeferredPromise.reject(ex);

      throw ex;
    }

    if (Services.startup.shuttingDown) {

      let ex = new Error("Abandoning search service init due to shutting down");
      this.#initializationStatus = "failed";
      this.#initDeferredPromise.reject(ex);
      throw ex;
    }

    this.#initializationStatus = "success";
    if (this._settings.lastGetCorrupt) {
      this._showSearchSettingsResetNotificationBox(this.defaultEngine.name);
    }
    this.#initDeferredPromise.resolve();
    this.#addObservers();

    Services.obs.notifyObservers(
      null,
      lazy.SearchUtils.TOPIC_SEARCH_SERVICE,
      "init-complete"
    );

    lazy.logConsole.debug("Completed #init");
    this.#doPostInitWork();
  }

  #doPostInitWork() {
    this.#maybeStartOpenSearchUpdateTimer();
  }

  async #getIgnoreListAndSubscribe() {
    let listener = this.#handleIgnoreListUpdated.bind(this);
    const current = await lazy.IgnoreLists.getAndSubscribe(listener);

    this.ignoreListListener = listener;

    await this.#handleIgnoreListUpdated({ data: { current } });
    Services.obs.notifyObservers(
      null,
      lazy.SearchUtils.TOPIC_SEARCH_SERVICE,
      "settings-update-complete"
    );
  }

  async #handleIgnoreListUpdated(eventData) {
    lazy.logConsole.debug("#handleIgnoreListUpdated");
    const {
      data: { current },
    } = eventData;

    for (const entry of current) {
      if (entry.id == "load-paths") {
        this.#loadPathIgnoreList = [...entry.matches];
      } else if (entry.id == "submission-urls") {
        this.#submissionURLIgnoreList = [...entry.matches];
      }
    }

    try {
      await this.promiseInitialized;
    } catch (ex) {
      return;
    }

    let engineRemoved = false;
    for (let engine of this._engines.values()) {
      if (this.#engineMatchesIgnoreLists(engine)) {
        await this.removeEngine(
          engine,
          this.CHANGE_REASON.ENGINE_IGNORE_LIST_UPDATED
        );
        engineRemoved = true;
      }
    }
    if (engineRemoved && !this._engines.size) {
      this.#maybeReloadEngines(
        this.CHANGE_REASON.ENGINE_IGNORE_LIST_UPDATED
      ).catch(console.error);
    }
  }

  #engineMatchesIgnoreLists(engine) {
    let logIgnored = (name, url, type) => {
      lazy.logConsole.warn("Search engine", name, `matches ${type}`, url);
      Services.prefs.setCharPref(
        lazy.SearchUtils.BROWSER_SEARCH_PREF + "lastEngineIgnored",
        `${Math.trunc(Date.now() / 1000)} Search engine '${name}' matches ${type} ignore list ${url.substring(0, 200)}`
      );
    };

    if (this.#loadPathIgnoreList.includes(engine._loadPath)) {
      logIgnored(engine.name, engine._loadPath, "load path");
      return true;
    }
    let url = engine.searchURLWithNoTerms.spec.toLowerCase();
    if (
      this.#submissionURLIgnoreList.some(code =>
        url.includes(code.toLowerCase())
      )
    ) {
      logIgnored(engine.name, url, "submission url");
      return true;
    }
    return false;
  }

  #handleConfigurationUpdated() {
    if (this.#queuedIdle) {
      return;
    }

    this.#queuedIdle = true;

    lazy.idleService.addIdleObserver(this, RECONFIG_IDLE_TIME_SEC);
  }

  #appDefaultEngine(privateMode = false) {
    let defaultEngine = this._engines.get(
      privateMode && this.#searchPrivateDefault
        ? this.#searchPrivateDefault
        : this.#searchDefault
    );

    if (defaultEngine) {
      return defaultEngine;
    }

    if (privateMode) {
      return this.#appDefaultEngine(false);
    }

    defaultEngine = this.#sortedVisibleEngines.find(
      e => e.isGeneralPurposeEngine
    );
    return defaultEngine ? defaultEngine : this.#sortedVisibleEngines[0];
  }

  async #loadEngines(settings, refinedConfig) {
    let prevMetaData = { ...settings?.metaData };
    let prevCurrentEngineId = prevMetaData.defaultEngineId;
    let prevAppDefaultEngineId = prevMetaData?.appDefaultEngineId;

    lazy.logConsole.debug("#loadEngines: start");
    this.#setDefaultFromSelector(refinedConfig);

    this.#loadEnginesFromConfig(refinedConfig.engines, settings);

    let skipDefaultChangedNotification = await this.#loadEnginesFromSettings(
      settings,
      refinedConfig.engines
    );

    skipDefaultChangedNotification ||=
      await this.#checkOpenSearchOverrides(settings);

    this._settings.migrateEngineIds(settings);

    lazy.logConsole.debug("#loadEngines: done");

    let newCurrentEngine = this._getEngineDefault(false);
    let newCurrentEngineId = newCurrentEngine?.id;

    this._settings.setMetaDataAttribute(
      "appDefaultEngineId",
      this.appDefaultEngine?.id
    );

    if (
      !skipDefaultChangedNotification &&
      this.#shouldDisplayRemovalOfEngineNotificationBox(
        settings,
        prevMetaData,
        newCurrentEngineId,
        prevCurrentEngineId,
        prevAppDefaultEngineId
      )
    ) {
      let newCurrentEngineName = newCurrentEngine?.name;

      let [prevCurrentEngineName, prevAppDefaultEngineName] = [
        settings.engines.find(e => e.id == prevCurrentEngineId)?._name,
        settings.engines.find(e => e.id == prevAppDefaultEngineId)?._name,
      ];

      this._showRemovalOfSearchEngineNotificationBox(
        prevCurrentEngineName || prevAppDefaultEngineName,
        newCurrentEngineName
      );
    }
  }

  #shouldDisplayRemovalOfEngineNotificationBox(
    settings,
    prevMetaData,
    newCurrentEngineId,
    prevCurrentEngineId,
    prevAppDefaultEngineId
  ) {
    if (
      !Services.prefs.getBoolPref("browser.search.removeEngineInfobar.enabled")
    ) {
      return false;
    }

    if (!newCurrentEngineId) {
      return false;
    }

    if (prevCurrentEngineId && this._engines.has(prevCurrentEngineId)) {
      return false;
    }
    if (!prevCurrentEngineId && this._engines.has(prevAppDefaultEngineId)) {
      return false;
    }

    if (
      (prevCurrentEngineId && prevCurrentEngineId !== newCurrentEngineId) ||
      (!prevCurrentEngineId &&
        prevAppDefaultEngineId &&
        prevAppDefaultEngineId !== newCurrentEngineId)
    ) {
      if (!this.#didSettingsMetaDataUpdate(prevMetaData)) {
        return true;
      }
    }

    return false;
  }

  #loadEnginesFromConfig(engineConfigs, settings) {
    lazy.logConsole.debug("#loadEnginesFromConfig");
    for (let config of engineConfigs) {
      try {
        let engine = new lazy.AppProvidedConfigEngine({ config, settings });
        this.#addEngineToStore(engine);
      } catch (ex) {
        console.error(
          "Could not load app provided search engine id:",
          config.identifier,
          ex
        );
      }
    }
  }

  async #checkOpenSearchOverrides(settings) {
    let defaultEngineChanged = false;
    let savedDefaultEngineId =
      settings.metaData.defaultEngineId || settings.metaData.appDefaultEngineId;
    if (!savedDefaultEngineId) {
      return false;
    }
    for (let engineSettings of settings.engines) {
      if (
        !this._engines.get(engineSettings.id) &&
        engineSettings._isConfigEngine &&
        engineSettings.id == savedDefaultEngineId &&
        engineSettings._metaData.overriddenByOpenSearch
      ) {
        let restoringEngine = new lazy.OpenSearchEngine({
          json: engineSettings._metaData.overriddenByOpenSearch,
        });
        restoringEngine.copyUserSettingsFrom(engineSettings);
        this.#addEngineToStore(restoringEngine, true);

        this.#setEngineDefault(
          false,
          restoringEngine,
          this.CHANGE_REASON.CONFIG
        );
        delete engineSettings._metaData.overriddenByOpenSearch;
      }
    }
    for (let engine of this._engines.values()) {
      if (
        engine instanceof lazy.ConfigSearchEngine &&
        engine.getAttr("overriddenByOpenSearch") &&
        engine.id == savedDefaultEngineId
      ) {
        let restoringEngine = new lazy.OpenSearchEngine({
          json: engine.getAttr("overriddenByOpenSearch"),
        });
        if (
          await lazy.defaultOverrideAllowlist.canEngineOverride(
            restoringEngine,
            engine.id
          )
        ) {
          engine.overrideWithEngine({ engine: restoringEngine });
        }
      }
    }

    return defaultEngineChanged;
  }

  async #maybeReloadEngines(changeReason) {
    if (this.#maybeReloadDebounce) {
      lazy.logConsole.debug("We're already waiting to reload engines.");
      return;
    }

    if (!this.isInitialized || this._reloadingEngines) {
      this.#maybeReloadDebounce = true;
      Services.tm.idleDispatchToMainThread(() => {
        if (!this.#maybeReloadDebounce) {
          return;
        }
        this.#maybeReloadDebounce = false;
        this.#maybeReloadEngines(changeReason).catch(console.error);
      }, 10000);
      lazy.logConsole.debug(
        "Post-poning maybeReloadEngines() as we're currently initializing."
      );
      return;
    }

    let settings = await this._settings.get();

    lazy.logConsole.debug("Running maybeReloadEngines");
    this._reloadingEngines = true;

    try {
      await this._reloadEngines(settings, changeReason);
    } catch (ex) {
      lazy.logConsole.error("maybeReloadEngines failed", ex);
    }
    this._reloadingEngines = false;
    lazy.logConsole.debug("maybeReloadEngines complete");
  }

  async _reloadEngines(settings, changeReason) {
    let prevCurrentEngine = this.#currentEngine;
    let prevPrivateEngine = this.#currentPrivateEngine;
    let prevMetaData = { ...settings?.metaData };

    this.#dontSetUseSavedOrder = true;

    let refinedConfig = await this._fetchEngineSelectorEngines();

    let availableConfigEngines = [...refinedConfig.engines];
    let oldEngineList = [...this._engines.values()];

    for (let engine of oldEngineList) {
      if (!(engine instanceof lazy.ConfigSearchEngine)) {
        continue;
      }

      let index = availableConfigEngines.findIndex(
        e => e.identifier == engine.id
      );
      let configuration = availableConfigEngines[index];

      if (!configuration && engine.getAttr("user-installed")) {
        configuration =
          await this.#engineSelector.findContextualSearchEngineById(engine.id);
      }

      if (!configuration) {
        this.#enginesPendingRemoval.add(engine);
        continue;
      } else {

        let willBeAppProvided = index != -1;
        if (
          willBeAppProvided &&
          engine instanceof lazy.UserInstalledConfigEngine
        ) {
          engine.upgrade();
        } else if (
          !willBeAppProvided &&
          engine instanceof lazy.AppProvidedConfigEngine
        ) {
          engine.downgrade();
        }

        engine.update({ configuration });
      }

      availableConfigEngines.splice(index, 1);
    }

    let existingDuplicateEngines = [];

    for (let engine of availableConfigEngines) {
      try {
        let newAppEngine = new lazy.AppProvidedConfigEngine({
          config: engine,
          settings,
        });

        let duplicateEngine = this.#getEngineByName(newAppEngine.name);
        if (duplicateEngine) {
          existingDuplicateEngines.push({
            duplicateEngine,
            newAppEngine,
          });
        }
        this.#addEngineToStore(newAppEngine, true);
      } catch (ex) {
        lazy.logConsole.warn(
          "Could not load app provided search engine id:",
          engine.identifier,
          ex
        );
      }
    }


    this.#currentEngine = null;
    this.#currentPrivateEngine = null;

    if (this.#enginesPendingRemoval.has(prevCurrentEngine)) {
      this._settings.setMetaDataAttribute("defaultEngineId", "");
    }
    if (this.#enginesPendingRemoval.has(prevPrivateEngine)) {
      this._settings.setMetaDataAttribute("privateDefaultEngineId", "");
    }

    this.#setDefaultFromSelector(refinedConfig);

    let skipDefaultChangedNotification = false;

    for (let { duplicateEngine, newAppEngine } of existingDuplicateEngines) {
      if (prevCurrentEngine && prevCurrentEngine == duplicateEngine) {
        if (
          duplicateEngine instanceof lazy.OpenSearchEngine &&
          (await lazy.defaultOverrideAllowlist.canEngineOverride(
            duplicateEngine,
            newAppEngine?.id
          ))
        ) {
          lazy.logConsole.log(
            "Applying override from",
            duplicateEngine.id,
            "to application engine",
            newAppEngine.id,
            "and setting app engine default"
          );
          newAppEngine.overrideWithEngine({
            engine: duplicateEngine,
          });

          this.#setEngineDefault(
            false,
            newAppEngine,
            this.CHANGE_REASON.CONFIG
          );
          skipDefaultChangedNotification = true;
        }
      }
      this.#enginesPendingRemoval.add(duplicateEngine);
    }

    if (this.#enginesPendingRemoval.has(prevCurrentEngine)) {
      skipDefaultChangedNotification ||=
        await this.#maybeRestoreEngineFromOverride(prevCurrentEngine);
    }

    if (prevCurrentEngine && this.defaultEngine !== prevCurrentEngine) {
      lazy.SearchUtils.notifyAction(
        this.#currentEngine,
        lazy.SearchUtils.MODIFIED_TYPE.DEFAULT
      );
      if (!this.#separatePrivateDefault) {
        lazy.SearchUtils.notifyAction(
          this.#currentEngine,
          lazy.SearchUtils.MODIFIED_TYPE.DEFAULT_PRIVATE
        );
      }

      if (
        !skipDefaultChangedNotification &&
        prevMetaData &&
        settings.metaData &&
        !this.#didSettingsMetaDataUpdate(prevMetaData) &&
        this.#enginesPendingRemoval.has(prevCurrentEngine) &&
        Services.prefs.getBoolPref("browser.search.removeEngineInfobar.enabled")
      ) {
        this._showRemovalOfSearchEngineNotificationBox(
          prevCurrentEngine.name,
          this.defaultEngine.name
        );
      }
    }

    if (
      this.#separatePrivateDefault &&
      prevPrivateEngine &&
      this.defaultPrivateEngine !== prevPrivateEngine
    ) {
      lazy.SearchUtils.notifyAction(
        this.#currentPrivateEngine,
        lazy.SearchUtils.MODIFIED_TYPE.DEFAULT_PRIVATE
      );
    }

    await this.#maybeRemoveEnginesAfterReload(this._engines);

    this._settings.setMetaDataAttribute(
      "appDefaultEngineId",
      this.appDefaultEngine?.id
    );

    if (
      prevMetaData.experiment &&
      !this._settings.getMetaDataAttribute("experiment")
    ) {
      if (this.defaultEngine == this.appDefaultEngine) {
        this._settings.setVerifiedMetaDataAttribute("defaultEngineId", "");
      }
      if (
        this.#separatePrivateDefault &&
        this.defaultPrivateEngine == this.appPrivateDefaultEngine
      ) {
        this._settings.setVerifiedMetaDataAttribute(
          "privateDefaultEngineId",
          ""
        );
      }
    }

    this.#dontSetUseSavedOrder = false;
    this._cachedSortedEngines = null;
    Services.obs.notifyObservers(
      null,
      lazy.SearchUtils.TOPIC_SEARCH_SERVICE,
      "engines-reloaded"
    );
  }

  async #maybeRestoreEngineFromOverride(prevCurrentEngine) {
    let overriddenByOpenSearch = prevCurrentEngine.getAttr(
      "overriddenByOpenSearch"
    );
    if (!overriddenByOpenSearch) {
      return false;
    }
    let engine = new lazy.OpenSearchEngine({
      json: overriddenByOpenSearch,
    });
    engine.copyUserSettingsFrom(prevCurrentEngine);
    this.#addEngineToStore(engine, true);

    this.#setEngineDefault(false, engine, this.CHANGE_REASON.CONFIG);
    return true;
  }

  async #maybeRemoveEnginesAfterReload(engines) {
    for (let engine of engines.values()) {
      if (!this.#enginesPendingRemoval.has(engine)) {
        continue;
      }

      this.#internalRemoveEngine(engine);

      if (engine instanceof lazy.ConfigSearchEngine) {
        await engine.cleanup();
      }

      lazy.SearchUtils.notifyAction(
        engine,
        lazy.SearchUtils.MODIFIED_TYPE.REMOVED
      );
    }
  }

  #addEngineToStore(engine, skipDuplicateCheck = false) {
    if (this.#engineMatchesIgnoreLists(engine)) {
      return;
    }

    lazy.logConsole.debug("#addEngineToStore: Adding engine:", engine.name);

    if (!skipDuplicateCheck && this.#getEngineByName(engine.name)) {
      throw new lazy.SearchEngineInstallError(
        "duplicate-title",
        `An engine called ${engine.name} already exists!`
      );
    }

    this._engines.set(engine.id, engine);
    if (this._cachedSortedEngines && !this.#dontSetUseSavedOrder) {
      this._cachedSortedEngines.push(engine);
      this.#saveSortedEngineList();
    }
    lazy.SearchUtils.notifyAction(engine, lazy.SearchUtils.MODIFIED_TYPE.ADDED);

    engine._engineAddedToStore = true;
  }

  async #loadEnginesFromSettings(settings, engines) {
    if (!settings.engines) {
      return false;
    }

    lazy.logConsole.debug(
      "#loadEnginesFromSettings: Loading",
      settings.engines.length,
      "engines from settings"
    );

    let defaultEngineChanged = false;
    let skippedEngines = 0;
    let appProvidedEngineIds = new Set(engines.map(e => e.identifier));
    for (let engineJSON of settings.engines) {
      let willBeUserInstalled =
        !!engineJSON._metaData?.["user-installed"] &&
        !appProvidedEngineIds.has(engineJSON.id);
      if (engineJSON._isConfigEngine && !willBeUserInstalled) {
        ++skippedEngines;
        continue;
      }

      let loadPath = engineJSON._loadPath?.toLowerCase();
      if (
        loadPath &&
        (loadPath.startsWith("[distribution]") ||
          loadPath.includes("[app]/extensions/langpack") ||
          loadPath.includes("[other]/langpack") ||
          loadPath.includes("[profile]/extensions/langpack") ||
          loadPath.startsWith("jar:[app]/omni.ja"))
      ) {
        continue;
      }

      try {
        let engine;
        if (loadPath?.startsWith("[user]")) {
          engine = new lazy.UserSearchEngine({ json: engineJSON });
        } else if (engineJSON.extensionID ?? engineJSON._extensionID) {
          skippedEngines++;
          continue;
        } else if (engineJSON._isConfigEngine && willBeUserInstalled) {
          let config =
            await this.#engineSelector.findContextualSearchEngineById(
              engineJSON.id
            );
          engine = new lazy.UserInstalledConfigEngine({ config, settings });
        } else {
          engine = new lazy.OpenSearchEngine({
            json: engineJSON,
          });
        }
        if (
          engine instanceof lazy.OpenSearchEngine &&
          settings.metaData?.defaultEngineId == engine.id
        ) {
          defaultEngineChanged = await this.#maybeApplyOverride(engine);
          if (defaultEngineChanged) {
            continue;
          }
        }
        this.#addEngineToStore(engine);
      } catch (ex) {
        lazy.logConsole.error(
          "Failed to load",
          engineJSON._name,
          "from settings:",
          ex,
          engineJSON
        );
      }
    }

    if (skippedEngines) {
      lazy.logConsole.debug(
        "#loadEnginesFromSettings: skipped",
        skippedEngines,
        "built-in engines."
      );
    }
    return defaultEngineChanged;
  }

  async #maybeApplyOverride(engine) {
    let existingEngine = this.#getEngineByName(engine.name);
    if (
      existingEngine instanceof lazy.ConfigSearchEngine &&
      (await lazy.defaultOverrideAllowlist.canEngineOverride(
        engine,
        existingEngine?.id
      ))
    ) {
      existingEngine.overrideWithEngine({
        engine,
      });
      this.#setEngineDefault(
        false,
        existingEngine,
        this.CHANGE_REASON.CONFIG
      );
      return true;
    }
    return false;
  }

  async _fetchEngineSelectorEngines() {
    let searchEngineSelectorProperties = {
      locale: Services.locale.appLocaleAsBCP47,
      region: lazy.Region.home || "unknown",
      channel: lazy.SearchUtils.MODIFIED_APP_CHANNEL,
      experiment: this.#lazyPrefs.experimentPrefValue,
      distroID: lazy.SearchUtils.distroID ?? "",
    };

    for (let [key, value] of Object.entries(searchEngineSelectorProperties)) {
      this._settings.setMetaDataAttribute(key, value);
    }

    return this.#engineSelector.fetchEngineConfiguration(
      searchEngineSelectorProperties
    );
  }

  #setDefaultFromSelector(refinedConfig) {
    this.#searchDefault = refinedConfig.appDefaultEngineId;
    this.#searchPrivateDefault = refinedConfig.appPrivateDefaultEngineId;
  }

  #saveSortedEngineList() {
    lazy.logConsole.debug("#saveSortedEngineList");

    this._settings.setMetaDataAttribute("useSavedOrder", true);

    var engines = this.#sortedEngines;

    for (var i = 0; i < engines.length; ++i) {
      engines[i].setAttr("order", i + 1);
    }
  }

  #buildSortedEngineList() {
    this._cachedSortedEngines = [];

    if (this._settings.getMetaDataAttribute("useSavedOrder")) {
      lazy.logConsole.debug("#buildSortedEngineList: using saved order");
      let addedEngines = {};

      let needToSaveEngineList = false;

      for (let engine of this._engines.values()) {
        var orderNumber = engine.getAttr("order");

        if (orderNumber && !this._cachedSortedEngines[orderNumber - 1]) {
          this._cachedSortedEngines[orderNumber - 1] = engine;
          addedEngines[engine.name] = engine;
        } else {
          needToSaveEngineList = true;
        }
      }

      var refinedConfig = this._cachedSortedEngines.filter(function (a) {
        return !!a;
      });
      if (this._cachedSortedEngines.length != refinedConfig.length) {
        needToSaveEngineList = true;
      }
      this._cachedSortedEngines = refinedConfig;

      if (needToSaveEngineList) {
        this.#saveSortedEngineList();
      }

      let alphaEngines = [];

      for (let engine of this._engines.values()) {
        if (!(engine.name in addedEngines)) {
          alphaEngines.push(engine);
        }
      }

      const collator = new Intl.Collator();
      alphaEngines.sort((a, b) => {
        return collator.compare(a.name, b.name);
      });
      return (this._cachedSortedEngines =
        this._cachedSortedEngines.concat(alphaEngines));
    }
    lazy.logConsole.debug("#buildSortedEngineList: using default orders");

    return (this._cachedSortedEngines = lazy.SearchUtils.sortEnginesByDefaults({
      engines: Array.from(this._engines.values()),
      appDefaultEngine: this.appDefaultEngine,
      appPrivateDefaultEngine: this.appPrivateDefaultEngine,
    }));
  }

  get #sortedVisibleEngines() {
    return this.#sortedEngines.filter(engine => !engine.hidden);
  }

  #internalRemoveEngine(engine) {
    if (this._cachedSortedEngines) {
      var index = this._cachedSortedEngines.indexOf(engine);
      if (index == -1) {
        throw new Error("Unable to find engine to remove in the cache");
      }
      this._cachedSortedEngines.splice(index, 1);
    }

    this._engines.delete(engine.id);
  }

  #findAndSetNewDefaultEngine({ privateMode }, changeReason) {
    let newDefault = privateMode
      ? this.appPrivateDefaultEngine
      : this.appDefaultEngine;

    if (
      !newDefault ||
      newDefault.hidden ||
      this.#enginesPendingRemoval.has(newDefault)
    ) {
      let sortedEngines = this.#sortedVisibleEngines;
      let generalSearchEngines = sortedEngines.filter(
        e => e.isGeneralPurposeEngine
      );

      let firstVisible = generalSearchEngines.find(
        e => !this.#enginesPendingRemoval.has(e)
      );
      if (firstVisible) {
        newDefault = firstVisible;
      } else if (newDefault) {
        if (!this.#enginesPendingRemoval.has(newDefault)) {
          newDefault.hidden = false;
        } else {
          newDefault = null;
        }
      }

      if (!newDefault) {
        if (!firstVisible) {
          sortedEngines = this.#sortedEngines;
          firstVisible = sortedEngines.find(e => e.isGeneralPurposeEngine);
          if (!firstVisible) {
            firstVisible = sortedEngines[0];
          }
        }
        if (firstVisible) {
          firstVisible.hidden = false;
          newDefault = firstVisible;
        }
      }
    }
    if (!newDefault) {
      lazy.logConsole.error("Could not find a replacement default engine.");
      return null;
    }

    this.#setEngineDefault(privateMode, newDefault, changeReason);

    return privateMode ? this.#currentPrivateEngine : this.#currentEngine;
  }

  #setEngineDefault(privateMode, newEngine, changeReason) {
    if (!(newEngine instanceof lazy.SearchEngine)) {
      throw new TypeError("newEngine is not a SearchEngine instance");
    }

    const newCurrentEngine = this._engines.get(newEngine.id);
    if (!newCurrentEngine) {
      throw new Error("Unable to find the new engine in the engine store");
    }

    if (!(newCurrentEngine instanceof lazy.ConfigSearchEngine)) {
      if (!newCurrentEngine._loadPath) {
        newCurrentEngine._loadPath = "[other]unknown";
      }
      let loadPathHash = lazy.SearchUtils.getVerificationHash(
        newCurrentEngine._loadPath
      );
      let currentHash = newCurrentEngine.getAttr("loadPathHash");
      if (!currentHash || currentHash != loadPathHash) {
        newCurrentEngine.setAttr("loadPathHash", loadPathHash);
        lazy.SearchUtils.notifyAction(
          newCurrentEngine,
          lazy.SearchUtils.MODIFIED_TYPE.CHANGED
        );
      }
    }

    let currentEngine = privateMode
      ? this.#currentPrivateEngine
      : this.#currentEngine;

    if (newCurrentEngine == currentEngine) {
      return;
    }

    currentEngine?.removeExtensionOverride();

    if (privateMode) {
      this.#currentPrivateEngine = newCurrentEngine;
    } else {
      this.#currentEngine = newCurrentEngine;
    }

    let newId = newCurrentEngine.id;
    const appDefaultEngine = privateMode
      ? this.appPrivateDefaultEngine
      : this.appDefaultEngine;
    if (
      newCurrentEngine == appDefaultEngine &&
      !this.#lazyPrefs.experimentPrefValue
    ) {
      newId = "";
    }

    this._settings.setVerifiedMetaDataAttribute(
      privateMode ? "privateDefaultEngineId" : "defaultEngineId",
      newId
    );

    lazy.SearchUtils.notifyAction(
      newCurrentEngine,
      lazy.SearchUtils.MODIFIED_TYPE[
        privateMode ? "DEFAULT_PRIVATE" : "DEFAULT"
      ]
    );
    if (!privateMode && !this.#separatePrivateDefault) {
      lazy.SearchUtils.notifyAction(
        newCurrentEngine,
        lazy.SearchUtils.MODIFIED_TYPE.DEFAULT_PRIVATE
      );
    }
  }

  #onSeparateDefaultPrefChanged(prefName, previousValue, currentValue) {
    this._cachedSortedEngines = null;

    if (
      prefName === "browser.search.separatePrivateDefault" &&
      !previousValue &&
      currentValue
    ) {
      if (this.#appDefaultEngine(true) != this.#appDefaultEngine(false)) {
        this._settings.setMetaDataAttribute(
          "privateDefaultEngineId",
          this.#appDefaultEngine(true).id
        );
      } else {
        this._settings.setMetaDataAttribute(
          "privateDefaultEngineId",
          this.defaultEngine.id
        );
      }
    }

    if (this.defaultEngine != this._getEngineDefault(true)) {
      lazy.SearchUtils.notifyAction(
        this.defaultPrivateEngine,
        lazy.SearchUtils.MODIFIED_TYPE.DEFAULT_PRIVATE
      );
    }

    if (previousValue && !currentValue) {
      this._settings.setMetaDataAttribute("privateDefaultEngineId", "");
      this.#currentPrivateEngine = null;
    }
  }

  #buildParseSubmissionMap() {
    this.#parseSubmissionMap = new Map();

    let keysOfAlternates = new Set();

    for (let engine of this.#sortedEngines) {
      if (engine.hidden) {
        continue;
      }

      let urlParsingInfo = engine.getURLParsingInfo();
      if (!urlParsingInfo) {
        continue;
      }

      let mapValueForEngine = {
        engine,
        termsParameterName: urlParsingInfo.termsParameterName,
      };

      let processDomain = (domain, isAlternate) => {
        let key = domain + urlParsingInfo.path;

        let existingEntry = this.#parseSubmissionMap.get(key);
        if (!existingEntry) {
          if (isAlternate) {
            keysOfAlternates.add(key);
          }
        } else if (!isAlternate && keysOfAlternates.has(key)) {
          keysOfAlternates.delete(key);
        } else {
          return;
        }

        this.#parseSubmissionMap.set(key, mapValueForEngine);
      };

      processDomain(urlParsingInfo.mainDomain, false);
      lazy.SearchStaticData.getAlternateDomains(
        urlParsingInfo.mainDomain
      ).forEach(d => processDomain(d, true));
    }
  }

  #addObservers() {
    if (this.#observersAdded) {
      return;
    }
    this.#observersAdded = true;

    Services.obs.addObserver(this, lazy.SearchUtils.TOPIC_ENGINE_MODIFIED);
    Services.obs.addObserver(this, QUIT_APPLICATION_TOPIC);
    Services.obs.addObserver(this, TOPIC_LOCALES_CHANGE);

    this._settings.addObservers();

    let shutdownState = {
      step: "Not started",
      latestError: {
        message: undefined,
        stack: undefined,
      },
    };
    IOUtils.profileBeforeChange.addBlocker(
      "Search service: shutting down",
      () =>
        (async () => {
          if (!this.isInitialized) {
            lazy.logConsole.warn(
              "not saving settings on shutdown due to initializing."
            );
            return;
          }

          try {
            await this._settings.shutdown(shutdownState);
          } catch (ex) {
            Promise.reject(ex);
          }
        })(),

      () => shutdownState
    );
  }

  #removeObservers() {
    if (this.ignoreListListener) {
      lazy.IgnoreLists.unsubscribe(this.ignoreListListener);
      delete this.ignoreListListener;
    }
    if (this.#queuedIdle) {
      lazy.idleService.removeIdleObserver(this, RECONFIG_IDLE_TIME_SEC);
      this.#queuedIdle = false;
    }

    this._settings.removeObservers();

    Services.obs.removeObserver(this, lazy.Region.REGION_TOPIC);
    Services.obs.removeObserver(this, lazy.SearchUtils.TOPIC_ENGINE_MODIFIED);
    Services.obs.removeObserver(this, QUIT_APPLICATION_TOPIC);
    Services.obs.removeObserver(this, TOPIC_LOCALES_CHANGE);
    this.#observersAdded = false;
    this.#earlyObserversAdded = false;
  }

  QueryInterface = ChromeUtils.generateQI(["nsIObserver", "nsITimerCallback"]);

  observe(subject, topic, verb) {
    switch (topic) {
      case lazy.SearchUtils.TOPIC_ENGINE_MODIFIED: {
        switch (verb) {
          case lazy.SearchUtils.MODIFIED_TYPE.ADDED:
            this.#parseSubmissionMap = null;
            break;
          case lazy.SearchUtils.MODIFIED_TYPE.CHANGED: {
            this.#parseSubmissionMap = null;
            break;
          }
          case lazy.SearchUtils.MODIFIED_TYPE.REMOVED:
            this.#parseSubmissionMap = null;
            break;
        }
        break;
      }
      case "idle": {
        lazy.idleService.removeIdleObserver(this, RECONFIG_IDLE_TIME_SEC);
        this.#queuedIdle = false;
        lazy.logConsole.debug(
          "Reloading engines after idle due to configuration change"
        );
        this.#maybeReloadEngines(this.CHANGE_REASON.CONFIG).catch(
          console.error
        );
        break;
      }

      case QUIT_APPLICATION_TOPIC:
        this.#removeObservers();
        break;

      case TOPIC_LOCALES_CHANGE:

        Services.tm.dispatchToMainThread(() => {
          if (!Services.startup.shuttingDown) {
            this.#maybeReloadEngines(this.CHANGE_REASON.LOCALE).catch(
              console.error
            );
          }
        });
        break;
      case lazy.Region.REGION_TOPIC:
        lazy.logConsole.debug("Region updated:", lazy.Region.home);
        this.#maybeReloadEngines(this.CHANGE_REASON.REGION).catch(
          console.error
        );
        break;
    }
  }

  #didSettingsMetaDataUpdate(metaData) {
    let metaDataProperties = [
      "locale",
      "region",
      "channel",
      "experiment",
      "distroID",
    ];

    return metaDataProperties.some(p => {
      return metaData?.[p] !== this._settings.getMetaDataAttribute(p);
    });
  }

  _showRemovalOfSearchEngineNotificationBox(
    prevCurrentEngineName,
    newCurrentEngineName
  ) {
    lazy.BrowserUtils.callModulesFromCategory(
      { categoryName: "search-service-notification" },
      "search-engine-removal",
      prevCurrentEngineName,
      newCurrentEngineName
    );
  }

  _showSearchSettingsResetNotificationBox(newEngine) {
    lazy.BrowserUtils.callModulesFromCategory(
      { categoryName: "search-service-notification" },
      "search-settings-reset",
      newEngine
    );
  }

  #maybeStartOpenSearchUpdateTimer() {
    if (
      this.#openSearchUpdateTimerStarted ||
      !Services.prefs.getBoolPref(
        lazy.SearchUtils.BROWSER_SEARCH_PREF + "update",
        true
      )
    ) {
      return;
    }

    let engineWithUpdates = [...this._engines.values()].some(
      engine => engine instanceof lazy.OpenSearchEngine && engine.hasUpdates
    );

    if (engineWithUpdates) {
      lazy.logConsole.debug("Engine with updates found, setting update timer");
      lazy.timerManager.registerTimer(
        OPENSEARCH_UPDATE_TIMER_TOPIC,
        this,
        OPENSEARCH_UPDATE_TIMER_INTERVAL,
        true
      );
      this.#openSearchUpdateTimerStarted = true;
    }
  }
})(); 

class SearchDefaultOverrideAllowlistHandler {
  constructor() {
    this.#remoteConfig = lazy.RemoteSettings(
      lazy.SearchUtils.SETTINGS_ALLOWLIST_KEY
    );
  }

  async canEngineOverride(engine, appProvidedEngineId) {
    const overrideEntries = await this.#getAllowlist();

    let entry = overrideEntries.find(
      e =>
        e.thirdPartyId == "opensearch@search.mozilla.org" &&
        e.engineName == engine.name
    );
    if (!entry) {
      return false;
    }

    if (appProvidedEngineId != entry.overridesAppIdv2) {
      return false;
    }

    return entry.urls.some(urlSet =>
      engine.checkSearchUrlMatchesManifest(urlSet)
    );
  }

  async #getAllowlist() {
    let result = [];
    try {
      result = await this.#remoteConfig.get();
    } catch (ex) {
      console.error(ex);
    }
    lazy.logConsole.debug("Allow list is:", result);
    return result;
  }

  #remoteConfig;
}
