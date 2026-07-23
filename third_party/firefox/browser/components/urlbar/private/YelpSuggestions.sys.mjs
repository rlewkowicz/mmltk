/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { SuggestProvider } from "moz-src:///browser/components/urlbar/private/SuggestFeature.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  GeolocationUtils:
    "moz-src:///browser/components/urlbar/private/GeolocationUtils.sys.mjs",
  GeonameMatchType:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSuggest.sys.mjs",
  QuickSuggest: "moz-src:///browser/components/urlbar/QuickSuggest.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  YelpSubjectType:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSuggest.sys.mjs",
});


const RESULT_MENU_COMMAND = {
  DISMISS: "dismiss",
  HELP: "help",
  INACCURATE_LOCATION: "inaccurate_location",
  MANAGE: "manage",
  NOT_INTERESTED: "not_interested",
  SHOW_LESS_FREQUENTLY: "show_less_frequently",
};

export class YelpSuggestions extends SuggestProvider {
  get enablingPreferences() {
    return [
      "yelpFeatureGate",
      "suggest.yelp",
      "suggest.quicksuggest.all",
      "suggest.quicksuggest.sponsored",
    ];
  }

  get primaryUserControlledPreferences() {
    return ["suggest.yelp"];
  }

  get rustSuggestionType() {
    return "Yelp";
  }

  get mlIntent() {
    return "yelp_intent";
  }

  get isMlIntentEnabled() {
    return lazy.UrlbarPrefs.get("yelpMlEnabled");
  }

  get showLessFrequentlyCount() {
    const count = lazy.UrlbarPrefs.get("yelp.showLessFrequentlyCount") || 0;
    return Math.max(count, 0);
  }

  get canShowLessFrequently() {
    const cap =
      lazy.UrlbarPrefs.get("yelpShowLessFrequentlyCap") ||
      lazy.QuickSuggest.config.showLessFrequentlyCap ||
      0;
    return !cap || this.showLessFrequentlyCount < cap;
  }

  isSuggestionSponsored(_suggestion) {
    return true;
  }

  getSuggestionTelemetryType() {
    return "yelp";
  }

  enable(enabled) {
    if (!enabled) {
      this.#metadataCache = null;
    }
  }

  async filterSuggestions(suggestions) {

    let suggestion;
    if (!lazy.UrlbarPrefs.get("yelpMlEnabled")) {
      suggestion = suggestions.find(s => s.source != "ml");
      if (suggestion) {
        suggestion = this.#normalizeRustSuggestion(suggestion);
      }
    } else {
      suggestion = suggestions.find(s => s.source == "ml");
      if (suggestion) {
        if (!this.#metadataCache) {
          this.#metadataCache = await this.#makeMetadataCache();
        }
        suggestion = this.#normalizeMlSuggestion(suggestion);
      }
    }

    return suggestion ? [suggestion] : [];
  }

