/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { SuggestProvider } from "moz-src:///browser/components/urlbar/private/SuggestFeature.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  QuickSuggest: "moz-src:///browser/components/urlbar/QuickSuggest.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarSearchUtils:
    "moz-src:///browser/components/urlbar/UrlbarSearchUtils.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

export class RealtimeSuggestProvider extends SuggestProvider {

  get realtimeType() {
    throw new Error("Trying to access the base class, must be overridden");
  }

  getViewTemplateForDescriptionTop(_item, _index) {
    throw new Error("Trying to access the base class, must be overridden");
  }

  getViewTemplateForDescriptionBottom(_item, _index) {
    throw new Error("Trying to access the base class, must be overridden");
  }

  getViewUpdateForPayloadItem(_item, _index) {
    throw new Error("Trying to access the base class, must be overridden");
  }


  get dynamicRustSuggestionTypes() {
    return [this.realtimeType + "_opt_in"];
  }

  get merinoProvider() {
    return this.realtimeType;
  }

  get baseTelemetryType() {
    return this.realtimeType;
  }

  get realtimeTypeForFtl() {
    return this.realtimeType.replace(/([A-Z])/g, "-$1").toLowerCase();
  }

  get featureGatePref() {
    return this.realtimeType + "FeatureGate";
  }

  get suggestPref() {
    return "suggest." + this.realtimeType;
  }

  get minKeywordLengthPref() {
    return this.realtimeType + ".minKeywordLength";
  }

  get showLessFrequentlyCountPref() {
    return this.realtimeType + ".showLessFrequentlyCount";
  }

  get optInIcon() {
    return `chrome://browser/skin/illustrations/${this.realtimeType}-opt-in.svg`;
  }

  get optInTitleL10n() {
    return {
      id: `urlbar-result-${this.realtimeTypeForFtl}-opt-in-title`,
    };
  }

  get optInDescriptionL10n() {
    return {
      id: `urlbar-result-${this.realtimeTypeForFtl}-opt-in-description`,
      parseMarkup: true,
    };
  }

  get notInterestedCommandL10n() {
    return {
      id: `urlbar-result-menu-dont-show-${this.realtimeTypeForFtl}2`,
    };
  }

  get acknowledgeDismissalL10n() {
    return {
      id: "urlbar-result-dismissal-acknowledgment-" + this.realtimeTypeForFtl,
    };
  }

  get ariaGroupL10n() {
    return {
      id: "urlbar-result-aria-group-" + this.realtimeTypeForFtl,
      attribute: "aria-label",
    };
  }

  get isSponsored() {
    return false;
  }

  get dynamicResultType() {
    return "realtime-" + this.realtimeType;
  }


  get rustSuggestionType() {
    return "Dynamic";
  }

  get enablingPreferences() {
    return [
      "suggest.quicksuggest.all",
      "suggest.realtimeOptIn",
      "quicksuggest.realtimeOptIn.dismissTypes",
      "quicksuggest.realtimeOptIn.notNowTimeSeconds",
      "quicksuggest.realtimeOptIn.notNowReshowAfterPeriodDays",
      "quickSuggestOnlineAvailable",
      "quicksuggest.online.enabled",
      this.featureGatePref,
      this.suggestPref,

      "suggest.quicksuggest.sponsored",
    ];
  }

  get primaryUserControlledPreferences() {
    return [
      "suggest.realtimeOptIn",
      "quicksuggest.realtimeOptIn.dismissTypes",
      "quicksuggest.realtimeOptIn.notNowTimeSeconds",
      "quicksuggest.realtimeOptIn.notNowReshowAfterPeriodDays",
      this.suggestPref,
    ];
  }

  get shouldEnable() {
    if (
      !lazy.UrlbarPrefs.get(this.featureGatePref) ||
      !lazy.UrlbarPrefs.get("quickSuggestOnlineAvailable") ||
      !lazy.UrlbarPrefs.get("suggest.quicksuggest.all")
    ) {
      return false;
    }

    if (lazy.UrlbarPrefs.get("quicksuggest.online.enabled")) {
      return lazy.UrlbarPrefs.get(this.suggestPref);
    }

    if (!lazy.UrlbarPrefs.get("suggest.realtimeOptIn")) {
      return false;
    }

    let dismissTypes = lazy.UrlbarPrefs.get(
      "quicksuggest.realtimeOptIn.dismissTypes"
    );
    if (dismissTypes.has(this.realtimeType)) {
      return false;
    }

    let notNowTimeSeconds = lazy.UrlbarPrefs.get(
      "quicksuggest.realtimeOptIn.notNowTimeSeconds"
    );
    if (!notNowTimeSeconds) {
      return true;
    }

    let notNowReshowAfterPeriodDays = lazy.UrlbarPrefs.get(
      "quicksuggest.realtimeOptIn.notNowReshowAfterPeriodDays"
    );

    let timeSecs = notNowReshowAfterPeriodDays * 24 * 60 * 60;
    return Date.now() / 1000 - notNowTimeSeconds > timeSecs;
  }

