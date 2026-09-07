/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

export const DEFAULT_FORM_HISTORY_PARAM = "searchbar-history";
const HTTP_OK = 200;
const REMOTE_TIMEOUT_DEFAULT = 500;

const lazy = XPCOMUtils.declareLazy({
  ConfigSearchEngine:
    "moz-src:///toolkit/components/search/ConfigSearchEngine.sys.mjs",
  FormHistory: "resource://gre/modules/FormHistory.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  SearchUtils: "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  logConsole: () =>
    console.createInstance({
      prefix: "SearchSuggestionController",
      maxLogLevel: lazy.SearchUtils.loggingEnabled ? "Debug" : "Warn",
    }),
  suggestionsEnabled: { pref: "browser.search.suggest.enabled", default: true },
  suggestionsInPrivateBrowsingEnabled: {
    pref: "browser.search.suggest.enabled.private",
    default: false,
  },
  richSuggestionsEnabled: {
    pref: "browser.urlbar.richSuggestions.featureGate",
    default: false,
  },
  remoteTimeout: {
    pref: "browser.search.suggest.timeout",
    default: REMOTE_TIMEOUT_DEFAULT,
  },
});







class SearchSuggestionEntry {
  constructor(value, { matchPrefix, tail, icon, description, trending } = {}) {
    this.#value = value;
    this.#matchPrefix = matchPrefix;
    this.#tail = tail;
    this.#trending = trending;
    this.#icon = icon;
    this.#description = description;
  }

  get value() {
    return this.#value;
  }

  get matchPrefix() {
    return this.#matchPrefix;
  }

  get tail() {
    return this.#tail;
  }

  get trending() {
    return this.#trending;
  }

  get icon() {
    return this.#icon;
  }

  get description() {
    return this.#description;
  }

  get tailOffsetIndex() {
    if (!this.#tail) {
      return -1;
    }

    let offsetIndex = this.#value.lastIndexOf(this.#tail);
    if (offsetIndex + this.#tail.length < this.#value.length) {
      let lastWordIndex = this.#value.lastIndexOf(" ");
      if (this.#tail.startsWith(this.#value.substring(lastWordIndex))) {
        offsetIndex = lastWordIndex;
      } else {
        offsetIndex = -1;
      }
    }

    return offsetIndex;
  }

  equals(otherEntry) {
    return otherEntry.value == this.value;
  }

  #value;
  #matchPrefix;
  #tail;
  #trending;
  #icon;
  #description;
}

export class SearchSuggestionController {
  static SEARCH_HISTORY_MAX_VALUE_LENGTH = 255;

  static REMOTE_TIMEOUT_DEFAULT = REMOTE_TIMEOUT_DEFAULT;

  static engineOffersSuggestions(engine, fetchTrending) {
    return engine.supportsResponseType(
      fetchTrending
        ? lazy.SearchUtils.URL_TYPE.TRENDING_JSON
        : lazy.SearchUtils.URL_TYPE.SUGGEST_JSON
    );
  }

  formHistoryResult = null;


