/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import {
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  UrlbarTokenizer:
    "moz-src:///browser/components/urlbar/UrlbarTokenizer.sys.mjs",
});

const RESTRICT_KEYWORDS_FEATURE_GATE = "searchRestrictKeywords.featureGate";

export class UrlbarProviderRestrictKeywords extends UrlbarProvider {
  constructor() {
    super();
  }

  get type() {
    return UrlbarUtils.PROVIDER_TYPE.HEURISTIC;
  }

  getPriority() {
    return 1;
  }

  async isActive(queryContext) {
    if (!lazy.UrlbarPrefs.get(RESTRICT_KEYWORDS_FEATURE_GATE)) {
      return false;
    }

    return (
      !queryContext.restrictInSearchMode() &&
      queryContext.trimmedSearchString == "@"
    );
  }

  async startQuery(queryContext, addCallback) {
    let instance = this.queryInstance;
    let tokenToKeyword = await lazy.UrlbarTokenizer.getL10nRestrictKeywords();

    if (instance != this.queryInstance) {
      return;
    }

    for (const [token, l10nRestrictKeywords] of tokenToKeyword.entries()) {
      let icon = UrlbarUtils.LOCAL_SEARCH_MODES.find(
        mode => mode.restrict == token
      )?.icon;

      let result = new lazy.UrlbarResult({
        type: lazy.UrlbarShared.RESULT_TYPE.RESTRICT,
        source: lazy.UrlbarShared.RESULT_SOURCE.OTHER_LOCAL,
        hideRowLabel: true,
        payload: {
          icon,
          keyword: token,
          l10nRestrictKeywords,
          providesSearchMode: true,
        },
        highlights: {
          l10nRestrictKeywords: UrlbarUtils.HIGHLIGHT.TYPED,
        },
      });
      addCallback(this, result);
    }
  }
}
