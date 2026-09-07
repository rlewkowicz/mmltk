/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const gIntegrationPoints = new Map();

export var Integration = new Proxy(
  {},
  {
    get(target, name) {
      let integrationPoint = gIntegrationPoints.get(name);
      if (!integrationPoint) {
        integrationPoint = new IntegrationPoint();
        gIntegrationPoints.set(name, integrationPoint);
      }
      return integrationPoint;
    },
  }
);

var IntegrationPoint = function () {
  this._overrideFns = new Set();
  this._combined = {
    // eslint-disable-next-line mozilla/use-chromeutils-generateqi
    QueryInterface() {
      let ex = new Components.Exception(
        "Integration objects should not be used with XPCOM because" +
          " they change when new overrides are registered.",
        Cr.NS_ERROR_NO_INTERFACE
      );
      console.error(ex);
      throw ex;
    },
  };
};

IntegrationPoint.prototype = {
  _overrideFns: null,

  _combined: null,

  _combinedIsCurrent: false,

  register(overrideFn) {
    this._overrideFns.add(overrideFn);
    this._combinedIsCurrent = false;
  },

  unregister(overrideFn) {
    this._overrideFns.delete(overrideFn);
    this._combinedIsCurrent = false;
  },

  getCombined(root) {
    if (this._combinedIsCurrent) {
      return this._combined;
    }

    let overrideFnArray = [...this._overrideFns, () => this._combined];

    let combined = root;
    for (let overrideFn of overrideFnArray) {
      try {
        let override = overrideFn(combined);

        let descriptors = {};
        for (let name of Object.getOwnPropertyNames(override)) {
          descriptors[name] = Object.getOwnPropertyDescriptor(override, name);
        }
        combined = Object.create(combined, descriptors);
      } catch (ex) {
        console.error(ex);
      }
    }

    this._combinedIsCurrent = true;
    return (this._combined = combined);
  },

  defineESModuleGetter(targetObject, name, moduleUrl) {
    let moduleHolder = {};
    // eslint-disable-next-line mozilla/lazy-getter-object-name
    ChromeUtils.defineESModuleGetters(moduleHolder, {
      [name]: moduleUrl,
    });
    Object.defineProperty(targetObject, name, {
      get: () => this.getCombined(moduleHolder[name]),
      configurable: true,
      enumerable: true,
    });
  },
};
