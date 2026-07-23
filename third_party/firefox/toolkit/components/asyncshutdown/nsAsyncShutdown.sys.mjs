/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AsyncShutdown: "resource://gre/modules/AsyncShutdown.sys.mjs",
});

class PropertyBagConverter {
  get primitiveProperty() {
    return "PropertyBagConverter_primitive";
  }

  propertyBagToJsValue(bag) {
    if (!(bag instanceof Ci.nsIPropertyBag)) {
      return null;
    }
    let result = {};
    for (let { name, value: property } of bag.enumerator) {
      let value = this.#toValue(property);
      if (name == this.primitiveProperty) {
        return value;
      }
      result[name] = value;
    }
    return result;
  }

  #toValue(property) {
    if (property instanceof Ci.nsIPropertyBag) {
      return this.propertyBagToJsValue(property);
    }
    if (["number", "boolean"].includes(typeof property)) {
      return property;
    }
    try {
      return JSON.parse(property);
    } catch (ex) {
    }
    return property;
  }

  jsValueToPropertyBag(val) {
    let bag = Cc["@mozilla.org/hash-property-bag;1"].createInstance(
      Ci.nsIWritablePropertyBag
    );
    if (val && typeof val == "object") {
      for (let k of Object.keys(val)) {
        bag.setProperty(k, this.#fromValue(val[k]));
      }
    } else {
      bag.setProperty(this.primitiveProperty, this.#fromValue(val));
    }
    return bag;
  }

  #fromValue(value) {
    if (typeof value == "function") {
      return "(function)";
    }
    if (value === undefined) {
      value = null;
    }
    if (["number", "boolean", "string"].includes(typeof value)) {
      return value;
    }
    return JSON.stringify(value);
  }
}

function nsAsyncShutdownClient(moduleClient) {
  if (!moduleClient) {
    throw new TypeError("nsAsyncShutdownClient expects one argument");
  }
  this._moduleClient = moduleClient;
  this._byXpcomBlocker = new Map();
}
nsAsyncShutdownClient.prototype = {
  get jsclient() {
    return this._moduleClient;
  },
  get name() {
    return this._moduleClient.name;
  },
  get isClosed() {
    return this._moduleClient.isClosed;
  },
  addBlocker(
     xpcomBlocker,
    fileName,
    lineNumber,
    stack
  ) {

    if (this._byXpcomBlocker.has(xpcomBlocker)) {
      throw new Error(
        `We have already registered the blocker (${xpcomBlocker.name})`
      );
    }

    const moduleBlocker = () =>
      new Promise(
        () => xpcomBlocker.blockShutdown(this)
      );

    this._byXpcomBlocker.set(xpcomBlocker, moduleBlocker);
    this._moduleClient.addBlocker(xpcomBlocker.name, moduleBlocker, {
      fetchState: () =>
        new PropertyBagConverter().propertyBagToJsValue(xpcomBlocker.state),
      filename: fileName,
      lineNumber,
      stack,
    });
  },

  removeBlocker(xpcomBlocker) {
    let moduleBlocker = this._byXpcomBlocker.get(xpcomBlocker);
    if (!moduleBlocker) {
      return false;
    }
    this._byXpcomBlocker.delete(xpcomBlocker);
    return this._moduleClient.removeBlocker(moduleBlocker);
  },

  QueryInterface: ChromeUtils.generateQI(["nsIAsyncShutdownClient"]),
};

function nsAsyncShutdownBarrier(moduleBarrier) {
  this._client = new nsAsyncShutdownClient(moduleBarrier.client);
  this._moduleBarrier = moduleBarrier;
}
nsAsyncShutdownBarrier.prototype = {
  get state() {
    return new PropertyBagConverter().jsValueToPropertyBag(
      this._moduleBarrier.state
    );
  },
  get client() {
    return this._client;
  },
  wait(onReady) {
    this._moduleBarrier.wait().then(() => {
      onReady.done();
    });
  },

  QueryInterface: ChromeUtils.generateQI(["nsIAsyncShutdownBarrier"]),
};

export function nsAsyncShutdownService() {

  for (let _k of [
    "appShutdownConfirmed",
    "profileBeforeChange",
    "profileChangeTeardown",
    "sendTelemetry",

    "contentChildShutdown",

    "webWorkersShutdown",
    "xpcomWillShutdown",
  ]) {
    let k = _k;
    Object.defineProperty(this, k, {
      configurable: true,
      get() {
        delete this[k];
        let wrapped = lazy.AsyncShutdown[k]; 
        let result = wrapped ? new nsAsyncShutdownClient(wrapped) : undefined;
        Object.defineProperty(this, k, {
          value: result,
        });
        return result;
      },
    });
  }

  this.wrappedJSObject = {
    _propertyBagConverter: PropertyBagConverter,
  };
}

nsAsyncShutdownService.prototype = {
  makeBarrier(name) {
    return new nsAsyncShutdownBarrier(new lazy.AsyncShutdown.Barrier(name));
  },

  QueryInterface: ChromeUtils.generateQI(["nsIAsyncShutdownService"]),
};
