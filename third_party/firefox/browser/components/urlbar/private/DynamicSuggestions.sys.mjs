/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { SuggestProvider } from "moz-src:///browser/components/urlbar/private/SuggestFeature.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  QuickSuggest: "moz-src:///browser/components/urlbar/QuickSuggest.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
});

export class DynamicSuggestions extends SuggestProvider {
  get enablingPreferences() {
    return ["quicksuggest.dynamicSuggestionTypes"];
  }

  get shouldEnable() {
    return !!this.dynamicRustSuggestionTypes.length;
  }

  get rustSuggestionType() {
    return "Dynamic";
  }

  get dynamicRustSuggestionTypes() {
    return [...lazy.UrlbarPrefs.get("quicksuggest.dynamicSuggestionTypes")];
  }

  isSuggestionSponsored(suggestion) {
    return !!suggestion.data?.result?.payload?.isSponsored;
  }

  getSuggestionTelemetryType(suggestion) {
    if (suggestion.data?.result?.payload?.hasOwnProperty("telemetryType")) {
      return suggestion.data.result.payload.telemetryType;
    }
    if (suggestion.data?.result?.isHiddenExposure) {
      return "exposure";
    }
    return suggestion.suggestionType;
  }

  makeResult(queryContext, suggestion, _searchString) {
    let { data } = suggestion;
    if (!data || typeof data != "object") {
      this.logger.warn(
        "suggestion.data is falsey or not an object, ignoring suggestion"
      );
      return null;
    }

    let { result } = data;
    if (!result || typeof result != "object") {
      this.logger.warn(
        "suggestion.data.result is falsey or not an object, ignoring suggestion"
      );
      return null;
    }

    let payload = {};
    if (result.hasOwnProperty("payload")) {
      if (typeof result.payload != "object") {
        this.logger.warn(
          "suggestion.data.result.payload is not an object, ignoring suggestion"
        );
        return null;
      }
      payload = result.payload;
    }

    if (result.isHiddenExposure) {
      return this.#makeExposureResult(suggestion, payload);
    }

    if (
      !result.bypassSuggestAll &&
      (!lazy.UrlbarPrefs.get("suggest.quicksuggest.all") ||
        (payload.isSponsored &&
          !lazy.UrlbarPrefs.get("suggest.quicksuggest.sponsored")))
    ) {
      return null;
    }

    payload.isManageable = true;
    payload.helpUrl = lazy.QuickSuggest.HELP_URL;

    let resultProperties = { ...result };
    delete resultProperties.payload;
    return new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.URL,
      source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
      ...resultProperties,
      payload,
    });
  }

  onEngagement(_queryContext, controller, details, _searchString) {
    switch (details.selType) {
      case "manage":
        break;
      case "dismiss": {
        let { result } = details;
        lazy.QuickSuggest.dismissResult(result);
        result.acknowledgeDismissalL10n = {
          id: "firefox-suggest-dismissal-acknowledgment-one",
        };
        controller.removeResult(result);
        break;
      }
    }
  }

  #makeExposureResult(suggestion, payload) {
    return new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.DYNAMIC,
      source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
      exposureTelemetry: lazy.UrlbarUtils.EXPOSURE_TELEMETRY.HIDDEN,
      payload: {
        ...payload,
        dynamicType: "exposure",
      },
    });
  }
}
