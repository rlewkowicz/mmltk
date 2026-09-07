/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const { EventEmitter } = ChromeUtils.importESModule(
  "resource://gre/modules/EventEmitter.sys.mjs"
);


export class AsyncSetting extends EventEmitter {
  static id = "";

  static controllingExtensionInfo;

  defaultValue = "";
  defaultDisabled = false;
  defaultVisible = true;
  defaultGetControlConfig = {};

  emitChange = () => {
    this.emit("change");
  };

  setup() {}

  beforeRefresh() {}

  async get() {}

  // eslint-disable-next-line no-unused-vars
  async set(value) {}

  async disabled() {
    return false;
  }

  async visible() {
    return true;
  }

  async getControlConfig() {
    return {};
  }

  // eslint-disable-next-line no-unused-vars
  onUserChange(value) {}

  // eslint-disable-next-line no-unused-vars
  onUserClick(event) {}

  //eslint-disable-next-line no-unused-vars
  onMessageBarDismiss(event) {}

  //eslint-disable-next-line no-unused-vars
  onUserReorder(event) {}
}

export class AsyncSettingHandler {
  asyncSetting;

  #emitChange;

  pref;

  deps = [];

  controllingExtensionInfo;

  constructor(id, AsyncSettingClass) {
    this.asyncSetting = new AsyncSettingClass();
    this.id = id;
    this.controllingExtensionInfo = AsyncSettingClass.controllingExtensionInfo;
    this.#emitChange = () => {};

    this.cachedValue = this.asyncSetting.defaultValue;
    this.cachedDisabled = this.asyncSetting.defaultDisabled;
    this.cachedVisible = this.asyncSetting.defaultVisible;
    this.cachedGetControlConfig = this.asyncSetting.defaultGetControlConfig;

    this.asyncSetting.on("change", () => this.refresh());
  }

  setup(emitChange) {
    let teardown = this.asyncSetting.setup();

    this.#emitChange = emitChange;

    this.refresh();
    return teardown;
  }

  async refresh() {
    this.asyncSetting.beforeRefresh();
    [
      this.cachedValue,
      this.cachedDisabled,
      this.cachedVisible,
      this.cachedGetControlConfig,
    ] = await Promise.all([
      this.asyncSetting.get(),
      this.asyncSetting.disabled(),
      this.asyncSetting.visible(),
      this.asyncSetting.getControlConfig(),
    ]);
    this.#emitChange();
  }

  get() {
    return this.cachedValue;
  }

  set(value) {
    return this.asyncSetting.set(value);
  }

  disabled() {
    return this.cachedDisabled;
  }

  visible() {
    return this.cachedVisible;
  }

  getControlConfig(config) {
    return {
      ...config,
      ...this.cachedGetControlConfig,
    };
  }

  onUserChange(value) {
    return this.asyncSetting.onUserChange(value);
  }

  onUserClick(event) {
    this.asyncSetting.onUserClick(event);
  }

  onMessageBarDismiss(event) {
    this.asyncSetting.onMessageBarDismiss(event);
  }

  onUserReorder(event) {
    this.asyncSetting.onUserReorder(event);
  }
}
