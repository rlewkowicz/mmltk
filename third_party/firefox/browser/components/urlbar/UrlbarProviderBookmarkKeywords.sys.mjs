/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import {
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  KeywordUtils: "resource://gre/modules/KeywordUtils.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

export class UrlbarProviderBookmarkKeywords extends UrlbarProvider {
  get type() {
    return UrlbarUtils.PROVIDER_TYPE.HEURISTIC;
  }

  async isActive(queryContext) {
    return (
      (!queryContext.restrictSource ||
        queryContext.restrictSource ==
          lazy.UrlbarShared.RESULT_SOURCE.BOOKMARKS) &&
      !queryContext.restrictInSearchMode() &&
      !!queryContext.tokens.length
    );
  }

  async startQuery(queryContext, addCallback) {
    let keyword = queryContext.tokens[0]?.value;

    let searchString = UrlbarUtils.substringAfter(
      queryContext.searchString,
      keyword
    ).trim();
    let { entry, url, postData } = await lazy.KeywordUtils.getBindableKeyword(
      keyword,
      searchString
    );
    if (!entry || !url) {
      return;
    }

    let title;
    if (entry.url.host && searchString) {
      title = UrlbarUtils.strings.formatStringFromName(
        "bookmarkKeywordSearch",
        [
          entry.url.host,
          queryContext.tokens
            .slice(1)
            .map(t => t.value)
            .join(" "),
        ]
      );
    } else {
      title = UrlbarUtils.prepareUrlForDisplay(url);
    }

    let result = new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.KEYWORD,
      source: lazy.UrlbarShared.RESULT_SOURCE.BOOKMARKS,
      heuristic: true,
      payload: {
        title,
        url,
        keyword,
        input: queryContext.searchString,
        postData,
        icon: UrlbarUtils.getIconForUrl(entry.url),
      },
      highlights: {
        title: UrlbarUtils.HIGHLIGHT.TYPED,
        url: UrlbarUtils.HIGHLIGHT.TYPED,
        keyword: UrlbarUtils.HIGHLIGHT.TYPED,
      },
    });
    addCallback(this, result);
  }
}
