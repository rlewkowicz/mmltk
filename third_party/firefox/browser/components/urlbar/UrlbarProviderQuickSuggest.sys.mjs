/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import {
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  QuickSuggest: "moz-src:///browser/components/urlbar/QuickSuggest.sys.mjs",
  SearchUtils: "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  UrlbarSearchUtils:
    "moz-src:///browser/components/urlbar/UrlbarSearchUtils.sys.mjs",
});

const DEFAULT_SUGGESTION_SCORE = 0.2;

export class UrlbarProviderQuickSuggest extends UrlbarProvider {
  get type() {
    return UrlbarUtils.PROVIDER_TYPE.NETWORK;
  }

  static get DEFAULT_SUGGESTION_SCORE() {
    return DEFAULT_SUGGESTION_SCORE;
  }

  async isActive(queryContext) {
    if (
      !queryContext.sources.includes(lazy.UrlbarShared.RESULT_SOURCE.SEARCH) ||
      (queryContext.restrictSource &&
        queryContext.restrictSource != lazy.UrlbarShared.RESULT_SOURCE.SEARCH)
    ) {
      return false;
    }

    if (
      !lazy.UrlbarPrefs.get("quickSuggestEnabled") ||
      queryContext.isPrivate ||
      queryContext.restrictInSearchMode()
    ) {
      return false;
    }

    let trimmedSearchString = queryContext.searchString.trimStart();

    if (trimmedSearchString.trimEnd().length < 2) {
      return false;
    }
    this._trimmedSearchString = trimmedSearchString;
    return true;
  }

  async startQuery(queryContext, addCallback) {
    let instance = this.queryInstance;
    let searchString = this._trimmedSearchString;

    let values = await Promise.all(
      lazy.QuickSuggest.enabledBackends.map(backend =>
        backend.query(searchString, { queryContext })
      )
    );
    if (instance != this.queryInstance) {
      return;
    }

    let suggestions = await this.#filterAndSortSuggestions(values.flat());
    if (instance != this.queryInstance) {
      return;
    }

    let remainingCount = queryContext.maxResults ?? 10;
    for (let suggestion of suggestions) {
      if (!remainingCount) {
        break;
      }

      let result = await this.#makeResult(queryContext, suggestion);
      if (instance != this.queryInstance) {
        return;
      }
      if (result) {
        let canAdd = await this.#canAddResult(result);
        if (instance != this.queryInstance) {
          return;
        }
        if (canAdd) {
          addCallback(this, result);
          if (!result.isHiddenExposure) {
            remainingCount--;
          }
        }
      }
    }
  }

  async #filterAndSortSuggestions(suggestions) {
    let requiredKeys = ["source", "provider"];
    let scoreMap = lazy.UrlbarPrefs.get("quickSuggestScoreMap");
    let suggestionsByFeature = new Map();
    let indexesBySuggestion = new Map();

    for (let i = 0; i < suggestions.length; i++) {
      let suggestion = suggestions[i];

      if (!requiredKeys.every(key => suggestion[key])) {
        this.logger.error("Suggestion is missing one or more required keys", {
          requiredKeys,
          suggestion,
        });
        continue;
      }

      if (typeof suggestion.score != "number" || isNaN(suggestion.score)) {
        suggestion.score = DEFAULT_SUGGESTION_SCORE;
      }

      await this.#applyRanking(suggestion);

      if (scoreMap) {
        let telemetryType = this.#getSuggestionTelemetryType(suggestion);
        if (scoreMap.hasOwnProperty(telemetryType)) {
          let score = parseFloat(scoreMap[telemetryType]);
          if (!isNaN(score)) {
            suggestion.score = score;
          }
        }
      }

      let feature = lazy.QuickSuggest.getFeatureBySource(suggestion);
      let featureSuggestions = suggestionsByFeature.get(feature);
      if (!featureSuggestions) {
        featureSuggestions = [];
        suggestionsByFeature.set(feature, featureSuggestions);
      }
      featureSuggestions.push(suggestion);
      indexesBySuggestion.set(suggestion, i);
    }

    let filteredSuggestions = (
      await Promise.all(
        [...suggestionsByFeature].map(([feature, featureSuggestions]) =>
          feature
            ? feature.filterSuggestions(featureSuggestions)
            : Promise.resolve(featureSuggestions)
        )
      )
    ).flat();

    filteredSuggestions.sort((a, b) => {
      return (
        b.score - a.score ||
        indexesBySuggestion.get(a) - indexesBySuggestion.get(b)
      );
    });

