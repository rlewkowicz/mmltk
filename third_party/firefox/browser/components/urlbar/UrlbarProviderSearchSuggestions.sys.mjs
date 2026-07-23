/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import {
  SkippableTimer,
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  DEFAULT_FORM_HISTORY_PARAM:
    "moz-src:///toolkit/components/search/SearchSuggestionController.sys.mjs",
  FormHistory: "resource://gre/modules/FormHistory.sys.mjs",
  SearchSuggestionController:
    "moz-src:///toolkit/components/search/SearchSuggestionController.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarProviderTopSites:
    "moz-src:///browser/components/urlbar/UrlbarProviderTopSites.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  UrlbarSearchUtils:
    "moz-src:///browser/components/urlbar/UrlbarSearchUtils.sys.mjs",
  UrlbarTokenizer:
    "moz-src:///browser/components/urlbar/UrlbarTokenizer.sys.mjs",
  UrlUtils: "resource://gre/modules/UrlUtils.sys.mjs",
});


const RESULT_MENU_COMMANDS = {
  TRENDING_BLOCK: "trendingblock",
  TRENDING_HELP: "help",
};

const TRENDING_HELP_URL =
  Services.urlFormatter.formatURLPref("app.support.baseURL") +
  "google-trending-searches-on-awesomebar";

function looksLikeUrl(str, ignoreAlphanumericHosts = false) {
  return (
    !lazy.UrlUtils.REGEXP_SPACES.test(str) &&
    (["/", "@", ":", "["].some(c => str.includes(c)) ||
      (ignoreAlphanumericHosts
        ? /^([\[\]A-Z0-9-]+\.){3,}[^.]+$/i.test(str)
        : str.includes(".")))
  );
}

export class UrlbarProviderSearchSuggestions extends UrlbarProvider {
  static RICH_ICON_SIZE = 28;

  constructor() {
    super();
  }

  get type() {
    return UrlbarUtils.PROVIDER_TYPE.NETWORK;
  }

  async isActive(queryContext) {
    if (AppConstants.MOZ_MINIMAL_BROWSER) {
      return false;
    }

    if (
      !queryContext.sources.includes(lazy.UrlbarShared.RESULT_SOURCE.SEARCH) ||
      (queryContext.restrictSource &&
        queryContext.restrictSource != lazy.UrlbarShared.RESULT_SOURCE.SEARCH)
    ) {
      return false;
    }

    if (
      !queryContext.trimmedSearchString &&
      !this._isTokenOrRestrictionPresent(queryContext) &&
      !this.#shouldFetchTrending(queryContext)
    ) {
      return false;
    }

    if (!this._allowSuggestions(queryContext)) {
      return false;
    }

    let wantsLocalSuggestions =
      lazy.UrlbarPrefs.get("maxHistoricalSearchSuggestions") &&
      queryContext.trimmedSearchString;

    return (
      !!wantsLocalSuggestions || this._allowRemoteSuggestions(queryContext)
    );
  }

  _isTokenOrRestrictionPresent(queryContext) {
    return (
      queryContext.searchString.startsWith("@") ||
      (queryContext.restrictSource &&
        queryContext.restrictSource ==
          lazy.UrlbarShared.RESULT_SOURCE.SEARCH) ||
      queryContext.tokens.some(
        t => t.type == lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_SEARCH
      ) ||
      (queryContext.searchMode &&
        queryContext.sources.includes(lazy.UrlbarShared.RESULT_SOURCE.SEARCH))
    );
  }

  _allowSuggestions(queryContext) {
    if (
      (queryContext.sapName == "urlbar" &&
        !lazy.UrlbarPrefs.get("suggest.searches") &&
        !this._isTokenOrRestrictionPresent(queryContext)) ||
      !lazy.UrlbarPrefs.get("browser.search.suggest.enabled") ||
      (queryContext.isPrivate &&
        !lazy.UrlbarPrefs.get("browser.search.suggest.enabled.private"))
    ) {
      return false;
    }
    return true;
  }

  _allowRemoteSuggestions(
    queryContext,
    searchString = queryContext.searchString
  ) {
    if (queryContext.prohibitRemoteResults) {
      return false;
    }

    if (this.#shouldFetchTrending(queryContext)) {
      return true;
    }

    if (!searchString.trim()) {
      return false;
    }

    if (this._isTokenOrRestrictionPresent(queryContext)) {
      return true;
    }

    if (
      !!this._lastLowResultsSearchSuggestion &&
      searchString.length > this._lastLowResultsSearchSuggestion.length &&
      searchString.startsWith(this._lastLowResultsSearchSuggestion)
    ) {
      return false;
    }

    return queryContext.allowRemoteResults(
      searchString,
      lazy.UrlbarPrefs.get("trending.featureGate")
    );
  }

