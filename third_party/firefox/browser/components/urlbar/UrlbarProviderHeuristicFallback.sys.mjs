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
  UrlbarSearchUtils:
    "moz-src:///browser/components/urlbar/UrlbarSearchUtils.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  UrlUtils: "resource://gre/modules/UrlUtils.sys.mjs",
});

export class UrlbarProviderHeuristicFallback extends UrlbarProvider {
  constructor() {
    super();
  }

  get type() {
    return UrlbarUtils.PROVIDER_TYPE.HEURISTIC;
  }

  async isActive(queryContext) {
    return !!queryContext.searchString.length;
  }

  getPriority() {
    return 0;
  }

  async startQuery(queryContext, addCallback) {
    let instance = this.queryInstance;

    if (queryContext.sapName != "searchbar") {
      let result =
        UrlbarProviderHeuristicFallback.matchUnknownUrl(queryContext);
      if (result) {
        addCallback(this, result);
        let str = queryContext.searchString;
        if (!URL.canParse(str)) {
          if (
            lazy.UrlbarPrefs.get("keyword.enabled") &&
            (lazy.UrlUtils.looksLikeOrigin(str, {
              noIp: true,
              noPort: true,
            }) ||
              lazy.UrlUtils.REGEXP_COMMON_EMAIL.test(str))
          ) {
            let searchResult = await this._engineSearchResult({ queryContext });
            if (instance != this.queryInstance) {
              return;
            }
            addCallback(this, searchResult);
          }
        }
        return;
      }
    }

    let result = await this._searchModeKeywordResult(queryContext);
    if (instance != this.queryInstance) {
      return;
    }
    if (result) {
      addCallback(this, result);
      return;
    }

    if (
      queryContext.sapName == "searchbar" ||
      lazy.UrlbarPrefs.get("keyword.enabled") ||
      queryContext.restrictSource == lazy.UrlbarShared.RESULT_SOURCE.SEARCH ||
      queryContext.searchMode
    ) {
      result = await this._engineSearchResult({
        queryContext,
        heuristic: true,
      });
      if (instance != this.queryInstance) {
        return;
      }
      if (result) {
        addCallback(this, result);
      }
    }
  }

  static matchUnknownUrl(queryContext) {
    if (
      queryContext.restrictSource == lazy.UrlbarShared.RESULT_SOURCE.SEARCH ||
      lazy.UrlbarShared.SEARCH_MODE_RESTRICT.has(
        queryContext.restrictToken?.value
      ) ||
      queryContext.searchMode
    ) {
      return null;
    }

    let unescapedSearchString = UrlbarUtils.unEscapeURIForUI(
      queryContext.searchString
    );
    let [prefix, suffix] = UrlbarUtils.stripURLPrefix(unescapedSearchString);
    if (!suffix && prefix) {
      return null;
    }

    let searchUrl = queryContext.trimmedSearchString;

    if (queryContext.fixupError) {
      if (
        queryContext.fixupError == Cr.NS_ERROR_MALFORMED_URI &&
        !lazy.UrlbarPrefs.get("keyword.enabled")
      ) {
        return new lazy.UrlbarResult({
          type: lazy.UrlbarShared.RESULT_TYPE.URL,
          source: lazy.UrlbarShared.RESULT_SOURCE.OTHER_LOCAL,
          heuristic: true,
          payload: {
            title: searchUrl,
            url: searchUrl,
          },
        });
      }

      return null;
    }

    if (!queryContext.fixupInfo?.href || queryContext.fixupInfo?.isSearch) {
      return null;
    }

    let uri = new URL(queryContext.fixupInfo.href);
    let hostExpected = ["http:", "https:", "ftp:", "chrome:"].includes(
      uri.protocol
    );
    if (hostExpected && !uri.host) {
      return null;
    }

    let escapedURL = uri.toString();
    let displayURL = UrlbarUtils.prepareUrlForDisplay(uri, {
      trimURL: false,
      schemeless: !prefix,
    });

    let iconUri;
    if (hostExpected && (searchUrl.endsWith("/") || uri.pathname.length > 1)) {
      let pathIndex = uri.toString().lastIndexOf(uri.pathname);
      let prePath = uri.toString().slice(0, pathIndex);
      iconUri = `page-icon:${prePath}/`;
    }

    return new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.URL,
      source: lazy.UrlbarShared.RESULT_SOURCE.OTHER_LOCAL,
      heuristic: true,
      payload: {
        title: displayURL,
        url: escapedURL,
        icon: iconUri,
      },
    });
  }

  async _searchModeKeywordResult(queryContext) {
    if (!queryContext.tokens.length || queryContext.sapName != "urlbar") {
      return null;
    }

    let firstToken = queryContext.tokens[0].value;
    if (!lazy.UrlbarShared.SEARCH_MODE_RESTRICT.has(firstToken)) {
      return null;
    }

    let query = UrlbarUtils.substringAfter(
      queryContext.searchString,
      firstToken
    );
    if (!lazy.UrlUtils.REGEXP_SPACES_START.test(query)) {
      return null;
    }

    if (queryContext.restrictSource == lazy.UrlbarShared.RESULT_SOURCE.SEARCH) {
      return await this._engineSearchResult({
        queryContext,
        keyword: firstToken,
        heuristic: true,
      });
    }

    query = query.trimStart();
    return new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.SEARCH,
      source: lazy.UrlbarShared.RESULT_SOURCE.OTHER_LOCAL,
      heuristic: true,
      payload: {
        query,
        title: query,
        keyword: firstToken,
      },
    });
  }

  async _engineSearchResult({
    queryContext,
    keyword = null,
    heuristic = false,
  }) {
    let engine;
    if (queryContext.searchMode?.engineName) {
      engine = lazy.UrlbarSearchUtils.getEngineByName(
        queryContext.searchMode.engineName
      );
    } else {
      engine = lazy.UrlbarSearchUtils.getDefaultEngine(queryContext.isPrivate);
    }

    if (!engine) {
      return null;
    }

    let query = queryContext.searchString;
    if (
      queryContext.tokens[0] &&
      queryContext.tokens[0].value === lazy.UrlbarShared.RESTRICT_TOKENS.SEARCH
    ) {
      query = UrlbarUtils.substringAfter(
        query,
        queryContext.tokens[0].value
      ).trim();
    }

    return new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.SEARCH,
      source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
      heuristic,
      payload: {
        engine: engine.name,
        icon: UrlbarUtils.ICON.SEARCH_GLASS,
        query,
        title: query,
        keyword,
      },
    });
  }
}