  isSuggestionSponsored(suggestion) {
    switch (suggestion.source) {
      case "merino":
        if (suggestion.hasOwnProperty("is_sponsored")) {
          return !!suggestion.is_sponsored;
        }
        break;
      case "rust":
        if (suggestion.data?.result?.payload?.hasOwnProperty("isSponsored")) {
          return suggestion.data.result.payload.isSponsored;
        }
        break;
    }
    return this.isSponsored;
  }

  getSuggestionTelemetryType(suggestion) {
    switch (suggestion.source) {
      case "merino":
        if (suggestion.hasOwnProperty("telemetry_type")) {
          return suggestion.telemetry_type;
        }
        break;
      case "rust":
        if (suggestion.data?.result?.payload?.hasOwnProperty("telemetryType")) {
          return suggestion.data.result.payload.telemetryType;
        }
        return this.baseTelemetryType + "_opt_in";
    }
    return this.baseTelemetryType;
  }

  filterSuggestions(suggestions) {
    if (lazy.UrlbarPrefs.get("quicksuggest.online.enabled")) {
      return suggestions.filter(s => s.source == "merino");
    }
    return suggestions;
  }

  makeResult(queryContext, suggestion, searchString) {
    if (
      !lazy.UrlbarPrefs.get("suggest.quicksuggest.all") ||
      (this.isSuggestionSponsored(suggestion) &&
        !lazy.UrlbarPrefs.get("suggest.quicksuggest.sponsored"))
    ) {
      return null;
    }

    switch (suggestion.source) {
      case "merino":
        return this.makeMerinoResult(queryContext, suggestion, searchString);
      case "rust":
        return this.makeOptInResult(queryContext, suggestion);
    }
    return null;
  }