  fetch({
    searchString,
    inPrivateBrowsing,
    engine,
    maxLocalResults = 5,
    maxRemoteResults = 10,
    userContextId = 0,
    restrictToEngine = false,
    dedupeRemoteAndLocal = true,
    fetchTrending = false,
  }) {

    lazy.logConsole.debug(
      `SearchSuggestionController.fetch() called with searchString: ${searchString}`
    );

    this.stop();

    if (!lazy.SearchService.isInitialized) {
      throw new Error("Search not initialized yet (how did you get here?)");
    }
    if (typeof inPrivateBrowsing === "undefined") {
      throw new Error(
        "The inPrivateBrowsing argument is required to avoid unintentional privacy leaks"
      );
    }
    if (!engine.getSubmission) {
      throw new Error("Invalid search engine");
    }
    if (!maxLocalResults && !maxRemoteResults) {
      throw new Error("Zero results expected, what are you trying to do?");
    }
    if (maxLocalResults < 0 || maxRemoteResults < 0) {
      throw new Error("Number of requested results must be positive");
    }

    let promises = [];
    this.#context = {
      awaitingLocalResults: false,
      dedupeRemoteAndLocal,
      engine,
      maxLocalResults,
      maxRemoteResults,
      fetchTrending,
      inPrivateBrowsing,
      restrictToEngine,
      searchString,
      timer: Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer),
      userContextId,
    };

    if (maxLocalResults && !fetchTrending) {
      this.#context.awaitingLocalResults = true;
      promises.push(this.#fetchFormHistory(this.#context));
    }
    if (
      (searchString || fetchTrending) &&
      lazy.suggestionsEnabled &&
      (!inPrivateBrowsing || lazy.suggestionsInPrivateBrowsingEnabled) &&
      maxRemoteResults &&
      SearchSuggestionController.engineOffersSuggestions(engine, fetchTrending)
    ) {
      promises.push(this.#fetchRemote(this.#context));
    }

    function handleRejection(reason) {
      if (
        typeof reason == "string" &&
        reason.startsWith("HTTP request aborted")
      ) {
        lazy.logConsole.debug(reason);
        return null;
      }
      console.error("SearchSuggestionController rejection:", reason);
      return null;
    }
    return Promise.all(promises).then(
      results => this.#dedupeAndReturnResults(this.#context, results),
      handleRejection
    );
  }

  stop() {
    if (this.#context) {
      this.#context.aborted = true;
      this.#context.request?.abort();
    }
    this.#context = null;
  }

  #context;

  async #fetchFormHistory(context) {
    let params = {
      fieldname: DEFAULT_FORM_HISTORY_PARAM,
    };

    if (context.restrictToEngine) {
      params.source = context.engine.name;
    }

    let results = await lazy.FormHistory.getAutoCompleteResults(
      context.searchString,
      params
    );

    context.awaitingLocalResults = false;

    return { localResults: results };
  }

  #fetchRemote(context) {
    let submission = context.engine.getSubmission(
      context.searchString,
      context.searchString
        ? lazy.SearchUtils.URL_TYPE.SUGGEST_JSON
        : lazy.SearchUtils.URL_TYPE.TRENDING_JSON
    );

    let deferredResponse = Promise.withResolvers();
    let request = (context.request = new XMLHttpRequest());
    request.responseType = "json";

    let method = submission.postData ? "POST" : "GET";
    request.open(method, submission.uri.spec, true);
    request.channel.loadFlags |=
      Ci.nsIChannel.LOAD_ANONYMOUS | Ci.nsIChannel.INHIBIT_PERSISTENT_CACHING;

    lazy.logConsole.debug(
      `HTTP request started for ${submission.uri.spec} by method ${method}`
    );

    request.setOriginAttributes({
      userContextId: context.userContextId,
      privateBrowsingId: context.inPrivateBrowsing ? 1 : 0,
      firstPartyDomain: `${context.engine.id}.search.suggestions.mozilla`,
    });

    request.mozBackgroundRequest = true; 

    context.timer.initWithCallback(
      () => {
        if (
          request.readyState != 4  &&
          !context.awaitingLocalResults
        ) {
          deferredResponse.resolve("HTTP request timeout");
        }
      },
      lazy.remoteTimeout,
      Ci.nsITimer.TYPE_ONE_SHOT
    );

    request.addEventListener("load", () => {
      context.timer.cancel();
      if (!this.#context || context != this.#context || context.aborted) {
        deferredResponse.resolve(
          "Got HTTP response after the request was cancelled"
        );
        return;
      }

      let status;
      try {
        status = context.request.status;
      } catch (e) {
        deferredResponse.resolve("Unknown HTTP status: " + e);
        return;
      }

      if (status != HTTP_OK) {
        deferredResponse.resolve(
          "Non-200 status or empty HTTP response: " + status
        );
        return;
      }

      this.#onRemoteLoaded(
        context,
        context.request.response,
        deferredResponse.resolve
      );
    });

    request.addEventListener("error", () => {
      this.#context.errorWasReceived = true;
      deferredResponse.resolve("HTTP error");
    });

    request.addEventListener("abort", () => {
      context.timer.cancel();
      deferredResponse.reject(
        `HTTP request aborted for ${submission.uri.spec}}`
      );
    });

    if (submission.postData) {
      request.sendInputStream(submission.postData);
    } else {
      request.send();
    }

    return deferredResponse.promise;
  }
  #onRemoteLoaded(context, serverResults, resolve) {
    lazy.logConsole.debug("Remote results:", serverResults);

    try {
      if (
        !Array.isArray(serverResults) ||
        serverResults[0] == undefined ||
        (context.searchString.localeCompare(serverResults[0], undefined, {
          sensitivity: "base",
        }) &&
          context.searchString.localeCompare(
            decodeURIComponent(
              JSON.parse('"' + serverResults[0].replace(/\"/g, '\\"') + '"')
            ),
            undefined,
            {
              sensitivity: "base",
            }
          ))
      ) {
        resolve(
          "Unexpected response, searchString does not match remote response"
        );
        return;
      }
    } catch (ex) {
      resolve(`Failed to parse the remote response string: ${ex}`);
      return;
    }

    let results = serverResults.slice(1) || [];
    resolve({ result: results });
  }

  #dedupeAndReturnResults(context, suggestResults) {
    if (context.aborted) {
      return null;
    }

    let results = {
      term: context.searchString,
      remote: [],
      local: [],
    };

    for (let resultData of suggestResults) {
      if (typeof resultData === "string") {
        console.error(
          "SearchSuggestionController found an unexpected string value:",
          resultData
        );
      } else if (resultData.localResults) {
        results.formHistoryResults = resultData.localResults;
        results.local = resultData.localResults.map(
          s => new SearchSuggestionEntry(s.text)
        );
      } else if (resultData.result) {
        let richSuggestionData = this.#getRichSuggestionData(resultData.result);
        let fullTextSuggestions = resultData.result[0];
        for (let i = 0; i < fullTextSuggestions.length; ++i) {
          results.remote.push(
            this.#newSearchSuggestionEntry(
              fullTextSuggestions[i],
              richSuggestionData?.[i],
              context.fetchTrending
            )
          );
        }
      }
    }

    if (results.remote.length) {
      results.local = results.local.slice(0, context.maxLocalResults);
    }

    if (
      results.remote.length &&
      results.local.length &&
      context.dedupeRemoteAndLocal
    ) {
      for (let i = 0; i < results.local.length; ++i) {
        let dupIndex = results.remote.findIndex(e =>
          e.equals(results.local[i])
        );
        if (dupIndex != -1) {
          results.remote.splice(dupIndex, 1);
        }
      }
    }

    let maxRemoteCount = context.maxRemoteResults;
    if (context.dedupeRemoteAndLocal) {
      maxRemoteCount -= results.local.length;
    }
    results.remote = results.remote.slice(0, maxRemoteCount);

    lazy.logConsole.debug(
      `Deduplication completed. Final results count: local=${results.local.length}, remote=${results.remote.length}`
    );

    return results;
  }

  #getRichSuggestionData(remoteResultData) {
    if (!remoteResultData || !Array.isArray(remoteResultData)) {
      return undefined;
    }

    for (let entry of remoteResultData) {
      if (
        typeof entry == "object" &&
        entry.hasOwnProperty("google:suggestdetail")
      ) {
        let richData = entry["google:suggestdetail"];
        if (
          Array.isArray(richData) &&
          richData.length == remoteResultData[0].length
        ) {
          return richData;
        }
      }
    }
    return undefined;
  }

  #newSearchSuggestionEntry(suggestion, richSuggestionData, trending) {
    if (richSuggestionData && (!trending || lazy.richSuggestionsEnabled)) {
      let args = { trending };

      if (!richSuggestionData?.i) {
        args.matchPrefix = richSuggestionData?.mp;
        args.tail = richSuggestionData?.t;
      } else if (lazy.richSuggestionsEnabled) {
        args.icon = richSuggestionData?.i;
        args.description = richSuggestionData?.a;
      }

      return new SearchSuggestionEntry(suggestion, args);
    }
    return new SearchSuggestionEntry(suggestion, { trending });
  }
}
