/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Copyright (c) 2009 Thomas Robinson <280north.com>
// MIT license: http://opensource.org/licenses/MIT


var pSlice = Array.prototype.slice;

export var ObjectUtils = {
  deepEqual(a, b) {
    return _deepEqual(a, b);
  },

  isEmpty(obj) {
    if (!obj) {
      return true;
    }
    if (typeof obj != "object") {
      return false;
    }
    if (Array.isArray(obj)) {
      return !obj.length;
    }
    for (let key in obj) {
      return false;
    }
    return true;
  },
};

// ... Start of previously MIT-licensed code.
// Copyright (c) 2009 Thomas Robinson <280north.com>
// MIT license: http://opensource.org/licenses/MIT

function _deepEqual(a, b) {

  if (a === b) {
    return true;
  }
  let aIsDate = instanceOf(a, "Date");
  let bIsDate = instanceOf(b, "Date");
  if (aIsDate || bIsDate) {
    if (!aIsDate || !bIsDate) {
      return false;
    }
    if (isNaN(a.getTime()) && isNaN(b.getTime())) {
      return true;
    }
    return a.getTime() === b.getTime();
  }
  let aIsRegExp = instanceOf(a, "RegExp");
  let bIsRegExp = instanceOf(b, "RegExp");
  if (aIsRegExp || bIsRegExp) {
    return (
      aIsRegExp &&
      bIsRegExp &&
      a.source === b.source &&
      a.global === b.global &&
      a.multiline === b.multiline &&
      a.lastIndex === b.lastIndex &&
      a.ignoreCase === b.ignoreCase
    );
  }
  if (typeof a != "object" || typeof b != "object") {
    return a == b;
  }
  return objEquiv(a, b);
}

function instanceOf(object, type) {
  return Object.prototype.toString.call(object) == "[object " + type + "]";
}

function isUndefinedOrNull(value) {
  return value === null || value === undefined;
}

function isArguments(object) {
  return instanceOf(object, "Arguments");
}

function objEquiv(a, b) {
  if (isUndefinedOrNull(a) || isUndefinedOrNull(b)) {
    return false;
  }
  if ((a.prototype || undefined) != (b.prototype || undefined)) {
    return false;
  }

  if (instanceOf(a, "ArrayBuffer") && instanceOf(b, "ArrayBuffer")) {
    if (a.byteLength !== b.byteLength) {
      return false;
    }
    const viewA = new Uint8Array(a);
    const viewB = new Uint8Array(b);
    for (let i = 0; i < viewA.length; i++) {
      if (viewA[i] !== viewB[i]) {
        return false;
      }
    }
    return true;
  }

  if (isArguments(a)) {
    if (!isArguments(b)) {
      return false;
    }
    a = pSlice.call(a);
    b = pSlice.call(b);
    return _deepEqual(a, b);
  }
  let ka, kb;
  try {
    ka = Object.keys(a);
    kb = Object.keys(b);
  } catch (e) {
    return false;
  }
  if (ka.length != kb.length) {
    return false;
  }
  ka.sort();
  kb.sort();
  for (let key of ka) {
    if (!_deepEqual(a[key], b[key])) {
      return false;
    }
  }
  return true;
}

// ... End of previously MIT-licensed code.
