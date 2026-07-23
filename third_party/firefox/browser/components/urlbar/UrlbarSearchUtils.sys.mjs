/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */



import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  AppProvidedConfigEngine:
    "moz-src:///toolkit/components/search/ConfigSearchEngine.sys.mjs",
  ConfigSearchEngine:
    "moz-src:///toolkit/components/search/ConfigSearchEngine.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  UrlUtils: "resource://gre/modules/UrlUtils.sys.mjs",
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
  separatePrivateDefaultUIEnabled: {
    pref: "browser.search.separatePrivateDefault.ui.enabled",
    default: false,
  },
  separatePrivateDefault: {
    pref: "browser.search.separatePrivateDefault",
    default: false,
  },
});

const SEARCH_ENGINE_TOPIC = "browser-search-engine-modified";

class SearchUtils {
  constructor() {
    this._refreshEnginesByAliasPromise = Promise.resolve();
    this.QueryInterface = ChromeUtils.generateQI([
      "nsIObserver",
      "nsISupportsWeakReference",
    ]);
  }

  async init() {
    if (!this._initPromise) {
      this._initPromise = this._initInternal();
    }
    await this._initPromise;
  }

  async enginesForDomainPrefix(prefix, { matchAllDomainLevels = false } = {}) {
    try {
      await this.init();
    } catch {
      return [];
    }
    prefix = prefix.toLowerCase();

    let partialMatchEngines = [];
    function matchPrefix(engine, engineHost) {
      let parts = engineHost.split(".");
      for (let i = 1; i < parts.length - 1; ++i) {
        if (parts.slice(i).join(".").startsWith(prefix)) {
          partialMatchEngines.push(engine);
        }
      }
    }

    let perfectMatchEngines = [];
    let perfectMatchEngineSet = new Set();
    for (let engine of await lazy.SearchService.getVisibleEngines()) {
      if (engine.hideOneOffButton) {
        continue;
      }
      let domain = engine.searchUrlDomain;
      if (domain.startsWith(prefix) || domain.startsWith("www." + prefix)) {
        perfectMatchEngines.push(engine);
        perfectMatchEngineSet.add(engine);
      }

      if (matchAllDomainLevels) {
        if (prefix.includes(".")) {
          matchPrefix(engine, domain);
        }
        matchPrefix(
          engine,
          domain.substr(0, domain.length - engine.searchUrlPublicSuffix.length)
        );
      }
    }

    let engines = perfectMatchEngines;
    let engineSet = perfectMatchEngineSet;
    for (let engine of partialMatchEngines) {
      if (!engineSet.has(engine)) {
        engineSet.add(engine);
        engines.push(engine);
      }
    }
    return engines;
  }

  async engineForAlias(alias, searchString = null) {
    try {
      await Promise.all([this.init(), this._refreshEnginesByAliasPromise]);
    } catch {
      return null;
    }

    let engine = this._enginesByAlias.get(alias.toLocaleLowerCase());
    if (engine && searchString) {
      let query = lazy.UrlbarUtils.substringAfter(searchString, alias);
      if (!lazy.UrlUtils.REGEXP_SPACES_START.test(query)) {
        return null;
      }
    }
    return engine || null;
  }

  async tokenAliasEngines() {
    try {
      await this.init();
    } catch {
      return [];
    }

    let engines = await lazy.SearchService.getVisibleEngines();
    let orderedEngines = this.#orderEnginesForAliases(engines);

    let tokenAliasEngines = [];
    for (let engine of orderedEngines) {
      let tokenAliases = this._aliasesForEngine(engine).filter(a =>
        a.startsWith("@")
      );
      if (tokenAliases.length) {
        tokenAliasEngines.push({ engine, tokenAliases });
      }
    }
    return tokenAliasEngines;
  }

  getRootDomainFromEngine(engine) {
    let domain = engine.searchUrlDomain;
    let suffix = engine.searchUrlPublicSuffix;
    if (!suffix) {
      if (domain.endsWith(".test")) {
        suffix = "test";
      } else {
        return domain;
      }
    }
    domain = domain.substr(
      0,
      domain.length - suffix.length - 1
    );
    let domainParts = domain.split(".");
    return domainParts.pop();
  }

