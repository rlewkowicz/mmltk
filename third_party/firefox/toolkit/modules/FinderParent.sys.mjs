// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

const kModalHighlightPref = "findbar.modalHighlight";

import {
  initSound,
  playSound,
} from "resource://gre/modules/FinderSound.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  GetClipboardSearchString: "resource://gre/modules/Finder.sys.mjs",
  Rect: "resource://gre/modules/Geometry.sys.mjs",
});

export function FinderParent(browser) {
  this._listeners = new Set();
  this._searchString = "";
  this._foundSearchString = null;
  this._lastFoundBrowsingContext = null;

  this._caseSensitive = false;
  this._entireWord = false;
  this._matchDiacritics = false;

  this.swapBrowser(browser);
}

FinderParent.prototype = {
  get browsingContext() {
    return this._browser.browsingContext;
  },

  get useRemoteSubframes() {
    return this._browser.documentGlobal.docShell.nsILoadContext
      .useRemoteSubframes;
  },

  swapBrowser(aBrowser) {
    this._browser = aBrowser;
    this._listeners.clear();
  },

  addResultListener(aListener) {
    this._listeners.add(aListener);
  },

  removeResultListener(aListener) {
    this._listeners.delete(aListener);
  },

  callListeners(aCallback, aArgs) {
    for (let l of this._listeners) {
      try {
        l[aCallback].apply(l, aArgs);
      } catch (e) {
        if (!l[aCallback]) {
          console.error(
            `Missing ${aCallback} callback on RemoteFinderListener`
          );
        } else {
          console.error(e);
        }
      }
    }
  },

  getLastFoundBrowsingContext(aList) {
    if (
      aList.includes(this._lastFoundBrowsingContext) &&
      !this._lastFoundBrowsingContext.isUnderHiddenEmbedderElement
    ) {
      return this._lastFoundBrowsingContext;
    }

    this._lastFoundBrowsingContext = null;
    return null;
  },

  sendMessageToContext(aMessageName, aArgs = {}) {
    let browsingContext = null;
    if (this._lastFoundBrowsingContext) {
      let list = this.gatherBrowsingContexts(this.browsingContext);
      let lastBrowsingContext = this.getLastFoundBrowsingContext(list);
      if (lastBrowsingContext) {
        browsingContext = lastBrowsingContext;
      }
    }

    if (!browsingContext) {
      browsingContext = this.browsingContext;
    }

    let windowGlobal = browsingContext.currentWindowGlobal;
    if (windowGlobal) {
      let actor = windowGlobal.getActor("Finder");
      actor.sendAsyncMessage(aMessageName, aArgs);
    }
  },

  sendQueryToContext(aMessageName, aArgs, aBrowsingContext) {
    let windowGlobal = aBrowsingContext.currentWindowGlobal;
    if (windowGlobal) {
      let actor = windowGlobal.getActor("Finder");
      return actor.sendQuery(aMessageName, aArgs).then(
        result => result,
        () => {}
      );
    }

    return Promise.resolve({});
  },

  sendMessageToAllContexts(aMessageName, aArgs = {}) {
    let list = this.gatherBrowsingContexts(this.browsingContext);
    for (let browsingContext of list) {
      let windowGlobal = browsingContext.currentWindowGlobal;
      if (windowGlobal) {
        let actor = windowGlobal.getActor("Finder");
        actor.sendAsyncMessage(aMessageName, aArgs);
      }
    }
  },

  gatherBrowsingContexts(aBrowsingContext) {
    if (aBrowsingContext.isUnderHiddenEmbedderElement) {
      return [];
    }

    let list = [aBrowsingContext];

    for (let child of aBrowsingContext.children) {
      list.push(...this.gatherBrowsingContexts(child));
    }

    return list;
  },

  needSubFrameSearch(aList) {
    let useSubFrames = false;

    let useModalHighlighter = Services.prefs.getBoolPref(kModalHighlightPref);
    let hasOutOfProcessChild = false;
    if (useModalHighlighter) {
      if (this.useRemoteSubframes) {
        return false;
      }

      for (let browsingContext of aList) {
        if (
          browsingContext != this.browsingContext &&
          browsingContext.currentWindowGlobal.isProcessRoot
        ) {
          hasOutOfProcessChild = true;
        }
      }

      if (!hasOutOfProcessChild) {
        aList.splice(0);
        aList.push(this.browsingContext);
        useSubFrames = true;
      }
    }

    return useSubFrames;
  },

  onResultFound(aResponse) {
    this._foundSearchString = aResponse.searchString;
    if (aResponse.rect) {
      aResponse.rect = lazy.Rect.fromRect(aResponse.rect);
    }

    this.callListeners("onFindResult", [aResponse]);
  },

  get searchString() {
    return this._foundSearchString;
  },

  get clipboardSearchString() {
    return lazy.GetClipboardSearchString(this._browser.loadContext);
  },

  set caseSensitive(aSensitive) {
    this._caseSensitive = aSensitive;
    this.sendMessageToAllContexts("Finder:CaseSensitive", {
      caseSensitive: aSensitive,
    });
  },

  set entireWord(aEntireWord) {
    this._entireWord = aEntireWord;
    this.sendMessageToAllContexts("Finder:EntireWord", {
      entireWord: aEntireWord,
    });
  },

  set matchDiacritics(aMatchDiacritics) {
    this._matchDiacritics = aMatchDiacritics;
    this.sendMessageToAllContexts("Finder:MatchDiacritics", {
      matchDiacritics: aMatchDiacritics,
    });
  },

  async setSearchStringToSelection() {
    return this.setToSelection("Finder:SetSearchStringToSelection", false);
  },

  async getInitialSelection() {
    return this.setToSelection("Finder:GetInitialSelection", true);
  },

  async setToSelection(aMessage, aInitial) {
    let browsingContext = this.browsingContext;

    let result;
    do {
      result = await this.sendQueryToContext(aMessage, {}, browsingContext);
      if (!result || !result.focusedChildBrowserContextId) {
        break;
      }

      browsingContext = BrowsingContext.get(
        result.focusedChildBrowserContextId
      );
    } while (browsingContext);

    if (result) {
      this.callListeners("onCurrentSelection", [result.selectedText, aInitial]);
    }

    return result;
  },

  async doFind(aFindNext, aArgs) {
    let rootBC = this.browsingContext;
    let highlightList = this.gatherBrowsingContexts(rootBC);

    let searchLengthened =
      aArgs.searchString.length > this._searchString.length;

    this._searchString = aArgs.searchString;

    let initialBC = this.getLastFoundBrowsingContext(highlightList);
    if (!initialBC) {
      initialBC = rootBC;
      aFindNext = false;
    }

    let searchList = [];
    for (let c = 0; c < highlightList.length; c++) {
      if (highlightList[c] == initialBC) {
        searchList = highlightList.slice(c);
        searchList.push(...highlightList.slice(0, c));
        break;
      }
    }

    let mode = Ci.nsITypeAheadFind.FIND_INITIAL;
    if (aFindNext) {
      mode = aArgs.findBackwards
        ? Ci.nsITypeAheadFind.FIND_PREVIOUS
        : Ci.nsITypeAheadFind.FIND_NEXT;
    }
    aArgs.findAgain = aFindNext;

    aArgs.caseSensitive = this._caseSensitive;
    aArgs.matchDiacritics = this._matchDiacritics;
    aArgs.entireWord = this._entireWord;

    aArgs.useSubFrames = this.needSubFrameSearch(searchList);
    if (aArgs.useSubFrames) {
      highlightList = searchList;
    }

    if (searchLengthened) {
      initSound();
    }

    searchList = [...searchList, initialBC];

    if (aArgs.findBackwards) {
      searchList.reverse();
    }

    let response = null;
    let wrapped = false;
    let foundBC = null;

    for (let c = 0; c < searchList.length; c++) {
      let currentBC = searchList[c];
      aArgs.mode = mode;

      if (this._searchString != aArgs.searchString) {
        return;
      }

      response = await this.sendQueryToContext("Finder:Find", aArgs, currentBC);

      if (!response) {
        break;
      }

      if (
        Object.hasOwn(response, "result") &&
        response.result != Ci.nsITypeAheadFind.FIND_NOTFOUND
      ) {
        if (
          this._lastFoundBrowsingContext &&
          this._lastFoundBrowsingContext != currentBC
        ) {
          this.removeSelection(true);
        }
        this._lastFoundBrowsingContext = currentBC;

        if (wrapped) {
          response.result = Ci.nsITypeAheadFind.FIND_WRAPPED;
        }

        foundBC = currentBC;
        break;
      }

      if (aArgs.findBackwards && currentBC == rootBC) {
        wrapped = true;
      } else if (
        !aArgs.findBackwards &&
        c + 1 < searchList.length &&
        searchList[c + 1] == rootBC
      ) {
        wrapped = true;
      }

      mode = aArgs.findBackwards
        ? Ci.nsITypeAheadFind.FIND_LAST
        : Ci.nsITypeAheadFind.FIND_FIRST;
    }

    if (response) {
      response.useSubFrames = aArgs.useSubFrames;
      this.updateHighlightAndMatchCount({
        list: highlightList,
        message: "Finder:UpdateHighlightAndMatchCount",
        args: response,
        foundBrowsingContextId: foundBC ? foundBC.id : -1,
        doHighlight: true,
        doMatchCount: true,
      });

      this.onResultFound(response);

      if (
        searchLengthened &&
        response.result == Ci.nsITypeAheadFind.FIND_NOTFOUND &&
        !aFindNext &&
        !response.entireWord
      ) {
        playSound("not-found");
      }

      if (response.result == Ci.nsITypeAheadFind.FIND_WRAPPED) {
        playSound("wrapped");
      }
    }
  },

  fastFind(aSearchString, aLinksOnly, aDrawOutline) {
    this.doFind(false, {
      searchString: aSearchString,
      findBackwards: false,
      linksOnly: aLinksOnly,
      drawOutline: aDrawOutline,
    });
  },

  findAgain(aSearchString, aFindBackwards, aLinksOnly, aDrawOutline) {
    this.doFind(true, {
      searchString: aSearchString,
      findBackwards: aFindBackwards,
      linksOnly: aLinksOnly,
      drawOutline: aDrawOutline,
    });
  },

  highlight(aHighlight, aWord, aLinksOnly) {
    let list = this.gatherBrowsingContexts(this.browsingContext);
    let args = {
      highlight: aHighlight,
      linksOnly: aLinksOnly,
      searchString: aWord,
    };

    args.useSubFrames = this.needSubFrameSearch(list);

    let lastBrowsingContext = this.getLastFoundBrowsingContext(list);
    this.updateHighlightAndMatchCount({
      list,
      message: "Finder:Highlight",
      args,
      foundBrowsingContextId: lastBrowsingContext ? lastBrowsingContext.id : -1,
      doHighlight: true,
      doMatchCount: false,
    });
  },

  requestMatchesCount(aSearchString, optionalArgs) {
    let list = this.gatherBrowsingContexts(this.browsingContext);
    let args = {
      searchString: aSearchString,
      linksOnly: optionalArgs.linksOnly,
      contextRange: optionalArgs.contextRange || 0,
    };

    args.useSubFrames = this.needSubFrameSearch(list);

    let lastBrowsingContext = this.getLastFoundBrowsingContext(list);
    this.updateHighlightAndMatchCount({
      list,
      message: "Finder:MatchesCount",
      args,
      foundBrowsingContextId: lastBrowsingContext ? lastBrowsingContext.id : -1,
      doHighlight: false,
      doMatchCount: true,
    });
  },

  updateHighlightAndMatchCount(options) {
    let promises = [];
    let found = options.args.result != Ci.nsITypeAheadFind.FIND_NOTFOUND;
    for (let browsingContext of options.list) {
      options.args.foundInThisFrame =
        options.foundBrowsingContextId != -1 &&
        found &&
        browsingContext.id == options.foundBrowsingContextId;

      let promise = this.sendQueryToContext(
        options.message,
        options.args,
        browsingContext
      );
      promises.push(promise);
    }

    Promise.all(promises).then(responses => {
      if (options.doHighlight) {
        let sendNotification = false;
        let highlight = false;
        let found = false;
        for (let response of responses) {
          if (!response) {
            break;
          }

          sendNotification = true;
          if (response.found) {
            found = true;
          }
          highlight = response.highlight;
        }

        if (sendNotification) {
          this.callListeners("onHighlightFinished", [
            { searchString: options.args.searchString, highlight, found },
          ]);
        }
      }

      if (options.doMatchCount) {
        let sendNotification = false;
        let current = 0;
        let total = 0;
        let limit = 0;
        let allSnippets = [];
        for (let response of responses) {
          if (!response || !("total" in response)) {
            break;
          }

          sendNotification = true;

          if (
            options.args.useSubFrames ||
            (options.foundBrowsingContextId >= 0 &&
              response.browsingContextId == options.foundBrowsingContextId)
          ) {
            current = total + response.current;
          }
          total += response.total;
          limit = response.limit;

          if (response.snippets && Array.isArray(response.snippets)) {
            allSnippets.push(...response.snippets);
          }
        }

        if (sendNotification) {
          this.callListeners("onMatchesCountResult", [
            {
              searchString: options.args.searchString,
              current,
              total,
              limit,
              snippets: allSnippets,
            },
          ]);
        }
      }
    });
  },

  enableSelection() {
    this.sendMessageToContext("Finder:EnableSelection");
  },

  removeSelection(aKeepHighlight) {
    this.sendMessageToContext("Finder:RemoveSelection", {
      keepHighlight: aKeepHighlight,
    });
  },

  focusContent() {
    for (let l of this._listeners) {
      try {
        if ("shouldFocusContent" in l && !l.shouldFocusContent()) {
          return;
        }
      } catch (ex) {
        console.error(ex);
      }
    }

    this._browser.focus();
    this.sendMessageToContext("Finder:FocusContent");
  },

  onFindbarClose() {
    this._lastFoundBrowsingContext = null;
    this.sendMessageToAllContexts("Finder:FindbarClose");
  },

  onFindbarOpen() {
    this.sendMessageToAllContexts("Finder:FindbarOpen");
  },

  onModalHighlightChange(aUseModalHighlight) {
    this.sendMessageToAllContexts("Finder:ModalHighlightChange", {
      useModalHighlight: aUseModalHighlight,
    });
  },

  onHighlightAllChange(aHighlightAll) {
    this.sendMessageToAllContexts("Finder:HighlightAllChange", {
      highlightAll: aHighlightAll,
    });
  },

  keyPress(aEvent) {
    this.sendMessageToContext("Finder:KeyPress", {
      keyCode: aEvent.keyCode,
      ctrlKey: aEvent.ctrlKey,
      metaKey: aEvent.metaKey,
      altKey: aEvent.altKey,
      shiftKey: aEvent.shiftKey,
    });
  },
};
