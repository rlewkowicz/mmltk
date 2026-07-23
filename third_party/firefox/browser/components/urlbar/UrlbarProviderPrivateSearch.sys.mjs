/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import {
  SkippableTimer,
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarSearchUtils:
    "moz-src:///browser/components/urlbar/UrlbarSearchUtils.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

export class UrlbarProviderPrivateSearch extends UrlbarProvider {
  constructor() {
    super();
  }

  get type() {
    return UrlbarUtils.PROVIDER_TYPE.PROFILE;
  }

  async isActive(queryContext) {
    return (
      lazy.UrlbarSearchUtils.separatePrivateDefaultUIEnabled &&
      !queryContext.isPrivate &&
      !!queryContext.tokens.length
    );
  }

  async startQuery(queryContext, addCallback) {
    let searchString = queryContext.trimmedSearchString;
    if (
      queryContext.tokens.some(
        t => t.type == lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_SEARCH
      )
    ) {
      if (queryContext.tokens.length == 1) {
        return;
      }
      searchString = queryContext.tokens
        .filter(t => t.type != lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_SEARCH)
        .map(t => t.value)
        .join(" ");
    }

    let instance = this.queryInstance;

    let engine = queryContext.searchMode?.engineName
      ? lazy.SearchService.getEngineByName(queryContext.searchMode.engineName)
      : await lazy.SearchService.getDefaultPrivate();
    let isPrivateEngine =
      lazy.UrlbarSearchUtils.separatePrivateDefault &&
      engine != (await lazy.SearchService.getDefault());
    this.logger.info(`isPrivateEngine: ${isPrivateEngine}`);

    await new SkippableTimer({
      name: "ProviderPrivateSearch",
      time: 100,
      logger: this.logger,
    }).promise;

    let icon = await engine.getIconURL();
    if (instance != this.queryInstance) {
      return;
    }

    let result = new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.SEARCH,
      source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
      suggestedIndex: 1,
      payload: {
        engine: engine.name,
        query: searchString,
        title: searchString,
        icon,
        inPrivateWindow: true,
        isPrivateEngine,
      },
      highlights: {
        engine: UrlbarUtils.HIGHLIGHT.TYPED,
      },
    });
    addCallback(this, result);
  }
}
