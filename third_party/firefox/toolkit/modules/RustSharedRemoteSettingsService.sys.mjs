/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { Region } from "resource://gre/modules/Region.sys.mjs";
import {
  RemoteSettingsConfig,
  RemoteSettingsContext,
  RemoteSettingsServer,
  RemoteSettingsService,
} from "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustRemoteSettings.sys.mjs";
import { Utils } from "resource://services-settings/Utils.sys.mjs";

class _SharedRemoteSettingsService {
  #config;
  #rustService;

  constructor() {
    const storageDir = PathUtils.join(
      Services.dirsvc.get("ProfLD", Ci.nsIFile).path,
      "remote-settings"
    );

    this.#config = new RemoteSettingsConfig({
      server: this.#makeServer(Utils.SERVER_URL),
      bucketName: Utils.actualBucketName("main"),
      appContext: new RemoteSettingsContext({
        formFactor: "desktop",
        appId: Services.appinfo.ID || "",
        channel: AppConstants.IS_ESR ? "esr" : AppConstants.MOZ_UPDATE_CHANNEL,
        appVersion: Services.appinfo.version,
        locale: Services.locale.appLocaleAsBCP47,
        os: AppConstants.platform,
        osVersion: Services.sysinfo.get("version"),
        country: Region.home ?? undefined,
      }),
    });

    Services.obs.addObserver(this, Region.REGION_TOPIC);
    Services.obs.addObserver(this, "intl:app-locales-changed");

    this.#rustService = RemoteSettingsService.init(storageDir, this.#config);
  }

  get country() {
    return this.#config.appContext.country;
  }

  get locale() {
    return this.#config.appContext.locale;
  }

  get server() {
    return this.#config.server;
  }

  updateServer(opts = {}) {
    this.#config.server = this.#makeServer(opts.url ?? Utils.SERVER_URL);
    this.#config.bucketName = opts.bucketName ?? Utils.actualBucketName("main");
    this.#updateConfig();
  }

  #updateConfig() {
    this.#rustService.updateConfig(this.#config);
  }

  rustService() {
    return this.#rustService;
  }

  async sync() {
    await this.#rustService.sync();
  }

  observe(subj, topic) {
    switch (topic) {
      case Region.REGION_TOPIC: {
        const newCountry = subj.data;
        if (newCountry != this.#config.appContext.country) {
          this.#config.appContext.country = newCountry;
          this.#updateConfig();
        }
        break;
      }
      case "intl:app-locales-changed": {
        const newLocale = Services.locale.appLocaleAsBCP47;
        if (newLocale != this.#config.appContext.locale) {
          this.#config.appContext.locale = newLocale;
          this.#updateConfig();
        }
        break;
      }
    }
  }

  #makeServer(url) {
    return !Utils.shouldSkipRemoteActivity || url != Utils.SERVER_URL
      ? new RemoteSettingsServer.Custom({ url })
      : null;
  }
}

export const SharedRemoteSettingsService = new _SharedRemoteSettingsService();
