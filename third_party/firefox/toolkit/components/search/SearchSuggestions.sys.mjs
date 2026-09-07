/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  DEFAULT_FORM_HISTORY_PARAM:
    "moz-src:///toolkit/components/search/SearchSuggestionController.sys.mjs",
  FormHistory: "resource://gre/modules/FormHistory.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  SearchSuggestionController:
    "moz-src:///toolkit/components/search/SearchSuggestionController.sys.mjs",
});

class SearchHistoryResult {
  #formFieldName = null;

  #formHistoryEntries = null;

  #remoteEntries = [];

  QueryInterface = ChromeUtils.generateQI([
    "nsIAutoCompleteResult",
    "nsISupportsWeakReference",
  ]);

  constructor(formFieldName, searchString, formHistoryEntries) {
    this.#formFieldName = formFieldName;
    this.searchString = searchString;
    this.#formHistoryEntries = formHistoryEntries;
  }

  set remoteEntries(remoteEntries) {
    this.#remoteEntries = remoteEntries;
    this.#removeDuplicateHistoryEntries();
  }

  searchString = "";

  errorDescription = "";

  get defaultIndex() {
    return this.matchCount ? 0 : -1;
  }

  get searchResult() {
    return this.matchCount
      ? Ci.nsIAutoCompleteResult.RESULT_SUCCESS
      : Ci.nsIAutoCompleteResult.RESULT_NOMATCH;
  }

  get matchCount() {
    return this.#formHistoryEntries.length + this.#remoteEntries.length;
  }

  getValueAt(index) {
    const item = this.#getAt(index);
    return item.text || item.value;
  }

  getLabelAt(index) {
    const item = this.#getAt(index);
    return item.text || item.label || item.value;
  }

  getCommentAt(index) {
    return this.#getAt(index).comment ?? "";
  }

  getStyleAt(index) {
    const itemStyle = this.#getAt(index).style;
    if (itemStyle) {
      return itemStyle;
    }

    if (index >= 0) {
      if (index < this.#formHistoryEntries.length) {
        return "fromhistory";
      }

      if (index > 0 && index == this.#formHistoryEntries.length) {
        return "datalist-first";
      }
    }
    return "";
  }

  getImageAt(_index) {
    return "";
  }

  getFinalCompleteValueAt(index) {
    return this.getValueAt(index);
  }

  isRemovableAt(index) {
    return this.#isFormHistoryEntry(index);
  }

  removeValueAt(index) {
    if (this.isRemovableAt(index)) {
      const [removedEntry] = this.#formHistoryEntries.splice(index, 1);
      lazy.FormHistory.update({
        op: "remove",
        fieldname: this.#formFieldName,
        value: removedEntry.text,
        guid: removedEntry.guid,
      });
    }
  }

  #getAt(index) {
    for (const group of [this.#formHistoryEntries, this.#remoteEntries]) {
      if (index < group.length) {
        return group[index];
      }
      index -= group.length;
    }

    throw Components.Exception(
      "Index out of range.",
      Cr.NS_ERROR_ILLEGAL_VALUE
    );
  }


  #isFormHistoryEntry(index) {
    return index >= 0 && index < this.#formHistoryEntries.length;
  }

  #removeDuplicateHistoryEntries() {
    this.#formHistoryEntries = this.#formHistoryEntries.filter(entry =>
      this.#remoteEntries.every(
        fixed => entry.text != (fixed.label || fixed.value)
      )
    );
  }
}

class SuggestAutoComplete {
  constructor() {
    this.#suggestionController = new lazy.SearchSuggestionController();
  }

  onResultsReady(result) {
    if (this.#listener) {
      this.#listener.onSearchResult(this, result);

      this.#listener = null;
    }
  }

  startSearch(searchString, searchParam, previousResult, listener) {
    var formHistorySearchParam = searchParam.split("|")[0];

    var privacyMode = searchParam.split("|")[1] == "private";

    if (lazy.SearchService.isInitialized) {
      this.#triggerSearch(
        searchString,
        formHistorySearchParam,
        listener,
        privacyMode
      ).catch(console.error);
      return;
    }

    lazy.SearchService.init()
      .then(() => {
        this.#triggerSearch(
          searchString,
          formHistorySearchParam,
          listener,
          privacyMode
        ).catch(console.error);
      })
      .catch(result =>
        console.error(
          "Could not initialize search service, bailing out:",
          result
        )
      );
  }

  stopSearch() {
    this.#listener = null;
    this.#suggestionController.stop();
  }

  #suggestionController;

  #historyLimit = 7;

  #listener = null;

  async #triggerSearch(searchString, searchParam, listener, inPrivateBrowsing) {
    this.#listener = listener;
    let results = await this.#suggestionController.fetch({
      searchString,
      inPrivateBrowsing,
      engine: lazy.SearchService.defaultEngine,
      maxLocalResults: this.#historyLimit,
    });

    let formHistoryEntries = (results?.formHistoryResults ?? []).map(
      historyEntry => ({
        comment: historyEntry.text,
        ...historyEntry,
      })
    );
    let autoCompleteResult = new SearchHistoryResult(
      lazy.DEFAULT_FORM_HISTORY_PARAM,
      searchString,
      formHistoryEntries
    );

    if (results?.remote?.length) {
      autoCompleteResult.remoteEntries = results.remote.reduce((acc, item) => {
        if (!item.matchPrefix && !item.tail) {
          acc.push({
            value: item.value,
            label: item.value,
            comment: item.value,
          });
        }

        return acc;
      }, []);
    }

    this.onResultsReady(autoCompleteResult);
  }

  QueryInterface = ChromeUtils.generateQI([
    "nsIAutoCompleteSearch",
    "nsIAutoCompleteObserver",
  ]);
}

export class SearchSuggestAutoComplete extends SuggestAutoComplete {
  classID = Components.ID("{aa892eb4-ffbf-477d-9f9a-06c995ae9f27}");
  serviceURL = "";
}
