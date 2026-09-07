/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ContentDOMReference: "resource://gre/modules/ContentDOMReference.sys.mjs",
});

const gFormFillController = Cc[
  "@mozilla.org/satchel/form-fill-controller;1"
].getService(Ci.nsIFormFillController);

export class AutoCompleteChild extends JSWindowActorChild {
  constructor() {
    super();

    this._input = null;
    this._popupOpen = false;
  }

  receiveMessage(message) {
    switch (message.name) {
      case "AutoComplete:HandleEnter": {
        this.selectedIndex = message.data.selectedIndex;

        let controller = Cc[
          "@mozilla.org/autocomplete/controller;1"
        ].getService(Ci.nsIAutoCompleteController);
        controller.handleEnter(message.data.isPopupSelection);
        break;
      }

      case "AutoComplete:PopupClosed": {
        this._popupOpen = false;
        break;
      }

      case "AutoComplete:PopupOpened": {
        this._popupOpen = true;
        break;
      }

      case "AutoComplete:Focus": {
        break;
      }
    }
  }

  get input() {
    return this._input;
  }

  set selectedIndex(index) {
    this.sendAsyncMessage("AutoComplete:SetSelectedIndex", { index });
  }

  get selectedIndex() {
    let selectedIndexResult = Services.cpmm.sendSyncMessage(
      "AutoComplete:GetSelectedIndex",
      {
        browsingContext: this.browsingContext,
      }
    );

    if (
      selectedIndexResult.length != 1 ||
      !Number.isInteger(selectedIndexResult[0])
    ) {
      throw new Error("Invalid autocomplete selectedIndex");
    }
    return selectedIndexResult[0];
  }

  get popupOpen() {
    return this._popupOpen;
  }

  openAutocompletePopup(input, element) {
    if (this._popupOpen || !input || !element?.isConnected) {
      return;
    }

    let window = element.documentGlobal;
    let rect = window.windowUtils.getElementBoundingScreenRect(element);
    let dir = window.getComputedStyle(element).direction;
    let results = this.getResultsFromController(input);
    let inputElementIdentifier = lazy.ContentDOMReference.get(element);
    let selectedIndex = Services.focus.focusedElement == element ? -1 : 0;

    this.sendAsyncMessage("AutoComplete:MaybeOpenPopup", {
      results,
      rect,
      dir,
      inputElementIdentifier,
      selectedIndex,
    });

    this._input = input;
  }

  closePopup() {
    this._popupOpen = false;
    this.sendAsyncMessage("AutoComplete:ClosePopup", {});
  }

  invalidate() {
    if (this._popupOpen) {
      let results = this.getResultsFromController(this._input);
      this.sendAsyncMessage("AutoComplete:Invalidate", { results });
    }
  }

  selectBy(reverse, page) {
    Services.cpmm.sendSyncMessage("AutoComplete:SelectBy", {
      browsingContext: this.browsingContext,
      reverse,
      page,
    });
  }

  getResultsFromController(inputField) {
    let results = [];

    if (!inputField) {
      return results;
    }

    let controller = inputField.controller;
    if (!(controller instanceof Ci.nsIAutoCompleteController)) {
      return results;
    }

    for (let i = 0; i < controller.matchCount; ++i) {
      let result = {};
      result.value = controller.getValueAt(i);
      result.label = controller.getLabelAt(i);
      result.comment = controller.getCommentAt(i);
      result.style = controller.getStyleAt(i);
      result.image = controller.getImageAt(i);
      results.push(result);
    }

    return results;
  }

  getNoRollupOnEmptySearch(input) {
    return false;
  }

  #providersByInput = new WeakMap();

  providersByInput(input) {
    return new Set(this.#providersByInput.get(input));
  }

  markAsAutoCompletableField(input, provider) {
    gFormFillController.markAsAutoCompletableField(input);

    let providers = this.#providersByInput.get(input);
    if (!providers) {
      providers = new Set();
      this.#providersByInput.set(input, providers);
    }
    providers.add(provider);
  }

  #ongoingSearches = new Set();

  async startSearch(searchString, input, listener) {
    const providers = this.providersByInput(input);
    const data = Array.from(providers)
      .filter(p => p.shouldSearchForAutoComplete(input, searchString))
      .map(p => ({
        actorName: p.actorName,
        options: p.getAutoCompleteSearchOption(input, searchString),
      }));

    let result = [];

    if (data.length) {
      const promise = this.sendQuery("AutoComplete:StartSearch", {
        searchString,
        data,
      });
      this.#ongoingSearches.add(promise);
      result = await promise.catch(e => {
        this.#ongoingSearches.delete(promise);
      });
      result ||= [];

      if (!this.#ongoingSearches.delete(promise)) {
        return;
      }
    }

    let foundResults = [];
    let emptyResults = [];

    for (const provider of providers) {
      const searchResult = result.find(r => r.actorName == provider.actorName);
      if (searchResult) {
        foundResults.push([provider, searchResult]);
      } else {
        emptyResults.push([provider, undefined]);
      }
    }

    for (const [provider, searchResult] of foundResults.length
      ? foundResults
      : emptyResults) {
      const acResult = provider.searchResultToAutoCompleteResult(
        searchString,
        input,
        searchResult
      );

      if (acResult) {
        listener.onSearchCompletion(acResult);
        return;
      }
    }
  }

  stopSearch() {
    this.#ongoingSearches.clear();
  }

  selectEntry() {
    this.sendAsyncMessage("AutoComplete:SelectEntry");
  }
}

AutoCompleteChild.prototype.QueryInterface = ChromeUtils.generateQI([
  "nsIAutoCompletePopup",
]);
