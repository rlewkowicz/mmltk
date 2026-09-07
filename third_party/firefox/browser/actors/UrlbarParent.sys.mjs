/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  UrlbarParentController:
    "moz-src:///browser/components/urlbar/UrlbarParentController.sys.mjs",
  UrlbarQueryContext:
    "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
});


export class UrlbarParent extends JSWindowActorParent {
  #messageControllers = new Map();

  receiveMessage(message) {
    let { instanceId } = message.data;

    if (message.name == "Init") {
      let { sapName, isPrivate } = message.data;
      let controller = new lazy.UrlbarParentController({
        sapName,
        isPrivate,
        actor: this,
      });
      controller.setChild(new UrlbarChildControllerProxy(this, instanceId));
      this.#messageControllers.set(instanceId, controller);
      return undefined;
    }

    if (message.name == "Destroy") {
      this.#messageControllers.delete(instanceId);
      return undefined;
    }

    let controller = this.#messageControllers.get(instanceId);
    if (!controller) {
      return undefined;
    }

    switch (message.name) {
      case "GetViewUpdate":
        return controller.getViewUpdate(
          lazy.UrlbarResult.fromWire(message.data.result),
          message.data.idsByName
        );
      case "GetHeuristicResult":
        return controller
          .getHeuristicResult(
            lazy.UrlbarQueryContext.fromWire(message.data.queryContext)
          )
          .then(result => result?.toWire() ?? null);
      case "StartQuery":
        return controller
          .startQuery(
            lazy.UrlbarQueryContext.fromWire(message.data.queryContext)
          )
          .then(context => context.toWire());
      case "CancelQuery":
        controller.cancelQuery();
        break;
      case "SpeculativeConnect":
        controller.speculativeConnect(
          lazy.UrlbarResult.fromWire(message.data.result),
          lazy.UrlbarQueryContext.fromWire(message.data.queryContext),
          message.data.reason
        );
        break;
      case "RemoveResult":
        controller.removeResult(
          lazy.UrlbarResult.fromWire(message.data.result)
        );
        break;
      case "SetLastQueryContextCache":
        controller.setLastQueryContextCache(
          lazy.UrlbarQueryContext.fromWire(message.data.queryContext)
        );
        break;
      case "ClearLastQueryContextCache":
        controller.clearLastQueryContextCache();
        break;
      case "OnBeforeSelection":
        controller.onBeforeSelection(
          lazy.UrlbarResult.fromWire(message.data.result)
        );
        break;
      case "OnSelection":
        controller.onSelection(lazy.UrlbarResult.fromWire(message.data.result));
        break;
    }
    return undefined;
  }
}

class UrlbarChildControllerProxy {
  #actor;

  #instanceId;

  constructor(actor, instanceId) {
    this.#actor = actor;
    this.#instanceId = instanceId;
  }

  notify(name, ...params) {
    this.#actor.sendAsyncMessage("Notify", {
      instanceId: this.#instanceId,
      name,
      params: params.map(param =>
        param instanceof lazy.UrlbarQueryContext
          ? { serializedQueryContext: param.toWire() }
          : param
      ),
    });
  }
}
