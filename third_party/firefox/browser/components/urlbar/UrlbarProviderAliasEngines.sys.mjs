/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import {
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarSearchUtils:
    "moz-src:///browser/components/urlbar/UrlbarSearchUtils.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

export class UrlbarProviderAliasEngines extends UrlbarProvider {
  get type() {
    return UrlbarUtils.PROVIDER_TYPE.HEURISTIC;
  }

  async isActive(queryContext) {
    return (
      (!queryContext.restrictSource ||
        queryContext.restrictSource ==
          lazy.UrlbarShared.RESULT_SOURCE.SEARCH) &&
      !queryContext.restrictInSearchMode() &&
      !!queryContext.tokens.length
    );
  }

  async startQuery(queryContext, addCallback) {
    let instance = this.queryInstance;
    let alias = queryContext.tokens[0]?.value;
    let engine = await lazy.UrlbarSearchUtils.engineForAlias(
      alias,
      queryContext.searchString
    );
    let icon = await engine?.getIconURL();
    if (!engine || instance != this.queryInstance) {
      return;
    }
    let query = UrlbarUtils.substringAfter(
      queryContext.searchString,
      alias
    ).trimStart();
    let result = new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.SEARCH,
      source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
      heuristic: true,
      payload: {
        engine: engine.name,
        keyword: alias,
        query,
        title: query,
        icon,
      },
    });
    addCallback(this, result);
  }
}
