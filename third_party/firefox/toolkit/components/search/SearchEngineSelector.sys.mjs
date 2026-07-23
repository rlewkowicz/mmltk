/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  RemoteSettings: "resource://services-settings/remote-settings.sys.mjs",
  SearchDeviceType:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSearch.sys.mjs",
  SearchEngineSelector:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSearch.sys.mjs",
  SearchUserEnvironment:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSearch.sys.mjs",
  SearchApplicationName:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSearch.sys.mjs",
  SearchUpdateChannel:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSearch.sys.mjs",
  SearchUtils: "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  logConsole: () =>
    console.createInstance({
      prefix: "SearchEngineSelector",
      maxLogLevel: lazy.SearchUtils.loggingEnabled ? "Debug" : "Warn",
    }),
});

export class SearchEngineSelector {
  constructor(listener) {
    this.#remoteConfig = lazy.RemoteSettings(lazy.SearchUtils.SETTINGS_KEY);
    this.#remoteConfigOverrides = lazy.RemoteSettings(
      lazy.SearchUtils.SETTINGS_OVERRIDES_KEY
    );
    this.#boundOnConfigurationUpdated = this._onConfigurationUpdated.bind(this);
    this.#boundOnConfigurationOverridesUpdated =
      this._onConfigurationOverridesUpdated.bind(this);
    this.#changeListener = listener;
  }

  reset() {
    if (this.#listenerAdded) {
      this.#remoteConfig.off("sync", this.#boundOnConfigurationUpdated);
      this.#remoteConfigOverrides.off(
        "sync",
        this.#boundOnConfigurationOverridesUpdated
      );
      this.#listenerAdded = false;
    }
  }

  clearCachedConfigurationForTests() {
    this.#configuration = null;
  }

  async getEngineConfiguration() {
    if (this.#getConfigurationPromise) {
      return this.#getConfigurationPromise;
    }

    this.#getConfigurationPromise = Promise.all([
      this.#getConfiguration(),
      this.#getConfigurationOverrides(),
    ]);
    let remoteSettingsData = await this.#getConfigurationPromise;
    this.#configuration = remoteSettingsData[0];
    this.#getConfigurationPromise = null;

    if (!this.#configuration?.length) {
      throw new Error("Failed to get engine data from Remote Settings");
    }

    if (!this.#listenerAdded) {
      this.#remoteConfig.on("sync", this.#boundOnConfigurationUpdated);
      this.#remoteConfigOverrides.on(
        "sync",
        this.#boundOnConfigurationOverridesUpdated
      );
      this.#listenerAdded = true;
    }

    this.#selector.setSearchConfig(
      JSON.stringify({ data: this.#configuration })
    );
    this.#selector.setConfigOverrides(
      JSON.stringify({ data: remoteSettingsData[1] })
    );

    return this.#configuration;
  }

  async findContextualSearchEngineByHost(host) {
    for (let config of this.#configuration) {
      if (config.recordType !== "engine") {
        continue;
      }
      let searchHost = new URL(config.base.urls.search.base).hostname;
      if (searchHost.startsWith("www.")) {
        searchHost = searchHost.slice(4);
      }
      if (searchHost.startsWith(host)) {
        let engine = structuredClone(config.base);
        engine.identifier = config.identifier;
        return engine;
      }
    }
    return null;
  }

  async findContextualSearchEngineById(id) {
    for (let config of this.#configuration) {
      if (config.recordType !== "engine") {
        continue;
      }
      if (config.identifier == id) {
        let engine = structuredClone(config.base);
        engine.identifier = config.identifier;
        return engine;
      }
    }
    return null;
  }

  async fetchEngineConfiguration({
    locale,
    region,
    channel = "default",
    distroID,
    experiment,
    appName = Services.appinfo.name ?? "",
    version = Services.appinfo.version ?? "",
  }) {
    if (!this.#configuration) {
      await this.getEngineConfiguration();
    }

    lazy.logConsole.debug(
      `fetchEngineConfiguration ${locale}:${region}:${channel}:${distroID}:${experiment}:${appName}:${version}`
    );

    let refinedSearchConfig = this.#selector.filterEngineConfiguration(
      new lazy.SearchUserEnvironment({
        locale,
        region,
        updateChannel: this.#convertUpdateChannel(channel),
        distributionId: distroID ?? "",
        experiment: experiment ?? "",
        appName: this.#convertApplicationName(appName),
        version,
        deviceType: lazy.SearchDeviceType.NONE,
      })
    );

    refinedSearchConfig.engines = refinedSearchConfig.engines.filter(
      e => !e.optional
    );

    if (
      !refinedSearchConfig.appDefaultEngineId ||
      !refinedSearchConfig.engines.find(
        e => e.identifier == refinedSearchConfig.appDefaultEngineId
      )
    ) {
      if (refinedSearchConfig.engines.length) {
        lazy.logConsole.error(
          "Could not find a matching default engine, using the first one in the list"
        );
        refinedSearchConfig.appDefaultEngineId =
          refinedSearchConfig.engines[0].identifier;
      } else {
        throw new Error(
          "Could not find any engines in the filtered configuration"
        );
      }
    }
    if (lazy.SearchUtils.loggingEnabled) {
      lazy.logConsole.debug(
        "fetchEngineConfiguration: " +
          refinedSearchConfig.engines.map(e => e.identifier)
      );
    }

    return refinedSearchConfig;
  }

  #remoteConfig;

  #remoteConfigOverrides;

  #configuration;

  #boundOnConfigurationUpdated;

  #boundOnConfigurationOverridesUpdated;

  #listenerAdded = false;

  #changeListener;

  #getConfigurationPromise;

  async #getConfiguration(firstTime = true) {
    let result = [];
    let failed = false;
    try {
      result = await this.#remoteConfig.get({
        order: "id",
      });
    } catch (ex) {
      lazy.logConsole.error(ex);
      failed = true;
    }
    if (!result.length) {
      lazy.logConsole.error("Received empty search configuration!");
      failed = true;
    }
    if (firstTime && failed) {
      await this.#remoteConfig.db.clear();
      return this.#getConfiguration(false);
    }
    return result;
  }

  _onConfigurationUpdated({ data: { current } }) {
    this.#configuration = current;

    this.#selector.setSearchConfig(
      JSON.stringify({ data: this.#configuration })
    );

    lazy.logConsole.debug("Search configuration updated remotely");
    if (this.#changeListener) {
      this.#changeListener();
    }
  }

  _onConfigurationOverridesUpdated({ data: { current } }) {
    this.#selector.setConfigOverrides(JSON.stringify({ data: current }));

    lazy.logConsole.debug("Search configuration overrides updated remotely");
    if (this.#changeListener) {
      this.#changeListener();
    }
  }

  async #getConfigurationOverrides() {
    let result = [];
    try {
      result = await this.#remoteConfigOverrides.get();
    } catch (ex) {
    }
    return result;
  }

  #cachedSelector = null;

  get #selector() {
    if (!this.#cachedSelector) {
      this.#cachedSelector = lazy.SearchEngineSelector.init();
    }
    return this.#cachedSelector;
  }

  #convertUpdateChannel(channel) {
    let uppercaseChannel = channel.toUpperCase();

    if (uppercaseChannel in lazy.SearchUpdateChannel) {
      return lazy.SearchUpdateChannel[uppercaseChannel];
    }

    return lazy.SearchUpdateChannel.DEFAULT;
  }

  #convertApplicationName(appName) {
    let uppercaseAppName = appName.toUpperCase().replace("-", "_");

    if (uppercaseAppName in lazy.SearchApplicationName) {
      return lazy.SearchApplicationName[uppercaseAppName];
    }
    return lazy.SearchApplicationName.FIREFOX;
  }
}
