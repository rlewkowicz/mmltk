/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { SuggestProvider } from "moz-src:///browser/components/urlbar/private/SuggestFeature.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  GeolocationUtils:
    "moz-src:///browser/components/urlbar/private/GeolocationUtils.sys.mjs",
  MerinoClient: "moz-src:///browser/components/urlbar/MerinoClient.sys.mjs",
  QuickSuggest: "moz-src:///browser/components/urlbar/QuickSuggest.sys.mjs",
  Region: "resource://gre/modules/Region.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

const MERINO_TIMEOUT_MS = 5000; 

const MERINO_WEATHER_CACHE_PERIOD_MS = 60000; 

const RESULT_MENU_COMMAND = {
  DISMISS: "dismiss",
  HELP: "help",
  INACCURATE_LOCATION: "inaccurate_location",
  MANAGE: "manage",
  SHOW_LESS_FREQUENTLY: "show_less_frequently",
};

const WEATHER_PROVIDER_DISPLAY_NAME = "AccuWeather®";

const NORTH_AMERICA_COUNTRY_CODES = new Set(["CA", "US"]);

const WEATHER_DYNAMIC_TYPE = "weather";
const WEATHER_VIEW_TEMPLATE = {
  attributes: {
    selectable: true,
  },
  children: [
    {
      name: "currentConditions",
      tag: "span",
      children: [
        {
          name: "currently",
          tag: "div",
        },
        {
          name: "currentTemperature",
          tag: "div",
          children: [
            {
              name: "temperature",
              tag: "span",
            },
            {
              name: "weatherIcon",
              tag: "img",
            },
          ],
        },
      ],
    },
    {
      name: "summary",
      tag: "span",
      overflowable: true,
      children: [
        {
          name: "top",
          tag: "div",
          children: [
            {
              name: "topNoWrap",
              tag: "span",
              children: [
                { name: "title", tag: "span", classList: ["urlbarView-title"] },
                {
                  name: "titleSeparator",
                  tag: "span",
                  classList: ["urlbarView-title-separator"],
                },
              ],
            },
            {
              name: "url",
              tag: "span",
              classList: ["urlbarView-url"],
            },
          ],
        },
        {
          name: "middle",
          tag: "div",
          children: [
            {
              name: "middleNoWrap",
              tag: "span",
              overflowable: true,
              children: [
                {
                  name: "summaryText",
                  tag: "span",
                },
                {
                  name: "summaryTextSeparator",
                  tag: "span",
                },
                {
                  name: "highLow",
                  tag: "span",
                },
              ],
            },
            {
              name: "highLowWrap",
              tag: "span",
            },
          ],
        },
        {
          name: "bottom",
          tag: "div",
        },
      ],
    },
  ],
};

export class WeatherSuggestions extends SuggestProvider {
  constructor() {
    super();
  }

  get enablingPreferences() {
    return [
      "weatherFeatureGate",
      "suggest.weather",
      "suggest.quicksuggest.all",
      "suggest.quicksuggest.sponsored",
    ];
  }

  get primaryUserControlledPreferences() {
    return ["suggest.weather"];
  }

  get rustSuggestionType() {
    return "Weather";
  }

  get showLessFrequentlyCount() {
    const count = lazy.UrlbarPrefs.get("weather.showLessFrequentlyCount") || 0;
    return Math.max(count, 0);
  }

  get canShowLessFrequently() {
    const cap =
      lazy.UrlbarPrefs.get("weatherShowLessFrequentlyCap") ||
      lazy.QuickSuggest.config.showLessFrequentlyCap ||
      0;
    return !cap || this.showLessFrequentlyCount < cap;
  }

  isSuggestionSponsored(_suggestion) {
    return true;
  }

  getSuggestionTelemetryType() {
    return "weather";
  }

  enable(enabled) {
    if (!enabled) {
      this.#merino = null;
    }
  }

  async filterSuggestions(suggestions) {
    if (suggestions.length <= 1) {
      return suggestions;
    }

    let suggestion = await lazy.GeolocationUtils.best(suggestions, s => ({
      latitude: s.city?.latitude,
      longitude: s.city?.longitude,
      country: s.city?.countryCode,
      region: s.city?.adminDivisionCodes.get(1),
      population: s.city?.population,
    }));

    return [suggestion];
  }

