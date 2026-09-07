/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

let global = Cu.getGlobalForObject({});

const EXTRA_GLOBAL_NAME_TO_IMPORT_NAME = {
  MessagePort: "MessageChannel",
};

function redefine(object, prop, value) {
  Object.defineProperty(object, prop, {
    configurable: true,
    enumerable: true,
    value,
    writable: true,
  });
  return value;
}

export var XPCOMUtils = {
  defineLazyScriptGetter(aObject, aNames, aResource) {
    if (!Array.isArray(aNames)) {
      aNames = [aNames];
    }
    for (let name of aNames) {
      Object.defineProperty(aObject, name, {
        get() {
          XPCOMUtils._scriptloader.loadSubScript(aResource, aObject);
          return aObject[name];
        },
        set(value) {
          redefine(aObject, name, value);
        },
        configurable: true,
        enumerable: true,
      });
    }
  },

  defineLazyGlobalGetters(aObject, aNames) {
    for (let name of aNames) {
      ChromeUtils.defineLazyGetter(aObject, name, () => {
        if (!(name in global)) {
          let importName = EXTRA_GLOBAL_NAME_TO_IMPORT_NAME[name] || name;
          // eslint-disable-next-line mozilla/reject-importGlobalProperties, no-unused-vars
          Cu.importGlobalProperties([importName]);
        }
        return global[name];
      });
    }
  },

  defineLazyServiceGetter(aObject, aName, aContract, aInterface) {
    ChromeUtils.defineLazyGetter(aObject, aName, () => {
      return Cc[aContract].getService(aInterface);
    });
  },


  defineLazyServiceGetters(aObject, aServices) {
    for (let [name, service] of Object.entries(aServices)) {
      this.defineLazyServiceGetter(aObject, name, service[0], service[1]);
    }
  },

  defineLazyPreferenceGetter(
    aObject,
    aName,
    aPreference,
    aDefaultPrefValue = null,
    aOnUpdate = null,
    aTransform = val => val
  ) {
    if (AppConstants.DEBUG && aDefaultPrefValue !== null) {
      let prefType = Services.prefs.getPrefType(aPreference);
      if (prefType != Ci.nsIPrefBranch.PREF_INVALID) {
        let prefTypeForDefaultValue = {
          boolean: Ci.nsIPrefBranch.PREF_BOOL,
          number: Ci.nsIPrefBranch.PREF_INT,
          string: Ci.nsIPrefBranch.PREF_STRING,
        }[typeof aDefaultPrefValue];
        if (prefTypeForDefaultValue != prefType) {
          throw new Error(
            `Default value does not match preference type (Got ${prefTypeForDefaultValue}, expected ${prefType}) for ${aPreference}`
          );
        }
      }
    }

    let observer = {
      QueryInterface: XPCU_lazyPreferenceObserverQI,

      value: undefined,

      observe(subject, topic, data) {
        if (data == aPreference) {
          if (aOnUpdate) {
            let previous = this.value;

            this.value = undefined;
            let latest = lazyGetter();
            aOnUpdate(data, previous, latest);
          } else {
            this.value = undefined;
          }
        }
      },
    };

    let defineGetter = get => {
      Object.defineProperty(aObject, aName, {
        configurable: true,
        enumerable: true,
        get,
      });
    };

    function lazyGetter() {
      if (observer.value === undefined) {
        let prefValue;
        switch (Services.prefs.getPrefType(aPreference)) {
          case Ci.nsIPrefBranch.PREF_STRING:
            prefValue = Services.prefs.getStringPref(aPreference);
            break;

          case Ci.nsIPrefBranch.PREF_INT:
            prefValue = Services.prefs.getIntPref(aPreference);
            break;

          case Ci.nsIPrefBranch.PREF_BOOL:
            prefValue = Services.prefs.getBoolPref(aPreference);
            break;

          case Ci.nsIPrefBranch.PREF_INVALID:
            prefValue = aDefaultPrefValue;
            break;

          default:
            throw new Error(
              `Error getting pref ${aPreference}; its value's type is ` +
                `${Services.prefs.getPrefType(aPreference)}, which I don't ` +
                `know how to handle.`
            );
        }

        observer.value = aTransform(prefValue);
      }
      return observer.value;
    }

    defineGetter(() => {
      Services.prefs.addObserver(aPreference, observer, true);

      defineGetter(lazyGetter);
      return lazyGetter();
    });
  },

  defineLazy(lazy, definition, options) {
    let modules = {};

    for (let [key, val] of Object.entries(definition)) {
      if (typeof val === "string") {
        modules[key] = val;
      } else if (typeof val === "function") {
        ChromeUtils.defineLazyGetter(lazy, key, val);
      } else if ("service" in val) {
        XPCOMUtils.defineLazyServiceGetter(lazy, key, val.service, val.iid);
      } else if ("pref" in val) {
        XPCOMUtils.defineLazyPreferenceGetter(
          lazy,
          key,
          val.pref,
          val.default,
          val.onUpdate,
          val.transform
        );
      } else {
        throw new Error(`Unkown LazyDefinition for ${key}`);
      }
    }

    ChromeUtils.defineESModuleGetters(lazy, modules, options);
    return  (lazy);
  },

  declareLazy(declaration, options) {
    return XPCOMUtils.defineLazy({}, declaration, options);
  },

  defineConstant(aObj, aName, aValue) {
    Object.defineProperty(aObj, aName, {
      value: aValue,
      enumerable: true,
      writable: false,
    });
  },
};

ChromeUtils.defineLazyGetter(XPCOMUtils, "_scriptloader", () => {
  return Services.scriptloader;
});

var XPCU_lazyPreferenceObserverQI = ChromeUtils.generateQI([
  "nsIObserver",
  "nsISupportsWeakReference",
]);
