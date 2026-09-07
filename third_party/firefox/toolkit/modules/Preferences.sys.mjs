/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const MAX_INT = 0x7fffffff; 
const MIN_INT = -0x80000000;

export function Preferences(args) {
  this._cachedPrefBranch = null;
  if (isObject(args)) {
    if (args.branch) {
      this._branchStr = args.branch;
    }
    if (args.defaultBranch) {
      this._defaultBranch = args.defaultBranch;
    }
    if (args.privacyContext) {
      this._privacyContext = args.privacyContext;
    }
  } else if (args) {
    this._branchStr = args;
  }
}

Preferences.get = function (prefName, defaultValue, valueType = null) {
  if (Array.isArray(prefName)) {
    return prefName.map(v => this.get(v, defaultValue));
  }

  return this._get(prefName, defaultValue, valueType);
};

Preferences._get = function (prefName, defaultValue, valueType) {
  switch (this._prefBranch.getPrefType(prefName)) {
    case Ci.nsIPrefBranch.PREF_STRING:
      if (valueType) {
        let ifaces = ["nsIFile", "nsIPrefLocalizedString"];
        if (ifaces.includes(valueType.name)) {
          return this._prefBranch.getComplexValue(prefName, valueType).data;
        }
      }
      return this._prefBranch.getStringPref(prefName);

    case Ci.nsIPrefBranch.PREF_INT:
      return this._prefBranch.getIntPref(prefName);

    case Ci.nsIPrefBranch.PREF_BOOL:
      return this._prefBranch.getBoolPref(prefName);

    case Ci.nsIPrefBranch.PREF_INVALID:
      return defaultValue;

    default:
      throw new Error(
        `Error getting pref ${prefName}; its value's type is ` +
          `${this._prefBranch.getPrefType(prefName)}, which I don't ` +
          `know how to handle.`
      );
  }
};

Preferences.set = function (prefName, prefValue) {
  if (isObject(prefName)) {
    for (let [name, value] of Object.entries(prefName)) {
      this.set(name, value);
    }
    return;
  }

  this._set(prefName, prefValue);
};

Preferences._set = function (prefName, prefValue) {
  let prefType;
  if (typeof prefValue != "undefined" && prefValue != null) {
    prefType = prefValue.constructor.name;
  }

  switch (prefType) {
    case "String":
      this._prefBranch.setStringPref(prefName, prefValue);
      break;

    case "Number":
      if (prefValue > MAX_INT || prefValue < MIN_INT) {
        throw new Error(
          `you cannot set the ${prefName} pref to the number ` +
            `${prefValue}, as number pref values must be in the signed ` +
            `32-bit integer range -(2^31-1) to 2^31-1.  To store numbers ` +
            `outside that range, store them as strings.`
        );
      }
      this._prefBranch.setIntPref(prefName, prefValue);
      if (prefValue % 1 != 0) {
        console.error(
          "Warning: setting the ",
          prefName,
          " pref to the non-integer number ",
          prefValue,
          " converted it " +
            "to the integer number " +
            this.get(prefName) +
            "; to retain fractional precision, store non-integer " +
            "numbers as strings."
        );
      }
      break;

    case "Boolean":
      this._prefBranch.setBoolPref(prefName, prefValue);
      break;

    default:
      throw new Error(
        `can't set pref ${prefName} to value '${prefValue}'; ` +
          `it isn't a String, Number, or Boolean`
      );
  }
};

Preferences.isSet = function (prefName) {
  if (Array.isArray(prefName)) {
    return prefName.map(this.isSet, this);
  }

  return this._prefBranch.prefHasUserValue(prefName);
};

Preferences.reset = function (prefName) {
  if (Array.isArray(prefName)) {
    prefName.map(v => this.reset(v));
    return;
  }

  this._prefBranch.clearUserPref(prefName);
};

Preferences.locked = function (prefName) {
  if (Array.isArray(prefName)) {
    return prefName.map(this.locked, this);
  }

  return this._prefBranch.prefIsLocked(prefName);
};

Preferences.observe = function (prefName, callback, thisObject) {
  let fullPrefName = this._branchStr + (prefName || "");

  let observer = new PrefObserver(fullPrefName, callback, thisObject);
  Preferences._prefBranch.addObserver(fullPrefName, observer, true);
  observers.push(observer);

  return observer;
};

Preferences.ignore = function (prefName, callback, thisObject) {
  let fullPrefName = this._branchStr + (prefName || "");

  let [observer] = observers.filter(
    v =>
      v.prefName == fullPrefName &&
      v.callback == callback &&
      v.thisObject == thisObject
  );

  if (observer) {
    Preferences._prefBranch.removeObserver(fullPrefName, observer);
    observers.splice(observers.indexOf(observer), 1);
  } else {
    console.error(
      `Attempt to stop observing a preference "${prefName}" that's not being observed`
    );
  }
};

Preferences._branchStr = "";

Preferences._cachedPrefBranch = null;

Object.defineProperty(Preferences, "_prefBranch", {
  get: function _prefBranch() {
    if (!this._cachedPrefBranch) {
      let prefSvc = Services.prefs;
      this._cachedPrefBranch = this._defaultBranch
        ? prefSvc.getDefaultBranch(this._branchStr)
        : prefSvc.getBranch(this._branchStr);
    }
    return this._cachedPrefBranch;
  },
  enumerable: true,
  configurable: true,
});

Preferences.prototype = Preferences;

var observers = [];

function PrefObserver(prefName, callback, thisObject) {
  this.prefName = prefName;
  this.callback = callback;
  this.thisObject = thisObject;
}

PrefObserver.prototype = {
  QueryInterface: ChromeUtils.generateQI([
    "nsIObserver",
    "nsISupportsWeakReference",
  ]),

  observe(subject, topic, data) {
    if (data != this.prefName) {
      return;
    }

    if (typeof this.callback == "function") {
      let prefValue = Preferences.get(this.prefName);

      if (this.thisObject) {
        this.callback.call(this.thisObject, prefValue);
      } else {
        this.callback(prefValue);
      }
    } else {
      this.callback.observe(subject, topic, data);
    }
  },
};

function isObject(val) {
  return (
    typeof val != "undefined" &&
    val != null &&
    typeof val == "object" &&
    val.constructor.name == "Object"
  );
}
