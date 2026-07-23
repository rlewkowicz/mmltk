/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


export const SettingGroupManager = {
  _data: new Map(),

  _onRegisterListeners: new Set(),

  get(id) {
    if (!this._data.has(id)) {
      throw new Error(`Setting group "${id}" not found`);
    }
    return this._data.get(id);
  },

  has(id) {
    return this._data.has(id);
  },

  onRegister(callback) {
    this._onRegisterListeners.add(callback);
    return () => this._onRegisterListeners.delete(callback);
  },

  registerGroup(id, config) {
    if (this._data.has(id)) {
      throw new Error(`Setting group "${id}" already registered`);
    }
    this._data.set(id, config);
    for (let callback of this._onRegisterListeners) {
      try {
        callback(id);
      } catch (ex) {
        console.error("Error notifying SettingGroupManager listener", ex);
      }
    }
  },

  registerGroups(groupConfigs) {
    for (let id in groupConfigs) {
      this.registerGroup(id, groupConfigs[id]);
    }
  },
};
