/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { SuggestProvider } from "moz-src:///browser/components/urlbar/private/SuggestFeature.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AmpMatchingStrategy:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSuggest.sys.mjs",
  QuickSuggest: "moz-src:///browser/components/urlbar/QuickSuggest.sys.mjs",
  rawSuggestionUrlMatches:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSuggest.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

const TIMESTAMP_TEMPLATE = "%YYYYMMDDHH%";
const TIMESTAMP_LENGTH = 10;
const TIMESTAMP_REGEXP = /^\d{10}$/;

export class AmpSuggestions extends SuggestProvider {
  get enablingPreferences() {
    return [
      "ampFeatureGate",
      "suggest.amp",
      "suggest.quicksuggest.all",
      "suggest.quicksuggest.sponsored",
    ];
  }

  get primaryUserControlledPreferences() {
    return ["suggest.amp"];
  }

  get merinoProvider() {
    return "adm";
  }

  get rustSuggestionType() {
    return "Amp";
  }

  get rustProviderConstraints() {
    let intValue = lazy.UrlbarPrefs.get("ampMatchingStrategy");
    if (!intValue) {
      return null;
    }
    if (!Object.values(lazy.AmpMatchingStrategy).includes(intValue)) {
      this.logger.error(
        "Unknown AmpMatchingStrategy value, using default strategy",
        { intValue }
      );
      return null;
    }
    return {
      ampAlternativeMatching: intValue,
    };
  }

  isSuggestionSponsored() {
    return true;
  }

  getSuggestionTelemetryType() {
    return "adm_sponsored";
  }