  makeMerinoResult(
    queryContext,
    suggestion,
    searchString,
    additionalOptions = {}
  ) {
    if (!this.isEnabled) {
      return null;
    }

    if (
      this.showLessFrequentlyCount &&
      searchString.length < this.#minKeywordLength
    ) {
      return null;
    }

    let values = suggestion.custom_details?.[this.merinoProvider]?.values;
    if (!values?.length) {
      return null;
    }

    let engine;
    if (values.some(v => v.query)) {
      engine = lazy.UrlbarSearchUtils.getDefaultEngine(queryContext.isPrivate);
      if (!engine) {
        return null;
      }
    }

    let result = new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.DYNAMIC,
      source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
      isBestMatch: true,
      ...additionalOptions,
      payload: {
        items: values.map((v, i) => this.makePayloadItem(v, i)),
        dynamicType: this.dynamicResultType,
        engine: engine?.name,
      },
    });

    return result;
  }

  makePayloadItem(value, _index) {
    return value;
  }

  makeOptInResult(queryContext, _suggestion) {
    let notNowTypes = lazy.UrlbarPrefs.get(
      "quicksuggest.realtimeOptIn.notNowTypes"
    );
    let splitButtonMain = notNowTypes.has(this.realtimeType)
      ? {
          command: "dismiss",
          l10n: {
            id: "urlbar-result-realtime-opt-in-dismiss",
          },
        }
      : {
          command: "not_now",
          l10n: {
            id: "urlbar-result-realtime-opt-in-not-now",
          },
        };

    return new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.TIP,
      source: lazy.UrlbarShared.RESULT_SOURCE.OTHER_LOCAL,
      isBestMatch: true,
      payload: {
        type: "realtime_opt_in",
        icon: this.optInIcon,
        titleL10n: this.optInTitleL10n,
        descriptionL10n: this.optInDescriptionL10n,
        descriptionLearnMoreTopic: lazy.QuickSuggest.HELP_TOPIC,
        buttons: [
          {
            command: "opt_in",
            l10n: {
              id: "urlbar-result-realtime-opt-in-allow",
            },
            input: queryContext.searchString,
            attributes: {
              primary: "",
            },
          },
          {
            ...splitButtonMain,
            menu: [
              {
                name: "not_interested",
                l10n: {
                  id: "urlbar-result-realtime-opt-in-dismiss-all2",
                },
              },
            ],
          },
        ],
      },
    });
  }

  getViewTemplate(result) {
    let { items } = result.payload;
    let hasMultipleItems = items.length > 1;
    return {
      name: "root",
      overflowable: true,
      attributes: {
        selectable: hasMultipleItems ? null : "",
        role: hasMultipleItems ? "group" : "option",
      },
      classList: ["urlbarView-realtime-root"],
      dataset: {
        url: items[0].url,
        query: items[0].query,
      },
      children: items.map((item, i) => ({
        name: `item_${i}`,
        tag: "span",
        classList: ["urlbarView-realtime-item"],
        attributes: {
          selectable: !hasMultipleItems ? null : "",
          role: hasMultipleItems ? "option" : "presentation",
        },
        dataset: {
          url: item.url,
          query: item.query,
        },
        children: [
          ...this.getViewTemplateForImageContainer(item, i),
          {
            tag: "span",
            classList: ["urlbarView-realtime-description"],
            children: [
              {
                tag: "div",
                classList: ["urlbarView-realtime-description-top"],
                children: this.getViewTemplateForDescriptionTop(item, i),
              },
              {
                tag: "div",
                classList: ["urlbarView-realtime-description-bottom"],
                children: this.getViewTemplateForDescriptionBottom(item, i),
              },
            ],
          },
        ],
      })),
    };
  }

  getViewTemplateForImageContainer(_item, index) {
    return [
      {
        name: `image_container_${index}`,
        tag: "span",
        classList: ["urlbarView-realtime-image-container"],
        children: [
          {
            name: `image_${index}`,
            tag: "img",
            classList: ["urlbarView-realtime-image"],
          },
        ],
      },
    ];
  }

  getViewUpdate(result) {
    let { items } = result.payload;
    let hasMultipleItems = items.length > 1;

    let update = {
      root: {
        l10n: hasMultipleItems ? this.ariaGroupL10n : null,
      },
    };

    for (let i = 0; i < items.length; i++) {
      let item = items[i];
      Object.assign(update, this.getViewUpdateForPayloadItem(item, i));
    }

    return update;
  }

  getResultCommands(result) {
    if (result.payload.source == "rust") {
      return null;
    }

    let commands = [];

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
        name: "not_interested",
        l10n: this.notInterestedCommandL10n,
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
    switch (details.result.payload.source) {
      case "merino":
        this.onMerinoEngagement(
          queryContext,
          controller,
          details,
          searchString
        );
        break;
      case "rust":
        this.onOptInEngagement(queryContext, controller, details, searchString);
        break;
    }
  }

  onMerinoEngagement(queryContext, controller, details, searchString) {
    let { result } = details;
    switch (details.selType) {
      case "help":
      case "manage": {
        break;
      }
      case "not_interested": {
        lazy.UrlbarPrefs.set(this.suggestPref, false);
        result.acknowledgeDismissalL10n = this.acknowledgeDismissalL10n;
        controller.removeResult(result);
        break;
      }
      case "show_less_frequently": {
        this.handleShowLessFrequently(controller, result);
        lazy.UrlbarPrefs.set(
          this.minKeywordLengthPref,
          searchString.length + 1
        );
        break;
      }
    }
  }

  onOptInEngagement(queryContext, controller, details, _searchString) {
    switch (details.selType) {
      case "opt_in":
        lazy.UrlbarPrefs.set("quicksuggest.online.enabled", true);
        controller.input.startQuery({ allowAutofill: false });
        break;
      case "not_now": {
        lazy.UrlbarPrefs.set(
          "quicksuggest.realtimeOptIn.notNowTimeSeconds",
          Date.now() / 1000
        );
        lazy.UrlbarPrefs.add(
          "quicksuggest.realtimeOptIn.notNowTypes",
          this.realtimeType
        );
        controller.removeResult(details.result);
        break;
      }
      case "dismiss": {
        lazy.UrlbarPrefs.add(
          "quicksuggest.realtimeOptIn.dismissTypes",
          this.realtimeType
        );
        details.result.acknowledgeDismissalL10n = this.acknowledgeDismissalL10n;
        controller.removeResult(details.result);
        break;
      }
      case "not_interested": {
        lazy.UrlbarPrefs.set("suggest.realtimeOptIn", false);
        details.result.acknowledgeDismissalL10n = {
          id: "urlbar-result-dismissal-acknowledgment-all",
        };
        controller.removeResult(details.result);
        break;
      }
    }
  }

  incrementShowLessFrequentlyCount() {
    if (this.canShowLessFrequently) {
      lazy.UrlbarPrefs.set(
        this.showLessFrequentlyCountPref,
        this.showLessFrequentlyCount + 1
      );
    }
  }

  get showLessFrequentlyCount() {
    const pref = this.showLessFrequentlyCountPref;
    const count = lazy.UrlbarPrefs.get(pref) || 0;
    return Math.max(count, 0);
  }

  get canShowLessFrequently() {
    const cap =
      lazy.UrlbarPrefs.get("realtimeShowLessFrequentlyCap") ||
      lazy.QuickSuggest.config.showLessFrequentlyCap ||
      0;
    return !cap || this.showLessFrequentlyCount < cap;
  }

  get #minKeywordLength() {
    let hasUserValue = Services.prefs.prefHasUserValue(
      "browser.urlbar." + this.minKeywordLengthPref
    );
    let nimbusValue = lazy.UrlbarPrefs.get("realtimeMinKeywordLength");
    let minLength =
      hasUserValue || nimbusValue === null
        ? lazy.UrlbarPrefs.get(this.minKeywordLengthPref)
        : nimbusValue;
    return Math.max(minLength, 0);
  }
}
