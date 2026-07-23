/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  RemoteSettings: "resource://services-settings/remote-settings.sys.mjs",
  RemoteSettingsClient:
    "resource://services-settings/RemoteSettingsClient.sys.mjs",
});

export const ESSENTIAL_DOMAINS_REMOTE_BUCKET = "moz-essential-domain-fallbacks";

export class EssentialDomainsRemoteSettings {
  #initialized = false;
  #fallbackDomains;
  classID = Components.ID("{962dbf40-2c3f-4c1f-8ae8-90e8c9d85368}");
  QueryInterface = ChromeUtils.generateQI(["nsIObserver"]);

  observe(subject, topic) {
    if (topic == "profile-after-change" || !this.#initialized) {
      this.#initialized = true;
      this.#init();
    }
  }

  async #init() {
    if (!this.#fallbackDomains) {
      this.#fallbackDomains = lazy.RemoteSettings(
        ESSENTIAL_DOMAINS_REMOTE_BUCKET
      );
    }

    const settingsList = await this.#getFallbackList();
    for (let setting of settingsList) {
      Services.io.addEssentialDomainMapping(setting.from, setting.to);
    }

    this.#fallbackDomains.on("sync", this.#updateFallbackDomains.bind(this));
  }

  async #updateFallbackDomains() {
    Services.io.clearEssentialDomainMapping();

    const settingsList = await this.#getFallbackList();
    for (let setting of settingsList) {
      Services.io.addEssentialDomainMapping(setting.from, setting.to);
    }
  }

  async #getFallbackList() {
    if (this._getSettingsPromise) {
      return this._getSettingsPromise;
    }

    const settings = await (this._getSettingsPromise =
      this.#getFallbackDomains());
    delete this._getSettingsPromise;
    return settings;
  }

  async #getFallbackDomains(firstTime = true) {
    let result = [];
    try {
      result = await this.#fallbackDomains.get({
        verifySignature: true,
      });
    } catch (ex) {
      if (
        ex instanceof lazy.RemoteSettingsClient.InvalidSignatureError &&
        firstTime
      ) {
        await this.#fallbackDomains.db.clear();
        return this.#getFallbackDomains(false);
      }
      console.error(ex);
    }
    return result;
  }
}
