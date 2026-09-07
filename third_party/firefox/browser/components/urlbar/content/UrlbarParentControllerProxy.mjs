/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  UrlbarQueryContext:
    "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
});


export class UrlbarParentControllerProxy {
  #actor;

  #instanceId;

  #lastQueryContextWrapper = null;

  constructor(actor, instanceId, { sapName, isPrivate }) {
    this.#actor = actor;
    this.#instanceId = instanceId;
    this.#actor.sendAsyncMessage("Init", { instanceId, sapName, isPrivate });
  }

  setChild(child) {
    this.#actor.registerChildController(this.#instanceId, child);
  }





  get _lastQueryContextWrapper() {
    return this.#lastQueryContextWrapper;
  }

  startQuery(queryContext) {
    this.#lastQueryContextWrapper = { queryContext };
    return this.#actor
      .sendQuery("StartQuery", {
        instanceId: this.#instanceId,
        queryContext: queryContext.toWire(),
      })
      .then(
        wire => lazy.UrlbarQueryContext.fromWire(wire),
        error => {
          if (error?.name == "AbortError") {
            return queryContext;
          }
          throw error;
        }
      );
  }

  async getHeuristicResult(queryContext) {
    let wire = await this.#actor.sendQuery("GetHeuristicResult", {
      instanceId: this.#instanceId,
      queryContext: queryContext.toWire(),
    });
    return wire ? lazy.UrlbarResult.fromWire(wire) : null;
  }

  cancelQuery() {
    this.#actor.sendAsyncMessage("CancelQuery", {
      instanceId: this.#instanceId,
    });
  }

  speculativeConnect(result, context, reason) {
    this.#actor.sendAsyncMessage("SpeculativeConnect", {
      instanceId: this.#instanceId,
      result: result.toWire(),
      queryContext: context.toWire(),
      reason,
    });
  }

  removeResult(result) {
    this.#actor.sendAsyncMessage("RemoveResult", {
      instanceId: this.#instanceId,
      result: result.toWire(),
    });
  }

  setLastQueryContextCache(queryContext) {
    this.#lastQueryContextWrapper = { queryContext };
    this.#actor.sendAsyncMessage("SetLastQueryContextCache", {
      instanceId: this.#instanceId,
      queryContext: queryContext.toWire(),
    });
  }

  clearLastQueryContextCache() {
    this.#lastQueryContextWrapper = null;
    this.#actor.sendAsyncMessage("ClearLastQueryContextCache", {
      instanceId: this.#instanceId,
    });
  }

  onBeforeSelection(result) {
    this.#actor.sendAsyncMessage("OnBeforeSelection", {
      instanceId: this.#instanceId,
      result: result.toWire(),
    });
  }

  onSelection(result) {
    this.#actor.sendAsyncMessage("OnSelection", {
      instanceId: this.#instanceId,
      result: result.toWire(),
    });
  }

  getViewUpdate(result, idsByName) {
    return this.#actor.sendQuery("GetViewUpdate", {
      instanceId: this.#instanceId,
      result: result.toWire(),
      idsByName,
    });
  }
}