  makeResult(queryContext, suggestion, searchString) {
    if (
      this.showLessFrequentlyCount &&
      searchString.length < this.#minKeywordLength
    ) {
      return null;
    }

    let normalized = Object.assign({}, suggestion);
    if (suggestion.source == "merino") {
      normalized.rawUrl = suggestion.url;
      normalized.fullKeyword = suggestion.full_keyword;
      normalized.impressionUrl = suggestion.impression_url;
      normalized.clickUrl = suggestion.click_url;
      normalized.blockId = suggestion.block_id;
      normalized.iabCategory = suggestion.iab_category;
      normalized.requestId = suggestion.request_id;

      if (suggestion.custom_details?.amp) {
        let { amp } = suggestion.custom_details;
        normalized.suggestionId = amp.suggestion_id;
        if (typeof amp.header_text == "string") {
          normalized.fullKeyword = amp.header_text;
        }
      }

      this.#replaceSuggestionTemplates(normalized);
    }

    let isTopPick;
    if (
      suggestion.source == "merino" &&
      typeof suggestion.is_top_pick == "boolean"
    ) {
      isTopPick = suggestion.is_top_pick;
    } else {
      isTopPick =
        (lazy.UrlbarPrefs.get("quickSuggestAmpTopPickCharThreshold") &&
          lazy.UrlbarPrefs.get("quickSuggestAmpTopPickCharThreshold") <=
            queryContext.trimmedLowerCaseSearchString.length) ||
        lazy.UrlbarPrefs.get("quickSuggestSponsoredPriority");
    }

    let richSuggestionIconSize;
    if (!isTopPick) {
      richSuggestionIconSize = 16;
    } else if (
      !lazy.UrlbarPrefs.get("quicksuggest.ampTopPickUseNovaIconSize")
    ) {
      richSuggestionIconSize = 28;
    }

    return new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.URL,
      source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
      isBottomUrlSuggestion: true,
      isBestMatch: isTopPick,
      richSuggestionIconSize,
      payload: {
        url: normalized.url,
        originalUrl: normalized.rawUrl,
        title: normalized.fullKeyword,
        subtitle: normalized.title,
        bottomTextL10n: {
          id: "urlbar-result-action-sponsored",
        },
        requestId: normalized.requestId,
        suggestionId: normalized.suggestionId,
        urlTimestampIndex: normalized.urlTimestampIndex,
        sponsoredImpressionUrl: normalized.impressionUrl,
        sponsoredClickUrl: normalized.clickUrl,
        sponsoredBlockId: normalized.blockId,
        sponsoredAdvertiser: normalized.advertiser,
        sponsoredIabCategory: normalized.iabCategory,
      },
    });
  }

  getResultCommands() {
    const commands = [];

    if (this.canShowLessFrequently) {
      commands.push({
        name: "show_less_frequently",
        l10n: {
          id: "urlbar-result-menu-show-less-frequently2",
        },
      });
    }

    commands.push(
      {
        name: "dismiss",
        l10n: {
          id: "urlbar-result-menu-dismiss-suggestion2",
        },
      },
      { name: "separator" },
      {
        name: "manage",
        l10n: {
          id: "urlbar-result-menu-manage-firefox-suggest2",
        },
      },
      {
        name: "help",
        l10n: {
          id: "urlbar-result-menu-learn-more2",
        },
      }
    );

    return commands;
  }

  onEngagement(queryContext, controller, details, searchString) {
    let { result } = details;

    switch (details.selType) {
      case "help":
      case "manage": {
        return;
      }
      case "dismiss": {
        lazy.QuickSuggest.dismissResult(result);
        controller.removeResult(result);
        break;
      }
      case "show_less_frequently": {
        this.handleShowLessFrequently(controller, result);
        lazy.UrlbarPrefs.set("amp.minKeywordLength", searchString.length + 1);
        break;
      }
    }

  }

  incrementShowLessFrequentlyCount() {
    if (this.canShowLessFrequently) {
      lazy.UrlbarPrefs.set(
        "amp.showLessFrequentlyCount",
        this.showLessFrequentlyCount + 1
      );
    }
  }

  get showLessFrequentlyCount() {
    const count = lazy.UrlbarPrefs.get("amp.showLessFrequentlyCount") || 0;
    return Math.max(count, 0);
  }

  get canShowLessFrequently() {
    const cap = lazy.QuickSuggest.config.showLessFrequentlyCap || 0;
    return !cap || this.showLessFrequentlyCount < cap;
  }

  get #minKeywordLength() {
    let minLength = lazy.UrlbarPrefs.get("amp.minKeywordLength");
    return Math.max(minLength, 0);
  }

  isUrlEquivalentToResultUrl(url, result) {
    let resultURL = result.payload.url;
    if (resultURL.length != url.length) {
      return false;
    }

    if (result.payload.source == "rust") {
      return lazy.rawSuggestionUrlMatches(result.payload.originalUrl, url);
    }

    let { urlTimestampIndex } = result.payload;
    if (typeof urlTimestampIndex != "number" || urlTimestampIndex < 0) {
      return resultURL == url;
    }

    if (
      resultURL.substring(0, urlTimestampIndex) !=
      url.substring(0, urlTimestampIndex)
    ) {
      return false;
    }

    let remainderIndex = urlTimestampIndex + TIMESTAMP_LENGTH;
    if (resultURL.substring(remainderIndex) != url.substring(remainderIndex)) {
      return false;
    }

    let maybeTimestamp = url.substring(
      urlTimestampIndex,
      urlTimestampIndex + TIMESTAMP_LENGTH
    );
    return TIMESTAMP_REGEXP.test(maybeTimestamp);
  }


  #replaceSuggestionTemplates(suggestion) {
    let now = new Date();
    let timestampParts = [
      now.getFullYear(),
      now.getMonth() + 1,
      now.getDate(),
      now.getHours(),
    ];
    let timestamp = timestampParts
      .map(n => n.toString().padStart(2, "0"))
      .join("");
    for (let key of ["url", "clickUrl"]) {
      let value = suggestion[key];
      if (!value) {
        continue;
      }

      let timestampIndex = value.indexOf(TIMESTAMP_TEMPLATE);
      if (timestampIndex >= 0) {
        if (key == "url") {
          suggestion.urlTimestampIndex = timestampIndex;
        }
        suggestion[key] =
          value.substring(0, timestampIndex) +
          timestamp +
          value.substring(timestampIndex + TIMESTAMP_TEMPLATE.length);
      }
    }
  }

  static get TIMESTAMP_TEMPLATE() {
    return TIMESTAMP_TEMPLATE;
  }

  static get TIMESTAMP_LENGTH() {
    return TIMESTAMP_LENGTH;
  }
}
