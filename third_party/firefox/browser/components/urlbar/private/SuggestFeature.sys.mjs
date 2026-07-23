/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* eslint-disable no-unused-vars */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  QuickSuggest: "moz-src:///browser/components/urlbar/QuickSuggest.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

export class SuggestFeature {

  get enablingPreferences() {
    return [];
  }

  get primaryUserControlledPreferences() {
    return [];
  }

  get shouldEnable() {
    return this.enablingPreferences.every(p => lazy.UrlbarPrefs.get(p));
  }

  enable(enabled) {}


  get logger() {
    if (!this._logger) {
      this._logger = lazy.UrlbarShared.getLogger({
        prefix: `QuickSuggest.${this.name}`,
      });
    }
    return this._logger;
  }

  get isEnabled() {
    return this.#isEnabled;
  }

  get name() {
    return this.constructor.name;
  }

  update() {
    let enable =
      lazy.UrlbarPrefs.get("quickSuggestEnabled") && this.shouldEnable;
    if (enable != this.isEnabled) {
      this.logger.info("Feature enabled status changed", {
        nowEnabled: enable,
      });
      this.#isEnabled = enable;
      this.enable(enable);
    }
  }

  #isEnabled = false;
}

export class SuggestProvider extends SuggestFeature {

  get merinoProvider() {
    return "";
  }

  get rustSuggestionType() {
    return "";
  }

  get dynamicRustSuggestionTypes() {
    return [];
  }

  get rustProviderConstraints() {
    if (this.dynamicRustSuggestionTypes?.length) {
      return {
        dynamicSuggestionTypes: this.dynamicRustSuggestionTypes,
      };
    }
    return null;
  }

  get mlIntent() {
    return "";
  }

  get isMlIntentEnabled() {
    return false;
  }

  getSuggestionTelemetryType(suggestion) {
    return this.merinoProvider;
  }

  getResultCommands(_result) {
    return undefined;
  }

  get canShowLessFrequently() {
    return false;
  }

  incrementShowLessFrequentlyCount() {}

  isSuggestionSponsored(suggestion) {
    return false;
  }

  async filterSuggestions(suggestions) {
    return suggestions;
  }

  async makeResult(queryContext, suggestion, searchString) {
    return null;
  }

  onImpression(state, queryContext, controller, featureResults, details) {}

  onEngagement(queryContext, controller, details, searchString) {}

  isUrlEquivalentToResultUrl(url, result) {
    return url == result.payload.url;
  }


  handleShowLessFrequently(controller, result) {
    controller.view.acknowledgeFeedback(result);
    this.incrementShowLessFrequentlyCount();
    if (!this.canShowLessFrequently) {
      result.commands = this.getResultCommands(result);
      controller.view.invalidateResultMenuCommands();
    }
  }

  update() {
    super.update();
    lazy.QuickSuggest.rustBackend?.ingestEnabledSuggestions(this);
  }
}

export class SuggestBackend extends SuggestFeature {

  async query(searchString, { queryContext, types = null } = {}) {
    throw new Error("Trying to access the base class, must be overridden");
  }

  cancelQuery() {}

  onSearchSessionEnd(queryContext, controller, details) {}
}