  async makeResult(queryContext, suggestion, searchString) {
    if (
      (this.showLessFrequentlyCount || !suggestion.subjectExactMatch) &&
      searchString.length < this.#minKeywordLength
    ) {
      return null;
    }

    let { city, region } = suggestion;
    if (!city && !region) {
      let geo = await lazy.GeolocationUtils.geolocation();
      if (geo) {
        city = geo.city;
        region = geo.region_code;
      }
    } else {
      let match = await this.#bestCityRegion(city, region);
      if (!match) {
        return null;
      }
      city = match.city;
      region = match.region;
    }

    let url = new URL(suggestion.url);

    let title = suggestion.title;
    let locationStr = [city, region].filter(s => !!s).join(", ");
    if (locationStr) {
      url.searchParams.set(suggestion.locationParam, locationStr);
      if (!suggestion.hasLocationSign) {
        title += " in";
      }
      title += " " + locationStr;
    }

    url.searchParams.set("utm_medium", "partner");
    url.searchParams.set("utm_source", "mozilla");

    let resultProperties = {
      type: lazy.UrlbarShared.RESULT_TYPE.URL,
      source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
      isBottomUrlSuggestion: true,
      isBestMatch: lazy.UrlbarPrefs.get("yelpSuggestPriority"),
    };

    if (!resultProperties.isBestMatch) {
      let suggestedIndex = lazy.UrlbarPrefs.get("yelpSuggestNonPriorityIndex");
      if (suggestedIndex !== null) {
        resultProperties.isSuggestedIndexRelativeToGroup = true;
        resultProperties.suggestedIndex = suggestedIndex;
      }
    }

    let payload = {
      url: url.toString(),
      originalUrl: suggestion.url,
      subtitleL10n: { id: "urlbar-result-yelp-subtitle" },
      bottomTextL10n: {
        id: "urlbar-result-action-sponsored",
      },
      iconBlob: suggestion.icon_blob,
    };

    if (
      lazy.UrlbarPrefs.get("yelpServiceResultDistinction") &&
      suggestion.subjectType === lazy.YelpSubjectType.SERVICE
    ) {
      payload.titleL10n = {
        id: "firefox-suggest-yelp-service-title",
        args: {
          service: title,
        },
      };
    } else {
      payload.title = title;
    }

    return new lazy.UrlbarResult({
      ...resultProperties,
      payload,
    });
  }


  getResultCommands() {
    let commands = [
      {
        name: RESULT_MENU_COMMAND.INACCURATE_LOCATION,
        l10n: {
          id: "urlbar-result-menu-report-inaccurate-location2",
        },
      },
    ];

    if (this.canShowLessFrequently) {
      commands.push({
        name: RESULT_MENU_COMMAND.SHOW_LESS_FREQUENTLY,
        l10n: {
          id: "urlbar-result-menu-show-less-frequently2",
        },
      });
    }

    commands.push(
      {
        name: RESULT_MENU_COMMAND.DISMISS,
        l10n: {
          id: "urlbar-result-menu-dismiss-suggestion2",
        },
      },
      { name: "separator" },
      {
        name: RESULT_MENU_COMMAND.MANAGE,
        l10n: {
          id: "urlbar-result-menu-manage-firefox-suggest2",
        },
      },
      {
        name: RESULT_MENU_COMMAND.HELP,
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
      case RESULT_MENU_COMMAND.HELP:
      case RESULT_MENU_COMMAND.MANAGE:
        break;
      case RESULT_MENU_COMMAND.INACCURATE_LOCATION:
        controller.view.acknowledgeFeedback(result);
        break;
      case RESULT_MENU_COMMAND.DISMISS:
        lazy.QuickSuggest.dismissResult(result);
        result.acknowledgeDismissalL10n = {
          id: "firefox-suggest-dismissal-acknowledgment-one-yelp",
        };
        controller.removeResult(result);
        break;
      case RESULT_MENU_COMMAND.NOT_INTERESTED:
        lazy.UrlbarPrefs.set("suggest.yelp", false);
        result.acknowledgeDismissalL10n = {
          id: "firefox-suggest-dismissal-acknowledgment-all-yelp",
        };
        controller.removeResult(result);
        break;
      case RESULT_MENU_COMMAND.SHOW_LESS_FREQUENTLY:
        this.handleShowLessFrequently(controller, result);
        lazy.UrlbarPrefs.set("yelp.minKeywordLength", searchString.length + 1);
        break;
    }
  }

  incrementShowLessFrequentlyCount() {
    if (this.canShowLessFrequently) {
      lazy.UrlbarPrefs.set(
        "yelp.showLessFrequentlyCount",
        this.showLessFrequentlyCount + 1
      );
    }
  }

  get #minKeywordLength() {
    let hasUserValue = Services.prefs.prefHasUserValue(
      "browser.urlbar.yelp.minKeywordLength"
    );
    let nimbusValue = lazy.UrlbarPrefs.get("yelpMinKeywordLength");
    let minLength =
      hasUserValue || nimbusValue === null
        ? lazy.UrlbarPrefs.get("yelp.minKeywordLength")
        : nimbusValue;
    return Math.max(minLength, 0);
  }