  async makeResult(queryContext, suggestion, searchString) {
    if (searchString.length < this.#minKeywordLength) {
      return null;
    }

    let merinoSuggestion = await this.#fetchMerinoSuggestion(suggestion.city);
    if (!merinoSuggestion) {
      return null;
    }

    let unit = Services.locale.regionalPrefsLocales[0] == "en-US" ? "f" : "c";

    let treatment = lazy.UrlbarPrefs.get("weatherUiTreatment");
    if (treatment == 1 || treatment == 2) {
      return this.#makeDynamicResult(merinoSuggestion, unit);
    }

    let titleL10n = await this.#getTitleL10n(suggestion.city, merinoSuggestion);

    return new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.URL,
      source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
      isBestMatch: true,
      suggestedIndex: 1,
      isRichSuggestion: true,
      richSuggestionIconVariation: String(
        merinoSuggestion.current_conditions.icon_id
      ),
      payload: {
        url: merinoSuggestion.url,
        titleL10n: {
          id: titleL10n.id,
          args: {
            temperature: merinoSuggestion.current_conditions.temperature[unit],
            unit: unit.toUpperCase(),
            ...titleL10n.args,
          },
          parseMarkup: true,
        },
        bottomTextL10n: {
          id: "urlbar-result-weather-provider-sponsored",
          args: { provider: WEATHER_PROVIDER_DISPLAY_NAME },
        },
        helpUrl: lazy.QuickSuggest.HELP_URL,
      },
    });
  }

  #makeDynamicResult(suggestion, unit) {
    return new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.DYNAMIC,
      source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
      showFeedbackMenu: true,
      suggestedIndex: 1,
      payload: {
        url: suggestion.url,
        input: suggestion.url,
        iconId: suggestion.current_conditions.icon_id,
        dynamicType: WEATHER_DYNAMIC_TYPE,
        city: suggestion.city_name,
        region: suggestion.region_code,
        temperatureUnit: unit,
        temperature: suggestion.current_conditions.temperature[unit],
        currentConditions: suggestion.current_conditions.summary,
        forecast: suggestion.forecast.summary,
        high: suggestion.forecast.high[unit],
        low: suggestion.forecast.low[unit],
        helpUrl: lazy.QuickSuggest.HELP_URL,
      },
    });
  }

  getViewTemplate(_result) {
    return WEATHER_VIEW_TEMPLATE;
  }

  getViewUpdate(result) {
    let useSimplerUi = lazy.UrlbarPrefs.get("weatherUiTreatment") == 1;
    let uppercaseUnit = result.payload.temperatureUnit.toUpperCase();
    return {
      currently: {
        l10n: {
          id: "firefox-suggest-weather-currently",
        },
      },
      temperature: {
        l10n: {
          id: "firefox-suggest-weather-temperature",
          args: {
            value: result.payload.temperature,
            unit: uppercaseUnit,
          },
        },
      },
      weatherIcon: {
        attributes: { "icon-variation": result.payload.iconId },
      },
      title: {
        l10n: {
          id: "firefox-suggest-weather-title",
          args: { city: result.payload.city, region: result.payload.region },
        },
      },
      url: {
        textContent: result.payload.url,
      },
      summaryText: useSimplerUi
        ? { textContent: result.payload.currentConditions }
        : {
            l10n: {
              id: "firefox-suggest-weather-summary-text",
              args: {
                currentConditions: result.payload.currentConditions,
                forecast: result.payload.forecast,
              },
            },
          },
      highLow: {
        l10n: {
          id: "firefox-suggest-weather-high-low",
          args: {
            high: result.payload.high,
            low: result.payload.low,
            unit: uppercaseUnit,
          },
        },
      },
      highLowWrap: {
        l10n: {
          id: "firefox-suggest-weather-high-low",
          args: {
            high: result.payload.high,
            low: result.payload.low,
            unit: uppercaseUnit,
          },
        },
      },
      bottom: {
        l10n: {
          id: "urlbar-result-weather-provider-sponsored",
          args: { provider: WEATHER_PROVIDER_DISPLAY_NAME },
        },
      },
    };
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
          id: "urlbar-result-menu-dont-show-weather-suggestions2",
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
      case RESULT_MENU_COMMAND.DISMISS:
        this.logger.info("Dismissing weather result");
        lazy.UrlbarPrefs.set("suggest.weather", false);
        result.acknowledgeDismissalL10n = {
          id: "urlbar-dismissal-acknowledgment-weather",
        };
        controller.removeResult(result);
        break;
      case RESULT_MENU_COMMAND.INACCURATE_LOCATION:
        controller.view.acknowledgeFeedback(result);
        break;
      case RESULT_MENU_COMMAND.SHOW_LESS_FREQUENTLY:
        this.handleShowLessFrequently(controller, result);
        lazy.UrlbarPrefs.set(
          "weather.minKeywordLength",
          searchString.length + 1
        );
        break;
    }
  }

  incrementShowLessFrequentlyCount() {
    if (this.canShowLessFrequently) {
      lazy.UrlbarPrefs.set(
        "weather.showLessFrequentlyCount",
        this.showLessFrequentlyCount + 1
      );
    }
  }

  get #config() {
    let { rustBackend } = lazy.QuickSuggest;
    let config = rustBackend.isEnabled
      ? rustBackend.getConfigForSuggestionType(this.rustSuggestionType)
      : null;
    return config || {};
  }

  get #minKeywordLength() {
    let minLength = lazy.UrlbarPrefs.get("weather.minKeywordLength");
    if (
      !Services.prefs.prefHasUserValue(
        "browser.urlbar.weather.minKeywordLength"
      )
    ) {
      let nimbusValue = lazy.UrlbarPrefs.get("weatherKeywordsMinimumLength");
      if (nimbusValue !== null) {
        minLength = nimbusValue;
      } else if (!isNaN(this.#config.minKeywordLength)) {
        minLength = this.#config.minKeywordLength;
      }
    }
    return Math.max(minLength, 0);
  }

  async #fetchMerinoSuggestion(cityGeoname) {
    if (!this.#merino) {
      this.#merino = new lazy.MerinoClient(this.constructor.name, {
        allowOhttp: false,
        cachePeriodMs: MERINO_WEATHER_CACHE_PERIOD_MS,
      });
    }

    let merino = this.#merino;
    let fetchInstance = (this.#fetchInstance = {});
    let merinoSuggestion = await merino.fetchWeatherReport({
      source: "urlbar",
      country: cityGeoname?.countryCode,
      region: cityGeoname?.adminDivisionCodes
        ? [...cityGeoname.adminDivisionCodes.entries()]
            .sort(([level1, _admin1], [level2, _admin2]) => level1 - level2)
            .map(([_, admin]) => admin)
            .join(",")
        : undefined,
      city: cityGeoname?.name,
      timeoutMs: this.#timeoutMs,
    });

    if (fetchInstance != this.#fetchInstance || merino != this.#merino) {
      return null;
    }

    return merinoSuggestion;
  }

  async #getTitleL10n(cityGeoname, merinoSuggestion) {
    let displayCity = "";
    let displayRegion = "";
    let displayCountry = "";

    if (!cityGeoname) {
      displayCity = merinoSuggestion.city_name;
      displayRegion = merinoSuggestion.region_code;
    } else {
      let alts =
        await lazy.QuickSuggest.rustBackend.fetchGeonameAlternates(cityGeoname);

      displayCity = alts.geoname.localized || alts.geoname.primary;

      if (NORTH_AMERICA_COUNTRY_CODES.has(cityGeoname.countryCode)) {
        displayRegion =
          alts.adminDivisions.get(1)?.abbreviation ||
          alts.adminDivisions.get(1)?.localized ||
          alts.adminDivisions.get(1)?.primary;
      }

      if (cityGeoname.countryCode != lazy.Region.home) {
        displayCountry = alts.country?.localized || alts.country?.primary;
      }
    }

    if (displayRegion && displayCountry) {
      return {
        id: "urlbar-result-weather-title-with-country",
        args: {
          city: displayCity,
          region: displayRegion,
          country: displayCountry,
        },
      };
    }

    let region = displayRegion || displayCountry;
    if (region) {
      return {
        id: "urlbar-result-weather-title",
        args: {
          region,
          city: displayCity,
        },
      };
    }

    return {
      id: "urlbar-result-weather-title-city-only",
      args: {
        city: displayCity,
      },
    };
  }

  get _test_merino() {
    return this.#merino;
  }

  _test_setTimeoutMs(ms) {
    this.#timeoutMs = ms < 0 ? MERINO_TIMEOUT_MS : ms;
  }

  #fetchInstance = null;
  #merino = null;
  #timeoutMs = MERINO_TIMEOUT_MS;
}
