/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  RemoteSettings: "resource://services-settings/remote-settings.sys.mjs",
  RemoteSettingsClient:
    "resource://services-settings/RemoteSettingsClient.sys.mjs",
});

const SETTINGS_DEFAULTURI_BYPASS_LIST_KEY =
  "url-parser-default-unknown-schemes-interventions";

export class SimpleURIUnknownSchemesRemoteObserver {
  #initialized = false;
  #bypassListSettings;
  classID = Components.ID("{86606ba1-de17-4df4-9013-e571ab94fd94}");
  QueryInterface = ChromeUtils.generateQI([
    "nsIObserver",
    "nsISimpleURIUnknownSchemesRemoteObserver",
  ]);

  observe(subject, topic) {
    if (topic == "profile-after-change" && !this.#initialized) {
      this.#initialized = true;
      this.#init();
    }
  }

  async #init() {
    if (!this.#bypassListSettings) {
      this.#bypassListSettings = lazy.RemoteSettings(
        SETTINGS_DEFAULTURI_BYPASS_LIST_KEY
      );
    }

    const settingsList = await this.#getBypassList();
    let schemes = settingsList.map(r => r.scheme);
    if (schemes.length) {
      Services.io.setSimpleURIUnknownRemoteSchemes(schemes);
    }

    this.#bypassListSettings.on("sync", this.#updateBypassList.bind(this));
  }

  async #updateBypassList() {
    const settingsList = await this.#getBypassList();
    let schemes = settingsList.map(r => r.scheme);
    if (schemes.length) {
      Services.io.setSimpleURIUnknownRemoteSchemes(schemes);
    }
  }

  async #getBypassList() {
    if (this._getSettingsPromise) {
      return this._getSettingsPromise;
    }

    const settings = await (this._getSettingsPromise =
      this.#getBypassListSettings());
    delete this._getSettingsPromise;
    return settings;
  }

  async #getBypassListSettings(firstTime = true) {
    let result = [];
    try {
      result = await this.#bypassListSettings.get({
        verifySignature: true,
      });
    } catch (ex) {
      if (
        ex instanceof lazy.RemoteSettingsClient.InvalidSignatureError &&
        firstTime
      ) {
        await this.#bypassListSettings.db.clear();
        return this.#getBypassListSettings(false);
      }
      console.error(ex);
    }
    return result;
  }
}
