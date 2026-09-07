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
  UrlbarSearchUtils:
    "moz-src:///browser/components/urlbar/UrlbarSearchUtils.sys.mjs",
  UrlUtils: "resource://gre/modules/UrlUtils.sys.mjs",
});

export class UrlbarProviderTokenAliasEngines extends UrlbarProvider {
  constructor() {
    super();
    this._engines = [];
  }

  get type() {
    return UrlbarUtils.PROVIDER_TYPE.HEURISTIC;
  }

  static get PRIORITY() {
    return 1;
  }

  async isActive(queryContext) {
    let instance = this.queryInstance;

    this._autofillData = null;

    if (
      !queryContext.searchString.startsWith("@") ||
      queryContext.tokens.length != 1
    ) {
      return false;
    }

    if (queryContext.restrictInSearchMode()) {
      return false;
    }

    this._engines = await lazy.UrlbarSearchUtils.tokenAliasEngines();
    if (!this._engines.length) {
      return false;
    }

    if (instance != this.queryInstance) {
      return false;
    }

    if (queryContext.trimmedSearchString == "@") {
      return true;
    }

    if (lazy.UrlbarPrefs.get("autoFill") && queryContext.allowAutofill) {
      let result = await this._getAutofillResult(queryContext);
      if (result && instance == this.queryInstance) {
        this._autofillData = { result, instance };
        return true;
      }
    }

    return false;
  }

  async startQuery(queryContext, addCallback) {
    if (!this._engines || !this._engines.length) {
      return;
    }

    if (
      this._autofillData &&
      this._autofillData.instance == this.queryInstance
    ) {
      addCallback(this, this._autofillData.result);
    }

    let instance = this.queryInstance;
    for (let { engine, tokenAliases } of this._engines) {
      if (
        tokenAliases[0].startsWith(queryContext.trimmedSearchString) &&
        engine.name != this._autofillData?.result.payload.engine
      ) {
        let result = new lazy.UrlbarResult({
          type: lazy.UrlbarShared.RESULT_TYPE.SEARCH,
          source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
          hideRowLabel: true,
          payload: {
            engine: engine.name,
            keyword: tokenAliases[0],
            keywords: tokenAliases.join(", "),
            query: "",
            icon: await engine.getIconURL(),
            providesSearchMode: true,
          },
          highlights: {
            engine: UrlbarUtils.HIGHLIGHT.TYPED,
            keyword: UrlbarUtils.HIGHLIGHT.TYPED,
          },
        });
        if (instance != this.queryInstance) {
          break;
        }
        addCallback(this, result);
      }
    }

    this._autofillData = null;
  }

  getPriority() {
    return UrlbarProviderTokenAliasEngines.PRIORITY;
  }

  cancelQuery() {
    if (this._autofillData?.instance == this.queryInstance) {
      this._autofillData = null;
    }
  }

  async _getAutofillResult(queryContext) {
    let { lowerCaseSearchString } = queryContext;

    for (let { engine, tokenAliases } of this._engines) {
      for (let alias of tokenAliases) {
        if (alias.startsWith(lowerCaseSearchString)) {

          if (
            lowerCaseSearchString.startsWith(alias) &&
            lazy.UrlUtils.REGEXP_SPACES_START.test(
              lowerCaseSearchString.substring(alias.length)
            )
          ) {
            return null;
          }

          let aliasPreservingUserCase =
            queryContext.searchString +
            alias.substr(queryContext.searchString.length);
          let value = aliasPreservingUserCase + " ";
          return new lazy.UrlbarResult({
            type: lazy.UrlbarShared.RESULT_TYPE.SEARCH,
            source: lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
            suggestedIndex: 0,
            autofill: {
              value,
              selectionStart: queryContext.searchString.length,
              selectionEnd: value.length,
            },
            hideRowLabel: true,
            payload: {
              engine: engine.name,
              keyword: aliasPreservingUserCase,
              keywords: tokenAliases.join(", "),
              query: "",
              icon: await engine.getIconURL(),
              providesSearchMode: true,
            },
            highlights: {
              engine: UrlbarUtils.HIGHLIGHT.TYPED,
              keyword: UrlbarUtils.HIGHLIGHT.TYPED,
            },
          });
        }
      }
    }
    return null;
  }
}