  async startQuery(queryContext, addCallback, controller) {
    let instance = this.queryInstance;

    let aliasEngine = await this._maybeGetAlias(queryContext);
    if (!aliasEngine) {
      if (queryContext.searchString.startsWith("@")) {
        return;
      }
    }

    let query = aliasEngine
      ? aliasEngine.query
      : UrlbarUtils.substringAt(
          queryContext.searchString,
          queryContext.tokens[0]?.value || ""
        ).trim();

    let leadingRestrictionToken = null;
    if (
      lazy.UrlbarTokenizer.isRestrictionToken(queryContext.tokens[0]) &&
      (queryContext.tokens.length > 1 ||
        queryContext.tokens[0].type ==
          lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_SEARCH)
    ) {
      leadingRestrictionToken = queryContext.tokens[0].value;
    }

    if (leadingRestrictionToken === lazy.UrlbarShared.RESTRICT_TOKENS.SEARCH) {
      query = UrlbarUtils.substringAfter(query, leadingRestrictionToken).trim();
    }

    let engine;
    if (aliasEngine) {
      engine = aliasEngine.engine;
    } else if (queryContext.searchMode?.engineName) {
      engine = lazy.UrlbarSearchUtils.getEngineByName(
        queryContext.searchMode.engineName
      );
    } else {
      engine = lazy.UrlbarSearchUtils.getDefaultEngine(queryContext.isPrivate);
    }

    if (!engine) {
      return;
    }

    let alias = (aliasEngine && aliasEngine.alias) || "";
    let results = await this.#fetchSearchSuggestions(
      queryContext,
      engine,
      query,
      alias,
      controller.browserWindow
    );

    if (!results || instance != this.queryInstance) {
      return;
    }