    return filteredSuggestions;
  }

  onImpression(state, queryContext, controller, resultsAndIndexes, details) {
    let resultsByFeature = resultsAndIndexes.reduce((memo, { result }) => {
      let feature = lazy.QuickSuggest.getFeatureByResult(result);
      if (feature) {
        let featureResults = memo.get(feature);
        if (!featureResults) {
          featureResults = [];
          memo.set(feature, featureResults);
        }
        featureResults.push(result);
      }
      return memo;
    }, new Map());

    for (let [feature, featureResults] of resultsByFeature) {
      feature.onImpression(
        state,
        queryContext,
        controller,
        featureResults,
        details
      );
    }
  }

  onEngagement(queryContext, controller, details) {
    let { result } = details;

    let feature = lazy.QuickSuggest.getFeatureByResult(result);
    if (feature) {
      feature.onEngagement(
        queryContext,
        controller,
        details,
        this._trimmedSearchString
      );
      return;
    }

    if (details.selType == "dismiss" && result.payload.isBlockable) {
      lazy.QuickSuggest.dismissResult(result);
      controller.removeResult(result);
    }
  }

  onSearchSessionEnd(queryContext, controller, details) {
    for (let backend of lazy.QuickSuggest.enabledBackends) {
      backend.onSearchSessionEnd(queryContext, controller, details);
    }
  }

  getViewTemplate(result) {
    return lazy.QuickSuggest.getFeatureByResult(result)?.getViewTemplate?.(
      result
    );
  }

  getViewUpdate(result) {
    return lazy.QuickSuggest.getFeatureByResult(result)?.getViewUpdate?.(
      result
    );
  }

  getResultCommands(result) {
    return lazy.QuickSuggest.getFeatureByResult(result)?.getResultCommands?.(
      result
    );
  }

  #getSuggestionTelemetryType(suggestion) {
    let feature = lazy.QuickSuggest.getFeatureBySource(suggestion);
    if (feature) {
      return feature.getSuggestionTelemetryType(suggestion);
    }
    return suggestion.provider;
  }

  async #makeResult(queryContext, suggestion) {
    let result = null;
    let feature = lazy.QuickSuggest.getFeatureBySource(suggestion);
    if (!feature) {
      result = this.#makeUnmanagedResult(queryContext, suggestion);
    } else if (feature.isEnabled) {
      result = await feature.makeResult(
        queryContext,
        suggestion,
        this._trimmedSearchString
      );
    }

    if (!result) {
      return null;
    }


    result.payload.source = suggestion.source;

    result.payload.provider = suggestion.provider;

    if (!result.payload.hasOwnProperty("isSponsored")) {
      result.payload.isSponsored = !!feature?.isSuggestionSponsored(suggestion);
    }

    result.payload.telemetryType = this.#getSuggestionTelemetryType(suggestion);

    result.payload.icon ||= suggestion.icon;
    result.payload.iconBlob ||= suggestion.icon_blob;

    switch (suggestion.source) {
      case "merino":
        if (
          suggestion.dismissal_key &&
          !result.payload.hasOwnProperty("dismissalKey")
        ) {
          result.payload.dismissalKey = suggestion.dismissal_key;
        }
        break;
      case "rust":
        result.payload.suggestionObject = suggestion;
        if (suggestion.suggestionType) {
          result.payload.suggestionType = suggestion.suggestionType;
        }
        break;
    }

    if (!result.hasSuggestedIndex) {
      if (result.isBestMatch) {
        result.isRichSuggestion = true;
        result.richSuggestionIconSize ||= 52;
        result.suggestedIndex = 1;
      } else {
        result.isSuggestedIndexRelativeToGroup = true;
        if (!result.payload.isSponsored) {
          result.suggestedIndex = lazy.UrlbarPrefs.get(
            "quickSuggestNonSponsoredIndex"
          );
        } else if (
          lazy.UrlbarPrefs.get("showSearchSuggestionsFirst") &&
          (await this.queryInstance
            .getProvider("UrlbarProviderSearchSuggestions")
            ?.isActive(queryContext, this.queryInstance.controller)) &&
          lazy.UrlbarSearchUtils.getDefaultEngine(
            queryContext.isPrivate
          ).supportsResponseType(lazy.SearchUtils.URL_TYPE.SUGGEST_JSON)
        ) {
          result.suggestedIndex = lazy.UrlbarPrefs.get(
            "quickSuggestSponsoredIndex"
          );
        } else {
          result.suggestedIndex = -1;
        }
      }
    }

    return result;
  }

  #makeUnmanagedResult(queryContext, suggestion) {
    if (suggestion.source != "merino" || suggestion.provider != "top_picks") {
      return null;
    }

    let payload = {
      url: suggestion.url,
      originalUrl: suggestion.original_url,
      isSponsored: !!suggestion.is_sponsored,
      isBlockable: true,
      isManageable: true,
    };

    let titleHighlights;
    if (suggestion.full_keyword) {
      let { value, highlights } =
        lazy.QuickSuggest.getFullKeywordTitleAndHighlights({
          tokens: queryContext.tokens,
          highlightType: UrlbarUtils.HIGHLIGHT.SUGGESTED,
          fullKeyword: suggestion.full_keyword,
          title: suggestion.title,
        });
      payload.title = value;
      titleHighlights = highlights;
    } else {
      payload.title = suggestion.title;
      titleHighlights = UrlbarUtils.HIGHLIGHT.TYPED;
      payload.shouldShowUrl = true;
    }

    return new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.URL,
      source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
      isBestMatch: !!suggestion.is_top_pick,
      payload,
      highlights: {
        title: titleHighlights,
      },
    });
  }

  cancelQuery() {
    for (let backend of lazy.QuickSuggest.enabledBackends) {
      backend.cancelQuery();
    }
  }

  async #applyRanking(suggestion) {
    let oldScore = suggestion.score;

    let mode = lazy.UrlbarPrefs.get("quickSuggestRankingMode");
    switch (mode) {
      case "random":
        suggestion.score = Math.random();
        break;
      case "interest":
        await this.#updateScoreByRelevance(suggestion);
        break;
      case "default":
      default:
        return;
    }

    this.logger.debug("Applied ranking to suggestion score", {
      mode,
      oldScore,
      newScore: suggestion.score.toFixed(3),
    });
  }

  async #updateScoreByRelevance(suggestion) {
    return;
  }

  async #canAddResult(result) {
    let feature = lazy.QuickSuggest.getFeatureByResult(result);
    if (
      !feature &&
      (!lazy.UrlbarPrefs.get("suggest.quicksuggest.all") ||
        (result.payload.isSponsored &&
          !lazy.UrlbarPrefs.get("suggest.quicksuggest.sponsored")))
    ) {
      return false;
    }

    if (await lazy.QuickSuggest.isResultDismissed(result)) {
      this.logger.debug("Suggestion dismissed, not adding it");
      return false;
    }

    return true;
  }

  async _test_applyRanking(suggestion) {
    await this.#applyRanking(suggestion);
  }
}
