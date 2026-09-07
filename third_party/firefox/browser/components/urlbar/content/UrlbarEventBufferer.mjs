/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { UrlbarShared } from "chrome://browser/content/urlbar/UrlbarShared.mjs";

const DEFERRED_KEY_CODES = new Set([
  KeyboardEvent.DOM_VK_RETURN,
  KeyboardEvent.DOM_VK_DOWN,
  KeyboardEvent.DOM_VK_TAB,
]);

const QUERY_STATUS = Object.freeze({
  UKNOWN: 0,
  RUNNING: 1,
  RUNNING_GOT_ALL_HEURISTIC_RESULTS: 2,
  COMPLETE: 3,
});

export class UrlbarEventBufferer {
  #logger;
  get logger() {
    this.#logger ??= UrlbarShared.getLogger({
      prefix: "EventBufferer",
    });
    return this.#logger;
  }

  static DEFERRING_TIMEOUT_MS = false ? 1500 : 300;

  constructor(input) {
    this.input = input;
    this.input.inputField.addEventListener("blur", this);

    this.#lastQuery = {
      startDate: ChromeUtils.now(),
      status: QUERY_STATUS.UKNOWN,
      context: null,
    };

    this.input.controller.addListener(this);
  }


  onQueryStarted(queryContext) {
    this.#lastQuery = {
      startDate: ChromeUtils.now(),
      status: QUERY_STATUS.RUNNING,
      context: queryContext,
    };
    if (this.#deferringTimeout) {
      clearTimeout(this.#deferringTimeout);
      this.#deferringTimeout = null;
    }
  }

  onQueryCancelled() {
    this.#lastQuery.status = QUERY_STATUS.COMPLETE;
  }

  onQueryFinished() {
    this.#lastQuery.status = QUERY_STATUS.COMPLETE;
  }

  onQueryResults(queryContext) {
    if (queryContext.pendingHeuristicProviders.size) {
      return;
    }
    this.#lastQuery.status = QUERY_STATUS.RUNNING_GOT_ALL_HEURISTIC_RESULTS;
    Services.tm.dispatchToMainThread(() => {
      this.replayDeferredEvents(true);
    });
  }

  handleEvent(event) {
    if (event.type == "blur") {
      this.logger.debug("Clearing queue on blur");
      this.#eventsQueue.length = 0;
      if (this.#deferringTimeout) {
        clearTimeout(this.#deferringTimeout);
        this.#deferringTimeout = null;
      }
    }
  }

  maybeDeferEvent(event, callback) {
    if (!callback) {
      throw new Error("Must provide a callback");
    }
    if (this.shouldDeferEvent(event)) {
      this.deferEvent(event, callback);
      return;
    }
    callback();
  }

  deferEvent(event, callback) {
    if (this.#eventsQueue.find(item => item.event == event)) {
      throw new Error(`Event ${event.type}:${event.keyCode} already deferred!`);
    }
    this.logger.debug(`Deferring ${event.type}:${event.keyCode} event`);
    this.#eventsQueue.push({
      event,
      callback,
      searchString: this.#lastQuery.context.searchString,
    });

    if (!this.#deferringTimeout) {
      let elapsed = ChromeUtils.now() - this.#lastQuery.startDate;
      let remaining = UrlbarEventBufferer.DEFERRING_TIMEOUT_MS - elapsed;
      this.#deferringTimeout = setTimeout(
        () => {
          this.replayDeferredEvents(false);
          this.#deferringTimeout = null;
        },
        Math.max(0, remaining)
      );
    }
  }

  replayDeferredEvents(onlyIfSafe) {
    if (typeof onlyIfSafe != "boolean") {
      throw new Error("Must provide a boolean argument");
    }
    if (!this.#eventsQueue.length) {
      return;
    }

    let { event, callback, searchString } = this.#eventsQueue[0];
    if (onlyIfSafe && !this.isSafeToPlayDeferredEvent(event)) {
      return;
    }

    this.#eventsQueue.shift();
    if (searchString == this.#lastQuery.context.searchString) {
      callback();
    }
    Services.tm.dispatchToMainThread(() => {
      this.replayDeferredEvents(onlyIfSafe);
    });
  }

  shouldDeferEvent(event) {
    if (this.#eventsQueue.length) {
      return true;
    }

    let isMacNavigation =
      this.input.controller.platform == "macosx" &&
      event.ctrlKey &&
      this.input.view.isOpen &&
      (event.key === "n" || event.key === "p");
    if (!DEFERRED_KEY_CODES.has(event.keyCode) && !isMacNavigation) {
      return false;
    }

    if (DEFERRED_KEY_CODES.has(event.keyCode)) {
      if (this.input.editor.composing) {
        return true;
      }
      if (this.input.controller.keyEventMovesCaret(event)) {
        return false;
      }
    }

    if (
      this.#lastQuery.startDate + UrlbarEventBufferer.DEFERRING_TIMEOUT_MS <=
      ChromeUtils.now()
    ) {
      return false;
    }

    if (
      event.keyCode == KeyEvent.DOM_VK_TAB &&
      !this.input.view.isOpen &&
      !this.waitingDeferUserSelectionProviders
    ) {
      return false;
    }

    return !this.isSafeToPlayDeferredEvent(event);
  }

  get isDeferringEvents() {
    return !!this.#eventsQueue.length;
  }

  get waitingDeferUserSelectionProviders() {
    return !!this.#lastQuery.context?.deferUserSelectionProviders.size;
  }

  isSafeToPlayDeferredEvent(event) {
    if (
      this.#lastQuery.status == QUERY_STATUS.COMPLETE ||
      this.#lastQuery.status == QUERY_STATUS.UKNOWN
    ) {
      return true;
    }
    let waitingHeuristicResults =
      this.#lastQuery.status == QUERY_STATUS.RUNNING;
    if (event.keyCode == KeyEvent.DOM_VK_RETURN) {
      if (this.waitingDeferUserSelectionProviders) {
        return false;
      }
      let selectedResult = this.input.view.selectedResult;
      return (
        (selectedResult && !selectedResult.heuristic) ||
        !waitingHeuristicResults
      );
    }

    if (
      waitingHeuristicResults ||
      !this.input.view.isOpen ||
      this.waitingDeferUserSelectionProviders
    ) {
      return false;
    }

    let isMacDownNavigation =
      this.input.controller.platform == "macosx" &&
      event.ctrlKey &&
      this.input.view.isOpen &&
      event.key === "n";
    if (event.keyCode == KeyEvent.DOM_VK_DOWN || isMacDownNavigation) {
      return !this.lastResultIsSelected;
    }

    return true;
  }

  get lastResultIsSelected() {
    let results = this.#lastQuery.context.results;
    return (
      results.length &&
      results[results.length - 1] == this.input.view.selectedResult
    );
  }

  #eventsQueue = [];

  #deferringTimeout = null;

  #lastQuery;
}
