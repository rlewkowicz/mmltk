/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  mozjexl: "resource://gre/modules/components-utils/mozjexl.sys.mjs",
  Sampling: "resource://gre/modules/components-utils/Sampling.sys.mjs",
});

function getPrefValue(prefKey, defaultValue) {
  switch (Services.prefs.getPrefType(prefKey)) {
    case Ci.nsIPrefBranch.PREF_STRING:
      return Services.prefs.getStringPref(prefKey);

    case Ci.nsIPrefBranch.PREF_INT:
      return Services.prefs.getIntPref(prefKey);

    case Ci.nsIPrefBranch.PREF_BOOL:
      return Services.prefs.getBoolPref(prefKey);

    case Ci.nsIPrefBranch.PREF_INVALID:
      return defaultValue;

    default:
      throw new Error(`Error getting pref ${prefKey}.`);
  }
}

ChromeUtils.defineLazyGetter(lazy, "jexl", () => {
  const jexl = new lazy.mozjexl.Jexl();
  jexl.addTransforms({
    date: dateString => new Date(dateString),
    stableSample: lazy.Sampling.stableSample,
    bucketSample: lazy.Sampling.bucketSample,
    preferenceValue: getPrefValue,
    preferenceIsUserSet: prefKey => Services.prefs.prefHasUserValue(prefKey),
    preferenceExists: prefKey =>
      Services.prefs.getPrefType(prefKey) != Ci.nsIPrefBranch.PREF_INVALID,
    keys,
    values,
    length,
    mapToProperty,
    regExpMatch,
    versionCompare,
  });
  jexl.addBinaryOp("intersect", 40, operatorIntersect);
  return jexl;
});

export var FilterExpressions = {
  getAvailableTransforms() {
    return Object.keys(lazy.jexl._transforms);
  },

  eval(expr, context = {}) {
    const onelineExpr = expr.replace(/[\t\n\r]/g, " ");
    return lazy.jexl.eval(onelineExpr, context);
  },
};

function keys(obj) {
  if (typeof obj !== "object" || obj === null) {
    return undefined;
  }

  return Object.keys(obj);
}

function values(obj) {
  if (typeof obj !== "object" || obj === null) {
    return undefined;
  }

  return Object.values(obj);
}

function length(arr) {
  return Array.isArray(arr) ? arr.length : undefined;
}

function mapToProperty(arr, prop) {
  return Array.isArray(arr) ? arr.map(elem => elem[prop]) : undefined;
}

function operatorIntersect(listA, listB) {
  if (!Array.isArray(listA) || !Array.isArray(listB)) {
    return undefined;
  }

  return listA.filter(item => listB.includes(item));
}

function regExpMatch(str, pattern, flags) {
  const re = new RegExp(pattern, flags);
  return str.match(re);
}

function versionCompare(v1, v2) {
  return Services.vc.compare(v1, v2);
}
