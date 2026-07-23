/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { SuggestProvider } from "moz-src:///browser/components/urlbar/private/SuggestFeature.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  QuickSuggest: "moz-src:///browser/components/urlbar/QuickSuggest.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarSearchUtils:
    "moz-src:///browser/components/urlbar/UrlbarSearchUtils.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
});

const SHOW_COUNTDOWN_THRESHOLD_DAYS = 30;
const MS_PER_DAY = 1000 * 60 * 60 * 24;

export class ImportantDatesSuggestions extends SuggestProvider {
  get enablingPreferences() {
    return ["importantDatesFeatureGate", "suggest.importantDates"];
  }

  get primaryUserControlledPreferences() {
    return ["suggest.importantDates"];
  }

  get rustSuggestionType() {
    return "Dynamic";
  }

  get dynamicRustSuggestionTypes() {
    return ["important_dates"];
  }

  isSuggestionSponsored(suggestion) {
    if (suggestion.data?.result?.payload?.hasOwnProperty("isSponsored")) {
      return suggestion.data.result.payload.isSponsored;
    }
    return false;
  }

  getSuggestionTelemetryType(suggestion) {
    if (suggestion.data?.result?.payload?.hasOwnProperty("telemetryType")) {
      return suggestion.data.result.payload.telemetryType;
    }
    return this.dynamicRustSuggestionTypes[0];
  }

  async makeResult(queryContext, suggestion, _searchString) {
    if (
      !suggestion.data?.result?.payload ||
      typeof suggestion.data.result.payload != "object"
    ) {
      this.logger.warn("Unexpected remote settings suggestion");
      return null;
    }

    return this.#makeDateResult(queryContext, suggestion.data.result.payload);
  }


  #formatDateOrRange(dateStr) {
    if (Array.isArray(dateStr)) {
      let format = new Intl.DateTimeFormat(Services.locale.appLocaleAsBCP47, {
        year: "numeric",
        month: "long",
        day: "numeric",
      });
      let startDate = new Date(dateStr[0] + "T00:00");
      let endDate = new Date(dateStr[1] + "T00:00");
      return format.formatRange(startDate, endDate);
    }

    let format = new Intl.DateTimeFormat(Services.locale.appLocaleAsBCP47, {
      weekday: "long",
      year: "numeric",
      month: "long",
      day: "numeric",
    });
    let date = new Date(dateStr + "T00:00");
    return format.format(date);
  }

  #formatDateCountdown(dateStr, name) {
    if (Array.isArray(dateStr)) {
      let daysUntilStart = this.#getDaysUntil(dateStr[0]);
      let daysUntilEnd = this.#getDaysUntil(dateStr[1]);
      if (daysUntilEnd < 0) {
        throw new Error("Date lies in the past.");
      }
      if (daysUntilStart > 0) {
        return {
          id: "urlbar-result-dates-countdown-range",
          args: { daysUntilStart, name },
        };
      }
      if (daysUntilEnd > 0) {
        return {
          id: "urlbar-result-dates-ongoing",
          args: { daysUntilEnd, name },
        };
      }
      return {
        id: "urlbar-result-dates-ends-today",
        args: { name },
      };
    }

    let daysUntil = this.#getDaysUntil(dateStr);
    if (daysUntil < 0) {
      throw new Error("Date lies in the past.");
    }
    if (daysUntil > 0) {
      return {
        id: "urlbar-result-dates-countdown",
        args: { daysUntilStart: daysUntil, name },
      };
    }
    return {
      id: "urlbar-result-dates-today",
      args: { name },
    };
  }

  #getDaysUntil(dateStr) {
    let now = new Date();
    now.setHours(0, 0, 0, 0);
    let date = new Date(dateStr + "T00:00");

    let msUntil = date.getTime() - now.getTime();
    return Math.round(msUntil / MS_PER_DAY);
  }

  #makeDateResult(queryContext, payload) {
    let eventDateOrRange = payload.dates.find(
      d => this.#getDaysUntil(Array.isArray(d) ? d[1] : d) >= 0
    );
    if (!eventDateOrRange) {
      return null;
    }

    let daysUntilStart = this.#getDaysUntil(
      Array.isArray(eventDateOrRange) ? eventDateOrRange[0] : eventDateOrRange
    );

    let description, descriptionL10n;
    if (daysUntilStart > SHOW_COUNTDOWN_THRESHOLD_DAYS) {
      description = payload.name;
    } else {
      descriptionL10n = {
        ...this.#formatDateCountdown(eventDateOrRange, payload.name),
      };
    }

    let dateString = this.#formatDateOrRange(eventDateOrRange);
    return new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.SEARCH,
      source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
      isBestMatch: true,
      richSuggestionIconSize: 24,
      payload: {
        title: dateString,
        description,
        engine: lazy.UrlbarSearchUtils.getDefaultEngine(queryContext.isPrivate)
          .name,
        descriptionL10n,
        query: payload.name,
        lowerCaseSuggestion: payload.name.toLowerCase(),
        icon: "chrome://browser/skin/calendar-24.svg",
        helpUrl: lazy.QuickSuggest.HELP_URL,
        isManageable: true,
        isBlockable: true,
      },
      highlights: {
        title: lazy.UrlbarUtils.HIGHLIGHT.ALL,
      },
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
}