  getDefaultEngine(isPrivate = false) {
    if (!lazy.SearchService.hasSuccessfullyInitialized) {
      return null;
    }

    return lazy.separatePrivateDefaultUIEnabled &&
      lazy.separatePrivateDefault &&
      isPrivate
      ? lazy.SearchService.defaultPrivateEngine
      : lazy.SearchService.defaultEngine;
  }

  get separatePrivateDefaultUIEnabled() {
    return lazy.separatePrivateDefaultUIEnabled;
  }

  get separatePrivateDefault() {
    return lazy.separatePrivateDefault;
  }

  getSearchModeScalarKey(searchMode) {
    let scalarKey;
    if (searchMode.engineName) {
      let engine = lazy.SearchService.getEngineByName(searchMode.engineName);
      let resultDomain = engine.searchUrlDomain;
      if (!(engine instanceof lazy.ConfigSearchEngine)) {
        scalarKey = "other";
      } else if (resultDomain.includes("amazon.")) {
        scalarKey = "Amazon";
      } else if (resultDomain.endsWith("wikipedia.org")) {
        scalarKey = "Wikipedia";
      } else {
        scalarKey = searchMode.engineName;
      }
    } else if (searchMode.source) {
      scalarKey =
        lazy.UrlbarUtils.getResultSourceName(searchMode.source) || "other";
      scalarKey += searchMode.restrictType ? `_${searchMode.restrictType}` : "";
    }

    return scalarKey;
  }

  resultIsSERP(result, allowedSources = null) {
    if (allowedSources && !allowedSources?.includes(result.source)) {
      return false;
    }
    try {
      return !!lazy.SearchService.parseSubmissionURL(result.payload.url)
        ?.engine;
    } catch (ex) {
      return false;
    }
  }

  resetInitPromiseForTests() {
    this._initPromise = null;
  }

  async _initInternal() {
    await lazy.SearchService.init();
    await this._refreshEnginesByAlias();
    Services.obs.addObserver(this, SEARCH_ENGINE_TOPIC, true);
  }

  #orderEnginesForAliases(engines) {
    let orderedEngines = new Set();

    orderedEngines.add(lazy.SearchService.defaultEngine);
    orderedEngines.add(lazy.SearchService.defaultPrivateEngine);

    for (let engine of engines) {
      if (engine instanceof lazy.AppProvidedConfigEngine) {
        orderedEngines.add(engine);
      }
    }

    for (let engine of engines) {
      orderedEngines.add(engine);
    }

    return orderedEngines.values();
  }

  async _refreshEnginesByAlias() {
    this._enginesByAlias = new Map();
    let engines = await lazy.SearchService.getVisibleEngines();
    let orderedEngines = this.#orderEnginesForAliases(engines);

    for (let engine of orderedEngines) {
      this.#addAliasesForEngine(engine);
    }
  }

  #addAliasesForEngine(engine) {
    for (let alias of this._aliasesForEngine(engine)) {
      this._enginesByAlias.getOrInsert(alias, engine);
    }
  }

  serpsAreEquivalent(historySerp, generatedSerp, ignoreParams = []) {
    let historyParams = new URL(historySerp).searchParams;
    let generatedParams = new URL(generatedSerp).searchParams;
    if (
      !Array.from(historyParams.entries()).every(
        ([key, value]) =>
          ignoreParams.includes(key) || value === generatedParams.get(key)
      )
    ) {
      return false;
    }

    return true;
  }

  _aliasesForEngine(engine) {
    return engine.aliases.reduce((aliases, aliasWithCase) => {
      let alias = aliasWithCase.toLocaleLowerCase();
      aliases.push(alias);
      if (!alias.startsWith("@")) {
        aliases.push("@" + alias);
      }
      return aliases;
    }, []);
  }

  getEngineByName(engineName) {
    if (!lazy.SearchService.hasSuccessfullyInitialized) {
      return null;
    }

    return lazy.SearchService.getEngineByName(engineName);
  }

  observe(subject, topic, data) {
    switch (data) {
      case "engine-added":
      case "engine-changed":
      case "engine-removed":
      case "engine-default":
      case "engine-default-private":
        this._refreshEnginesByAliasPromise = this._refreshEnginesByAlias();
        break;
    }
  }
}

export var UrlbarSearchUtils = new SearchUtils();
