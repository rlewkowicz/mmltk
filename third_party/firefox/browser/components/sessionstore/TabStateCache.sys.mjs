/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

export var TabStateCache = Object.freeze({
  get(permanentKey) {
    return TabStateCacheInternal.get(permanentKey);
  },

  update(permanentKey, newData) {
    TabStateCacheInternal.update(permanentKey, newData);
  },
});

var TabStateCacheInternal = {
  _data: new WeakMap(),

  get(permanentKey) {
    return this._data.get(permanentKey);
  },

  updatePartialStorageChange(data, change) {
    if (!data.storage) {
      data.storage = {};
    }

    let storage = data.storage;
    for (let domain of Object.keys(change)) {
      if (!change[domain]) {
        delete storage[domain];
      } else {
        for (let key of Object.keys(change[domain])) {
          let value = change[domain][key];
          if (value === null) {
            if (storage[domain] && storage[domain][key]) {
              delete storage[domain][key];
            }
          } else {
            if (!storage[domain]) {
              storage[domain] = {};
            }
            storage[domain][key] = value;
          }
        }
      }
    }
  },

  updatePartialHistoryChange(data, change) {
    const kLastIndex = Number.MAX_SAFE_INTEGER - 1;

    if (!data.history) {
      data.history = { entries: [] };
    }

    let history = data.history;
    for (let key of Object.keys(change)) {
      if (key == "entries") {
        if (change.fromIdx != kLastIndex) {
          let start = change.fromIdx + 1;
          history.entries.splice(start, Infinity, ...change.entries);
        }
      } else if (key != "fromIdx") {
        history[key] = change[key];
      }
    }
  },

  update(permanentKey, newData) {
    let data = this._data.get(permanentKey) || {};

    for (let key of Object.keys(newData)) {
      if (key == "storagechange") {
        this.updatePartialStorageChange(data, newData.storagechange);
        continue;
      }

      if (key == "historychange") {
        this.updatePartialHistoryChange(data, newData.historychange);
        continue;
      }

      let value = newData[key];
      if (value === null) {
        delete data[key];
      } else {
        data[key] = value;
      }
    }

    this._data.set(permanentKey, data);
  },
};
