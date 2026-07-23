/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import {
  AsyncSetting,
  AsyncSettingHandler,
} from "chrome://global/content/preferences/AsyncSetting.mjs";
import { Preferences } from "chrome://global/content/preferences/Preferences.mjs";

















const { EventEmitter } = ChromeUtils.importESModule(
  "resource://gre/modules/EventEmitter.sys.mjs"
);

class PreferenceNotAddedError extends Error {
  constructor(settingId, prefId) {
    super(
      `Setting "${settingId}" was unable to find Preference "${prefId}". Did you register it with Preferences.add/addAll?`
    );
    this.name = "PreferenceNotAddedError";
    this.settingId = settingId;
    this.prefId = prefId;
  }
}

export class Setting extends EventEmitter {
  _pref;

  _deps;

  config;

  get pref() {
    return this._pref;
  }

  set pref(newPref) {
    if (this._pref) {
      this._pref.off("change", this.onChange);
    }

    this._pref = newPref;

    if (this._pref) {
      this._pref.on("change", this.onChange);
    }
  }

  constructor(id, config) {
    super();

    let configObj;

    if (Object.getPrototypeOf(config) == AsyncSetting) {
      configObj = new AsyncSettingHandler(
        id,
         (config)
      );
    } else {
      configObj = config;
    }

    this.id = id;
    this.config = configObj;
    this.pref = configObj.pref && Preferences.get(configObj.pref);
    if (configObj.pref && !this.pref) {
      throw new PreferenceNotAddedError(id, configObj.pref);
    }
    this._emitting = false;

    if (typeof this.config.setup === "function") {
      this._teardown = this.config.setup(this.onChange, this.deps, this);
    }
  }

  onChange = () => {
    if (this._emitting) {
      return;
    }
    this._emitting = true;
    this.emit("change");
    this._emitting = false;
  };

  get deps() {
    if (this._deps) {
      return this._deps;
    }
    const deps = {};

    if (this.config.deps) {
      for (let id of this.config.deps) {
        const setting = Preferences.getSetting(id);
        if (setting) {
          deps[id] = setting;
        }
      }
    }
    this._deps = deps;

    for (const setting of Object.values(this._deps)) {
      setting.on("change", this.onChange);
    }

    return this._deps;
  }

  get value() {
    let prefVal = this.pref?.value;
    if (this.config.get) {
      return this.config.get(prefVal, this.deps, this);
    }
    return prefVal;
  }

  set value(val) {
    let newVal = this.config.set ? this.config.set(val, this.deps, this) : val;
    if (this.pref && !(newVal instanceof Object && "then" in newVal)) {
      this.pref.value = newVal;
    }
  }

  get locked() {
    return this.pref?.locked ?? false;
  }

  get visible() {
    return this.config.visible ? this.config.visible(this.deps, this) : true;
  }

  get disabled() {
    return this.config.disabled ? this.config.disabled(this.deps, this) : false;
  }

  getControlConfig(config) {
    if (this.config.getControlConfig) {
      return this.config.getControlConfig(config, this.deps, this);
    }
    return config;
  }

  userClick(event) {
    if (this.config.onUserClick) {
      this.config.onUserClick(event, this.deps, this);
    }
  }

  messageBarDismiss(event) {
    if (this.config.onMessageBarDismiss) {
      this.config.onMessageBarDismiss(event, this.deps, this);
    }
  }

  userReorder(event) {
    if (this.config.onUserReorder) {
      this.config.onUserReorder(event, this.deps, this);
    }
  }

  userChange(val) {
    this.value = val;
    if (this.config.onUserChange) {
      this.config.onUserChange(val, this.deps, this);
    }
  }

  destroy() {
    if (typeof this._teardown === "function") {
      this._teardown();
      this._teardown = null;
    }

    if (this.pref) {
      this.pref.off("change", this.onChange);
    }

  }
}
