/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";


const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ProvidersManager:
    "moz-src:///browser/components/urlbar/UrlbarProvidersManager.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "logger", () =>
  lazy.UrlbarShared.getLogger({ prefix: "Controller" })
);

export class UrlbarParentController {
  #child = null;

  #actor = null;

  constructor({ sapName, isPrivate = false, actor, manager }) {
    if (!sapName) {
      throw new Error("Missing options: sapName");
    }

    this.sapName = sapName;
    this.isPrivate = isPrivate;
    this.#actor = actor;

    this.manager =
      manager || lazy.ProvidersManager.getInstanceForSap(this.sapName);

  }

  get platform() {
    return AppConstants.platform;
  }

  get input() {
    return this.#child?.input;
  }

  get browserWindow() {
    return this.#actor?.browsingContext?.topChromeWindow;
  }

  get view() {
    return this.#child?.view;
  }

  getViewUpdate(result, idsByName) {
    return this.manager
      .getProvider(result.providerName)
      ?.getViewUpdate(result, idsByName);
  }

  onBeforeSelection(result, element) {
    this.manager
      .getProvider(result?.providerName)
      ?.tryMethod("onBeforeSelection", result, element);
  }

  onSelection(result, element) {
    this.manager
      .getProvider(result?.providerName)
      ?.tryMethod("onSelection", result, element);
  }

  async getHeuristicResult(queryContext) {
    await this.manager.startQuery(queryContext);
    return queryContext.heuristicResult;
  }

  async startQuery(queryContext) {
    this.cancelQuery();

    let contextWrapper = (this._lastQueryContextWrapper = { queryContext });

    queryContext.lastResultCount = 0;

    this.notify(lazy.UrlbarShared.NOTIFICATIONS.QUERY_STARTED, queryContext);
    await this.manager.startQuery(queryContext, this);

    if (
      contextWrapper === this._lastQueryContextWrapper &&
      !contextWrapper.done
    ) {
      contextWrapper.done = true;
      this.manager.cancelQuery(queryContext);
      this.notify(lazy.UrlbarShared.NOTIFICATIONS.QUERY_FINISHED, queryContext);
    }

    return queryContext;
  }





  cancelQuery() {
    if (!this._lastQueryContextWrapper || this._lastQueryContextWrapper.done) {
      return;
    }

    this._lastQueryContextWrapper.done = true;

    let { queryContext } = this._lastQueryContextWrapper;


    this.manager.cancelQuery(queryContext);
    this.notify(lazy.UrlbarShared.NOTIFICATIONS.QUERY_CANCELLED, queryContext);
    this.notify(lazy.UrlbarShared.NOTIFICATIONS.QUERY_FINISHED, queryContext);
  }

  receiveResults(queryContext) {

    if (queryContext.firstResultChanged && this.input) {
      if (this.input.onFirstResult(queryContext.results[0])) {
        return;
      }
    }

    this.notify(lazy.UrlbarShared.NOTIFICATIONS.QUERY_RESULTS, queryContext);
    queryContext.lastResultCount = queryContext.results.length;
  }

  setChild(child) {
    this.#child = child;
  }

  speculativeConnect(result, context, reason) {
    if (!this.browserWindow || context.isPrivate || !context.results.length) {
      return;
    }

    switch (reason) {
      case "resultsadded": {
        if (result.heuristic || result.autofill) {
          if (result.type == lazy.UrlbarShared.RESULT_TYPE.SEARCH) {
            if (lazy.UrlbarPrefs.get("browser.search.suggest.enabled")) {
              let engine = lazy.SearchService.getEngineByName(
                result.payload.engine
              );
              lazy.UrlbarUtils.setupSpeculativeConnection(
                engine,
                this.browserWindow
              );
            }
          } else if (result.autofill) {
            const { url } = lazy.UrlbarUtils.getUrlFromResult(result);
            if (!url) {
              return;
            }

            lazy.UrlbarUtils.setupSpeculativeConnection(
              url,
              this.browserWindow
            );
          }
        }
        return;
      }
      case "mousedown": {
        const { url } = lazy.UrlbarUtils.getUrlFromResult(result);
        if (!url) {
          return;
        }

        if (url.startsWith("http")) {
          lazy.UrlbarUtils.setupSpeculativeConnection(url, this.browserWindow);
        }
        return;
      }
      default: {
        throw new Error("Invalid speculative connection reason");
      }
    }
  }

  removeResult(result) {
    if (!result || result.heuristic) {
      return;
    }

    if (!this._lastQueryContextWrapper) {
      console.error("Cannot remove result, last query not present");
      return;
    }
    let { queryContext } = this._lastQueryContextWrapper;

    let index = queryContext.results.indexOf(result);
    if (index < 0) {
      console.error("Failed to find the selected result in the results");
      return;
    }

    queryContext.results.splice(index, 1);
    this.notify(lazy.UrlbarShared.NOTIFICATIONS.QUERY_RESULT_REMOVED, index);
  }

  setLastQueryContextCache(queryContext) {
    this._lastQueryContextWrapper = { queryContext };
  }

  clearLastQueryContextCache() {
    this._lastQueryContextWrapper = null;
  }

  notify(name, ...params) {
    this.#child.notify(name, ...params);
  }
}
