/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const EXPORTED_METHODS = [
  "getState",
  "clear",
  "get",
  "set",
  "delete",
  "setFromState",
];

export function GlobalState() {
  let internal = new GlobalStateInternal();
  let external = {};
  for (let method of EXPORTED_METHODS) {
    external[method] = internal[method].bind(internal);
  }
  return Object.freeze(external);
}

function GlobalStateInternal() {
  this.state = {};
}

GlobalStateInternal.prototype = {
  getState() {
    return this.state;
  },

  clear() {
    this.state = {};
  },

  get(aKey) {
    return this.state[aKey] || "";
  },

  set(aKey, aStringValue) {
    this.state[aKey] = aStringValue;
  },

  delete(aKey) {
    delete this.state[aKey];
  },

  setFromState(aState) {
    this.state = (aState && aState.global) || {};
  },
};
