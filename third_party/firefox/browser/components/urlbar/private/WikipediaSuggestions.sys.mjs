/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { SuggestProvider } from "moz-src:///browser/components/urlbar/private/SuggestFeature.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  QuickSuggest: "moz-src:///browser/components/urlbar/QuickSuggest.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

export class WikipediaSuggestions extends SuggestProvider {
  get enablingPreferences() {
    return [
      "wikipediaFeatureGate",
      "suggest.wikipedia",
      "suggest.quicksuggest.all",
    ];
  }

  get primaryUserControlledPreferences() {
    return ["suggest.wikipedia"];
  }

  get merinoProvider() {
    return "wikipedia";
  }

  get rustSuggestionType() {
    return "Wikipedia";
  }

  isSuggestionSponsored() {
    return false;
  }

  getSuggestionTelemetryType(suggestion) {
    return suggestion.source == "merino" ? "wikipedia" : "adm_nonsponsored";
  }

  makeResult(queryContext, suggestion) {
    return new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.URL,
      source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
      isBottomUrlSuggestion: true,
      richSuggestionIconSize: 16,
      payload: {
        url: suggestion.url,
        title: suggestion.fullKeyword ?? suggestion.full_keyword,
        subtitle: suggestion.title,
        bottomTextL10n: {
          id: "urlbar-result-suggestion-recommended",
        },
      },
    });
  }

  getResultCommands() {
    return [
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
    ];
  }

  onEngagement(queryContext, controller, details, _searchString) {
    let { result } = details;

    if (details.selType == "dismiss") {
      lazy.QuickSuggest.dismissResult(result);
      controller.removeResult(result);
    }
  }
}
