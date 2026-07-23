/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


var gNextId = 1; 

var gTimerTable = new Map();

var gIdleTable = new Map();

var setTimeout_timerCallbackQI = ChromeUtils.generateQI([
  "nsITimerCallback",
  "nsINamed",
]);

function _setTimeoutOrIsInterval(
  callback,
  milliseconds,
  isInterval,
  target,
  args
) {
  if (typeof callback !== "function") {
    throw new Error(
      `callback is not a function in ${
        isInterval ? "setInterval" : "setTimeout"
      }`
    );
  }
  let id = gNextId++;
  let timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);

  if (target) {
    timer.target = target;
  }

  let callbackObj = {
    QueryInterface: setTimeout_timerCallbackQI,

    notify() {
      if (!isInterval) {
        gTimerTable.delete(id);
      }
      callback.apply(null, args);
    },

    get name() {
      return `${
        isInterval ? "setInterval" : "setTimeout"
      }() for ${Cu.getDebugName(callback)}`;
    },
  };

  timer.initWithCallback(
    callbackObj,
    milliseconds,
    isInterval ? timer.TYPE_REPEATING_SLACK : timer.TYPE_ONE_SHOT
  );

  gTimerTable.set(id, timer);
  return id;
}

export function setTimeout(callback, milliseconds, ...args) {
  return _setTimeoutOrIsInterval(callback, milliseconds, false, null, args);
}

export function setTimeoutWithTarget(callback, milliseconds, target, ...args) {
  return _setTimeoutOrIsInterval(callback, milliseconds, false, target, args);
}

export function setInterval(callback, milliseconds, ...args) {
  return _setTimeoutOrIsInterval(callback, milliseconds, true, null, args);
}

export function setIntervalWithTarget(callback, milliseconds, target, ...args) {
  return _setTimeoutOrIsInterval(callback, milliseconds, true, target, args);
}

function clear(id) {
  if (gTimerTable.has(id)) {
    gTimerTable.get(id).cancel();
    gTimerTable.delete(id);
  }
}

export var clearInterval = clear;

export var clearTimeout = clear;

export function requestIdleCallback(callback, options) {
  if (typeof callback !== "function") {
    throw new Error("callback is not a function in requestIdleCallback");
  }
  let id = gNextId++;

  ChromeUtils.idleDispatch(deadline => {
    if (gIdleTable.has(id)) {
      gIdleTable.delete(id);
      callback(deadline);
    }
  }, options);
  gIdleTable.set(id, callback);
  return id;
}

export function cancelIdleCallback(id) {
  if (gIdleTable.has(id)) {
    gIdleTable.delete(id);
  }
}
