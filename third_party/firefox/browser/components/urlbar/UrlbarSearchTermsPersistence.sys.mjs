/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

import { UrlbarUtils } from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";

ChromeUtils.defineESModuleGetters(lazy, {
  ConfigSearchEngine:
    "moz-src:///toolkit/components/search/ConfigSearchEngine.sys.mjs",
  RemoteSettings: "resource://services-settings/remote-settings.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});



ChromeUtils.defineLazyGetter(lazy, "logger", () =>
  lazy.UrlbarShared.getLogger({ prefix: "UrlbarSearchTermsPersistence" })
);

const URLBAR_PERSISTENCE_SETTINGS_KEY = "urlbar-persisted-search-terms";

class _UrlbarSearchTermsPersistence {
  #initialized = false;

  #originalProviderInfo = [];

  #searchProviderInfo = [];

  #urlbarSearchTermsPersistenceSettings;

  #urlbarSearchTermsPersistenceSettingsSync;

  async init() {
    if (this.#initialized) {
      return;
    }

    this.#urlbarSearchTermsPersistenceSettings = lazy.RemoteSettings(
      URLBAR_PERSISTENCE_SETTINGS_KEY
    );
    let rawProviderInfo = [];
    try {
      rawProviderInfo = await this.#urlbarSearchTermsPersistenceSettings.get();
    } catch (ex) {
      lazy.logger.error("Could not get settings:", ex);
    }

    this.#urlbarSearchTermsPersistenceSettingsSync = event =>
      this.#onSettingsSync(event);
    this.#urlbarSearchTermsPersistenceSettings.on(
      "sync",
      this.#urlbarSearchTermsPersistenceSettingsSync
    );

    this.#originalProviderInfo = rawProviderInfo;
    this.#setSearchProviderInfo(rawProviderInfo);

    this.#initialized = true;
  }

  uninit() {
    if (!this.#initialized) {
      return;
    }

    try {
      this.#urlbarSearchTermsPersistenceSettings.off(
        "sync",
        this.#urlbarSearchTermsPersistenceSettingsSync
      );
    } catch (ex) {
      lazy.logger.error(
        "Failed to shutdown UrlbarSearchTermsPersistence Remote Settings.",
        ex
      );
    }
    this.#urlbarSearchTermsPersistenceSettings = null;
    this.#urlbarSearchTermsPersistenceSettingsSync = null;

    this.#initialized = false;
  }

  getSearchProviderInfo() {
    return this.#searchProviderInfo;
  }

  overrideSearchTermsPersistenceForTests(providerInfo) {
    let info = providerInfo ? providerInfo : this.#originalProviderInfo;
    this.#setSearchProviderInfo(info);
  }

  getSearchTerm(uri) {
    if (!lazy.SearchService.hasSuccessfullyInitialized || !uri?.spec) {
      return "";
    }

    if (!/^https?:\/\//.test(uri.spec)) {
      return "";
    }

    let searchTerm = "";

    let provider = this.#getProviderInfoForURL(uri.spec);
    if (provider) {
      let result = lazy.SearchService.parseSubmissionURL(uri.spec);
      if (
        !(result.engine instanceof lazy.ConfigSearchEngine) ||
        !this.isDefaultPage(uri, provider)
      ) {
        return "";
      }
      searchTerm = result.terms;
    } else {
      let result = lazy.SearchService.parseSubmissionURL(uri.spec);
      if (!(result.engine instanceof lazy.ConfigSearchEngine)) {
        return "";
      }
      searchTerm = result.engine.searchTermFromResult(uri);
    }

    if (!searchTerm || searchTerm.length > UrlbarUtils.MAX_TEXT_LENGTH) {
      return "";
    }

    let searchTermWithSpacesRemoved = searchTerm.replaceAll(/\s/g, "");

    if (
      searchTermWithSpacesRemoved.startsWith("https://") ||
      searchTermWithSpacesRemoved.startsWith("http://")
    ) {
      return "";
    }

    try {
      let info = Services.uriFixup.getFixupURIInfo(
        searchTermWithSpacesRemoved,
        Ci.nsIURIFixup.FIXUP_FLAG_FIX_SCHEME_TYPOS |
          Ci.nsIURIFixup.FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP
      );
      if (info.keywordAsSent) {
        return searchTerm;
      }
    } catch (e) {}

    return "";
  }

  shouldPersist(state, { uri, isSameDocument, userTypedValue, firstView }) {
    let persist = state.persist;
    if (!persist) {
      return false;
    }

    if (!persist.searchTerms) {
      return false;
    }

    if (userTypedValue && userTypedValue !== persist.searchTerms) {
      return false;
    }

    if (
      isSameDocument &&
      state.persist.provider &&
      !this.isDefaultPage(uri, state.persist.provider)
    ) {
      return false;
    }

    if (
      !firstView &&
      !this.searchModeMatchesState(state.searchModes?.confirmed, state)
    ) {
      return false;
    }

    let origin, pathname;
    try {
      let url = URL.fromURI(uri);
      origin = url.origin;
      pathname = url.pathname;
    } catch (ex) {
      return false;
    }

    if (
      origin !== state.persist.origin ||
      pathname !== state.persist.pathname
    ) {
      return false;
    }

    return true;
  }

  setPersistenceState(state, uri) {
    state.persist = {
      isDefaultEngine: null,

      origin: null,

      originalEngineName: null,

      originalURI: null,

      path: null,

      provider: null,

      searchTerms: "",

      shouldPersist: null,
    };

    let origin, pathname;
    try {
      let url = URL.fromURI(uri);
      origin = url.origin;
      pathname = url.pathname;
    } catch (ex) {
      return;
    }

    let searchTerms = this.getSearchTerm(uri);
    if (!searchTerms || !origin || !pathname) {
      return;
    }

    state.persist.origin = origin;
    state.persist.searchTerms = searchTerms;
    state.persist.pathname = pathname;
    state.persist.originalURI = uri;

    let provider = this.#getProviderInfoForURL(uri?.spec);
    if (provider) {
      state.persist.provider = provider;
    }

    let result = this.#searchModeForUrl(uri.spec);
    state.persist.originalEngineName = result.engineName;
    state.persist.isDefaultEngine = result.isDefaultEngine;
  }

  searchModeMatchesState(searchMode, state) {
    if (searchMode?.engineName === state.persist?.originalEngineName) {
      return true;
    }
    if (!searchMode && state.persist?.isDefaultEngine) {
      return true;
    }
    return false;
  }

  onSearchModeChanged(window) {
    let state = window.gURLBar.getBrowserState(window.gBrowser.selectedBrowser);
    if (!state?.persist) {
      return;
    }

    if (
      state.persist.shouldPersist &&
      !this.searchModeMatchesState(state.searchModes?.confirmed, state)
    ) {
      state.persist.shouldPersist = false;
      window.gURLBar.removeAttribute("persistsearchterms");
    }
  }

  async #onSettingsSync(event) {
    let current = event.data?.current;
    if (current) {
      lazy.logger.debug("Update provider info due to Remote Settings sync.");
      this.#originalProviderInfo = current;
      this.#setSearchProviderInfo(current);
    } else {
      lazy.logger.debug(
        "Ignoring Remote Settings sync data due to missing records."
      );
    }
    Services.obs.notifyObservers(null, "urlbar-persisted-search-terms-synced");
  }

  #searchModeForUrl(url) {
    if (!lazy.SearchService.defaultEngine) {
      return null;
    }
    let result = lazy.SearchService.parseSubmissionURL(url);
    if (!(result.engine instanceof lazy.ConfigSearchEngine)) {
      return null;
    }
    return {
      engineName: result.engine.name,
      isDefaultEngine: result.engine === lazy.SearchService.defaultEngine,
    };
  }

  #setSearchProviderInfo(providerInfo) {
    this.#searchProviderInfo = providerInfo.map(provider => {
      let newProvider = {
        ...provider,
        searchPageRegexp: new RegExp(provider.searchPageRegexp),
      };
      return newProvider;
    });
  }

  #getProviderInfoForURL(url) {
    return this.#searchProviderInfo.find(info =>
      info.searchPageRegexp.test(url)
    );
  }

  isDefaultPage(currentURI, provider) {
    let { searchParams } = URL.fromURI(currentURI);
    if (!searchParams.size) {
      return false;
    }

    if (provider.includeParams?.length) {
      let foundMatch = false;
      for (let param of provider.includeParams) {
        if (param.canBeMissing && !searchParams.has(param.key)) {
          foundMatch = true;
          break;
        }

        if (searchParams.has(param.key) && !param.values?.length) {
          foundMatch = true;
          break;
        }

        let value = searchParams.get(param.key);
        if (value && param?.values.includes(value)) {
          foundMatch = true;
          break;
        }
      }
      if (!foundMatch) {
        return false;
      }
    }

    if (provider.excludeParams) {
      for (let param of provider.excludeParams) {
        let value = searchParams.get(param.key);
        if (!param.values?.length && value) {
          return false;
        }
        if (param.values?.includes(value)) {
          return false;
        }
      }
    }
    return true;
  }
}

export var UrlbarSearchTermsPersistence = new _UrlbarSearchTermsPersistence();