    for (let result of results) {
      addCallback(this, result);
    }
  }

  getPriority(queryContext) {
    if (this.#shouldFetchTrending(queryContext)) {
      return AppConstants.MOZ_PLACES ? lazy.UrlbarProviderTopSites.PRIORITY : 1;
    }
    return 0;
  }

  cancelQuery() {
    if (this.#suggestionsController) {
      this.#suggestionsController.stop();
    }
  }

  getResultCommands(result) {
    if (result.payload.trending) {
      return  ([
        {
          name: RESULT_MENU_COMMANDS.TRENDING_BLOCK,
          l10n: { id: "urlbar-result-menu-trending-dont-show2" },
        },
        {
          name: "separator",
        },
        {
          name: RESULT_MENU_COMMANDS.TRENDING_HELP,
          l10n: { id: "urlbar-result-menu-learn-more2" },
        },
      ]);
    }
    return undefined;
  }

  onEngagement(queryContext, controller, details) {
    let { result } = details;

    if (details.selType == "dismiss") {
      lazy.FormHistory.update({
        op: "remove",
        fieldname: lazy.DEFAULT_FORM_HISTORY_PARAM,
        value: result.payload.suggestion,
      }).catch(error =>
        console.error(`Removing form history failed: ${error}`)
      );
      controller.removeResult(result);
      return;
    }

    switch (details.selType) {
      case RESULT_MENU_COMMANDS.TRENDING_HELP:
        break;
      case RESULT_MENU_COMMANDS.TRENDING_BLOCK:
        lazy.UrlbarPrefs.set("suggest.trending", false);
        this.#recordTrendingBlockedTelemetry();
        this.#replaceTrendingResultWithAcknowledgement(controller);
        break;
    }
  }

  #suggestionsController;

  async #fetchSearchSuggestions(
    queryContext,
    engine,
    searchString,
    alias,
    win
  ) {
    if (!engine) {
      return null;
    }

    if (!this.#suggestionsController) {
      this.#suggestionsController = new lazy.SearchSuggestionController();
    }

    let maxLocalResults = queryContext.maxResults + 1;

    let allowRemote = this._allowRemoteSuggestions(queryContext, searchString);
    let maxRemoteResults = allowRemote ? queryContext.maxResults + 1 : 0;

    if (allowRemote && this.#shouldFetchTrending(queryContext)) {
      if (
        queryContext.searchMode &&
        lazy.UrlbarPrefs.get("trending.maxResultsSearchMode") != -1
      ) {
        maxRemoteResults = lazy.UrlbarPrefs.get(
          "trending.maxResultsSearchMode"
        );
      } else if (
        !queryContext.searchMode &&
        lazy.UrlbarPrefs.get("trending.maxResultsNoSearchMode") != -1
      ) {
        maxRemoteResults = lazy.UrlbarPrefs.get(
          "trending.maxResultsNoSearchMode"
        );
      }
    }

    let restrictToEngine =
      queryContext.sapName != "searchbar" &&
      this._isTokenOrRestrictionPresent(queryContext);

    let fetchData = await this.#suggestionsController.fetch({
      searchString,
      inPrivateBrowsing: queryContext.isPrivate,
      engine,
      userContextId: queryContext.userContextId,
      restrictToEngine,
      dedupeRemoteAndLocal: false,
      fetchTrending: this.#shouldFetchTrending(queryContext),
      maxLocalResults,
      maxRemoteResults,
    });

    if (!fetchData) {
      return null;
    }

    let results = [];

    if (lazy.UrlbarPrefs.get("maxHistoricalSearchSuggestions")) {
      for (let entry of fetchData.local) {
        results.push(makeFormHistoryResult(queryContext, engine, entry));
      }
    }

    if (
      allowRemote &&
      !fetchData.remote.length &&
      searchString.length > lazy.UrlbarPrefs.get("maxCharsForSearchSuggestions")
    ) {
      this._lastLowResultsSearchSuggestion = searchString;
    }

    let tailTimer = new SkippableTimer({
      name: "ProviderSearchSuggestions",
      time: 100,
      logger: this.logger,
    });

    for (let entry of fetchData.remote) {
      if (looksLikeUrl(entry.value)) {
        continue;
      }

      let tail = entry.tail;
      let tailPrefix = entry.matchPrefix;

      if (tail && !lazy.UrlbarPrefs.get("richSuggestions.tail")) {
        continue;
      }

      if (!tail) {
        await tailTimer.fire().catch(ex => this.logger.error(ex));
      }

      try {
        let query = searchString.trim();
        let suggestion = entry.value;
        let title;
        let titleHighlight;
        if (tail && entry.tailOffsetIndex >= 0) {
          title = tail;
          titleHighlight = UrlbarUtils.HIGHLIGHT.SUGGESTED;
        } else if (suggestion) {
          title = suggestion;
          titleHighlight = UrlbarUtils.HIGHLIGHT.SUGGESTED;
        } else {
          title = query;
        }

        results.push(
          new lazy.UrlbarResult({
            type: lazy.UrlbarShared.RESULT_TYPE.SEARCH,
            source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
            isRichSuggestion: !!entry.icon,
            richSuggestionIconSize: entry.icon
              ? UrlbarProviderSearchSuggestions.RICH_ICON_SIZE
              : undefined,
            payload: {
              title,
              engine: engine.name,
              suggestion,
              lowerCaseSuggestion: entry.value.toLocaleLowerCase(),
              tailPrefix,
              tail,
              tailOffsetIndex: tail ? entry.tailOffsetIndex : undefined,
              keyword: alias || undefined,
              trending: entry.trending,
              description: entry.description || undefined,
              query,
              icon: !entry.value
                ? await engine.getIconURL()
                : UrlbarUtils.getRemoteIconUrl(
                    entry.icon,
                    UrlbarProviderSearchSuggestions.RICH_ICON_SIZE,
                    win
                  ),
              helpUrl: entry.trending ? TRENDING_HELP_URL : undefined,
            },
            highlights: {
              title: titleHighlight,
            },
          })
        );
      } catch (err) {
        this.logger.error(err);
        continue;
      }
    }

    await tailTimer.promise;
    return results;
  }


  async _maybeGetAlias(queryContext) {
    if (queryContext.searchMode) {
      return null;
    }

    let possibleAlias = queryContext.tokens[0]?.value;
    if (!possibleAlias || possibleAlias == "@") {
      return null;
    }

    let query = UrlbarUtils.substringAfter(
      queryContext.searchString,
      possibleAlias
    );

    if (!lazy.UrlUtils.REGEXP_SPACES_START.test(query)) {
      return null;
    }

    let engineMatch =
      await lazy.UrlbarSearchUtils.engineForAlias(possibleAlias);
    if (engineMatch) {
      return {
        engine: engineMatch,
        alias: possibleAlias,
        query: query.trim(),
      };
    }

    return null;
  }

  #shouldFetchTrending(queryContext) {
    if (AppConstants.MOZ_MINIMAL_BROWSER) {
      return false;
    }
    return !!(
      queryContext.searchString == "" &&
      lazy.UrlbarPrefs.get("trending.featureGate") &&
      lazy.UrlbarPrefs.get("suggest.trending") &&
      (queryContext.searchMode ||
        !lazy.UrlbarPrefs.get("trending.requireSearchMode"))
    );
  }

  #recordTrendingBlockedTelemetry() {
  }

  #replaceTrendingResultWithAcknowledgement(controller) {
    let resultsToRemove = controller.view.visibleResults.filter(
      result => result.payload.trending
    );
    if (resultsToRemove.length) {
      resultsToRemove[0].acknowledgeDismissalL10n = {
        id: "urlbar-trending-dismissal-acknowledgment",
      };
    }
    resultsToRemove.reverse();
    resultsToRemove.forEach(result => controller.removeResult(result));
  }
}

function makeFormHistoryResult(queryContext, engine, entry) {
  return new lazy.UrlbarResult({
    type: lazy.UrlbarShared.RESULT_TYPE.SEARCH,
    source: lazy.UrlbarShared.RESULT_SOURCE.HISTORY,
    payload: {
      engine: engine.name,
      suggestion: entry.value,
      title: entry.value,
      lowerCaseSuggestion: entry.value.toLocaleLowerCase(),
      isBlockable: true,
      blockL10n: { id: "urlbar-result-menu-remove-from-history2" },
      helpUrl:
        Services.urlFormatter.formatURLPref("app.support.baseURL") +
        "awesome-bar-result-menu",
    },
    highlights: {
      suggestion: UrlbarUtils.HIGHLIGHT.SUGGESTED,
    },
  });
}
