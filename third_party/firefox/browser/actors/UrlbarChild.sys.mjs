/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarQueryContext:
    "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
});


export class UrlbarChild extends JSWindowActorChild {
  #nextInstanceId = 0;

  #childControllers = new Map();

  #destroyRegistry = new FinalizationRegistry(instanceId => {
    this.#childControllers.delete(instanceId);
    try {
      this.sendAsyncMessage("Destroy", { instanceId });
    } catch (ex) {
    }
  });

  get usesMessagePath() {
    return (
      !this.manager.parentActor ||
      lazy.UrlbarPrefs.get("ipc.chromeMessagePassing")
    );
  }

  registerMessagePathInput(input) {
    let instanceId = ++this.#nextInstanceId;
    this.#destroyRegistry.register(input, instanceId);
    return instanceId;
  }

  registerChildController(instanceId, child) {
    this.#childControllers.set(instanceId, new WeakRef(child));
  }

  receiveMessage(message) {
    if (message.name != "Notify") {
      return;
    }
    let { instanceId, name, params } = message.data;
    let child = this.#childControllers.get(instanceId)?.deref();
    if (!child) {
      this.#childControllers.delete(instanceId);
      return;
    }
    let deserialized = params.map(param =>
      param?.serializedQueryContext
        ? lazy.UrlbarQueryContext.fromWire(param.serializedQueryContext)
        : param
    );
    if (name == "onQueryResults") {
      let queryContext = deserialized[0];
      if (
        queryContext.firstResultChanged &&
        child.input.onFirstResult(queryContext.results[0])
      ) {
        return;
      }
    }
    child.notify(name, ...deserialized);
  }
}
