/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  RemoteSettings: "resource://services-settings/remote-settings.sys.mjs",
  RemoteSettingsClient:
    "resource://services-settings/RemoteSettingsClient.sys.mjs",
});


const SETTINGS_IGNORELIST_KEY = "hijack-blocklists";

class IgnoreListsManager {
  #ignoreListSettings;

  #init() {
    if (!this.#ignoreListSettings) {
      this.#ignoreListSettings = lazy.RemoteSettings(SETTINGS_IGNORELIST_KEY);
    }
  }

  async getAndSubscribe(listener) {
    this.#init();

    const settings = await this.#getIgnoreList();

    this.#ignoreListSettings.on("sync", listener);

    return settings;
  }

  unsubscribe(listener) {
    if (!this.#ignoreListSettings) {
      return;
    }

    this.#ignoreListSettings.off("sync", listener);
  }

  #getSettingsPromise;

  async #getIgnoreList() {
    if (this.#getSettingsPromise) {
      return this.#getSettingsPromise;
    }

    const settings = await (this.#getSettingsPromise =
      this.#getIgnoreListSettings());
    this.#getSettingsPromise = undefined;
    return settings;
  }

  async #getIgnoreListSettings(firstTime = true) {
    let result = [];
    try {
      result = await this.#ignoreListSettings.get({
        verifySignature: true,
      });
    } catch (ex) {
      if (
        ex instanceof lazy.RemoteSettingsClient.InvalidSignatureError &&
        firstTime
      ) {
        await this.#ignoreListSettings.db.clear();
        return this.#getIgnoreListSettings(false);
      }
      console.error(ex);
    }
    return result;
  }
}

export const IgnoreLists = new IgnoreListsManager();