  #normalizeRustSuggestion(suggestion) {

    let url = new URL(suggestion.url);
    let loc = url.searchParams.get(suggestion.locationParam);
    if (loc) {
      url.searchParams.delete(suggestion.locationParam);
      suggestion.url = url.toString();
      suggestion.city = loc;

      if (suggestion.title.endsWith(loc)) {
        suggestion.title = suggestion.title
          .substring(0, suggestion.title.length - loc.length)
          .trimEnd();
      }
    }

    return suggestion;
  }

  #normalizeMlSuggestion(ml) {
    if (!ml.subject) {
      return null;
    }

    let url = new URL(this.#metadataCache.urlOrigin);
    url.pathname = this.#metadataCache.urlPathname;
    url.searchParams.set(this.#metadataCache.findDesc, ml.subject);

    return {
      ...ml,
      title: ml.subject,
      url: url.toString(),
      subjectExactMatch: false,
      hasLocationSign: false,
      locationParam: this.#metadataCache.findLoc,
      icon_blob: this.#metadataCache.iconBlob,
      score: this.#metadataCache.score,
      city: ml.location?.city,
      region: ml.location?.state,
    };
  }

  async #makeMetadataCache() {
    let cache;

    this.logger.debug("Querying Rust backend to populate metadata cache");
    let rs = await lazy.QuickSuggest.rustBackend.query("coffee in atlanta", {
      types: ["Yelp"],
    });
    if (!rs.length) {
      this.logger.debug("Rust didn't return any Yelp suggestions!");
      cache = {};
    } else {
      let suggestion = rs[0];
      let url = new URL(suggestion.url);
      let findParamWithValue = value => {
        let tuple = [...url.searchParams.entries()].find(
          ([_, v]) => v == value
        );
        return tuple?.[0];
      };
      cache = {
        iconBlob: suggestion.icon_blob,
        score: suggestion.score,
        urlOrigin: url.origin,
        urlPathname: url.pathname,
        findDesc: findParamWithValue("coffee"),
        findLoc: findParamWithValue("atlanta"),
      };
    }

    let defaults = {
      urlOrigin: "https://www.yelp.com",
      urlPathname: "/search",
      findDesc: "find_desc",
      findLoc: "find_loc",
      score: 0.25,
    };
    for (let [key, value] of Object.entries(defaults)) {
      if (cache[key] === undefined) {
        cache[key] = value;
      }
    }

    return cache;
  }

  async #bestCityRegion(city, region) {
    let regionMatches;
    if (region) {
      regionMatches = await lazy.QuickSuggest.rustBackend.fetchGeonames(
        region,
        false, 
        null 
      );
      if (!regionMatches.length) {
        return null;
      }
    }

    if (city) {
      let cityMatches = await lazy.QuickSuggest.rustBackend.fetchGeonames(
        city,
        true, 
        regionMatches?.map(m => m.geoname)
      );
      cityMatches = cityMatches.filter(
        match => match.matchType == lazy.GeonameMatchType.NAME || !match.prefix
      );
      if (!cityMatches.length) {
        return null;
      }

      let best = await lazy.GeolocationUtils.best(
        cityMatches,
        locationFromGeonameMatch
      );
      return {
        city: best.geoname.name,
        region: best.geoname.adminDivisionCodes.get(1),
      };
    }

    regionMatches = regionMatches?.filter(
      match => match.matchType == lazy.GeonameMatchType.NAME
    );
    if (regionMatches?.length) {
      let best = await lazy.GeolocationUtils.best(
        regionMatches,
        locationFromGeonameMatch
      );
      return { city: null, region: best.geoname.name };
    }

    return null;
  }

  _test_invalidateMetadataCache() {
    this.#metadataCache = null;
  }

  #metadataCache = null;
}

function locationFromGeonameMatch(match) {
  return {
    latitude: match.geoname.latitude,
    longitude: match.geoname.longitude,
    country: match.geoname.countryCode,
    region: match.geoname.adminDivisionCodes.get(1),
    population: match.geoname.population,
  };
}
