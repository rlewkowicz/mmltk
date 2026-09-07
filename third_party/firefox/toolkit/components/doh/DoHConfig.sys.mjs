/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { RemoteSettings } from "resource://services-settings/remote-settings.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  Preferences: "resource://gre/modules/Preferences.sys.mjs",
  Region: "resource://gre/modules/Region.sys.mjs",
});

const kGlobalPrefBranch = "doh-rollout";
function regionPrefBranch() {
  let homeRegion = Services.prefs.getStringPref(
    `${kGlobalPrefBranch}.home-region`,
    undefined
  );
  if (!homeRegion) {
    return undefined;
  }
  return `${kGlobalPrefBranch}.${homeRegion.toLowerCase()}`;
}

function currentRegion() {
  if (lazy.Region.current) {
    return lazy.Region.current;
  }
  return lazy.Region.home;
}

const kConfigPrefs = {
  kEnabledPref: "enabled",
  kProvidersPref: "provider-list",
  kTRRSelectionEnabledPref: "trr-selection.enabled",
  kTRRSelectionProvidersPref: "trr-selection.provider-list",
  kTRRSelectionCommitResultPref: "trr-selection.commit-result",
  kProviderSteeringEnabledPref: "provider-steering.enabled",
  kProviderSteeringListPref: "provider-steering.provider-list",
};

const kPrefChangedTopic = "nsPref:changed";

const gProvidersCollection = RemoteSettings("doh-providers");
const gConfigCollection = RemoteSettings("doh-config");

function getPrefValueRegionFirst(prefName) {
  let regionalPrefName = `${regionPrefBranch()}.${prefName}`;
  let regionalPrefValue = lazy.Preferences.get(regionalPrefName);
  if (regionalPrefValue !== undefined) {
    return regionalPrefValue;
  }
  return lazy.Preferences.get(`${kGlobalPrefBranch}.${prefName}`);
}

function getProviderListFromPref(prefName) {
  let prefVal = getPrefValueRegionFirst(prefName);
  if (prefVal) {
    try {
      return JSON.parse(prefVal);
    } catch (e) {
      console.error(`DoH provider list not a valid JSON array: ${prefName}`);
    }
  }
  return undefined;
}

function makeBaseConfigObject() {
  function makeConfigProperty({
    obj,
    propName,
    defaultVal,
    prefName,
    isProviderList,
  }) {
    let prefFn = isProviderList
      ? getProviderListFromPref
      : getPrefValueRegionFirst;

    let overridePropName = "_" + propName;

    Object.defineProperty(obj, propName, {
      get() {
        let prefVal = prefFn(prefName);
        if (prefVal !== undefined) {
          return prefVal;
        }
        if (this[overridePropName] !== undefined) {
          return this[overridePropName];
        }
        return defaultVal;
      },
      set(val) {
        this[overridePropName] = val;
      },
    });
  }
  let newConfig = {
    get fallbackProviderURI() {
      return this.providerList[0]?.uri;
    },
    trrSelection: {},
    providerSteering: {},
  };
  makeConfigProperty({
    obj: newConfig,
    propName: "enabled",
    defaultVal: false,
    prefName: kConfigPrefs.kEnabledPref,
    isProviderList: false,
  });
  makeConfigProperty({
    obj: newConfig,
    propName: "providerList",
    defaultVal: [],
    prefName: kConfigPrefs.kProvidersPref,
    isProviderList: true,
  });
  makeConfigProperty({
    obj: newConfig.trrSelection,
    propName: "enabled",
    defaultVal: false,
    prefName: kConfigPrefs.kTRRSelectionEnabledPref,
    isProviderList: false,
  });
  makeConfigProperty({
    obj: newConfig.trrSelection,
    propName: "commitResult",
    defaultVal: false,
    prefName: kConfigPrefs.kTRRSelectionCommitResultPref,
    isProviderList: false,
  });
  makeConfigProperty({
    obj: newConfig.trrSelection,
    propName: "providerList",
    defaultVal: [],
    prefName: kConfigPrefs.kTRRSelectionProvidersPref,
    isProviderList: true,
  });
  makeConfigProperty({
    obj: newConfig.providerSteering,
    propName: "enabled",
    defaultVal: false,
    prefName: kConfigPrefs.kProviderSteeringEnabledPref,
    isProviderList: false,
  });
  makeConfigProperty({
    obj: newConfig.providerSteering,
    propName: "providerList",
    defaultVal: [],
    prefName: kConfigPrefs.kProviderSteeringListPref,
    isProviderList: true,
  });
  return newConfig;
}

export const DoHConfigController = {
  initComplete: null,
  _resolveInitComplete: null,

  currentConfig: makeBaseConfigObject(),

  async loadRegion() {
    await new Promise(resolve => {
      let homeRegionChanged = Services.prefs.getBoolPref(
        `${kGlobalPrefBranch}.home-region-changed`,
        false
      );
      if (homeRegionChanged) {
        Services.prefs.clearUserPref(
          `${kGlobalPrefBranch}.home-region-changed`
        );
        Services.prefs.clearUserPref(`${kGlobalPrefBranch}.home-region`);
      }

      let homeRegion = Services.prefs.getStringPref(
        `${kGlobalPrefBranch}.home-region`,
        undefined
      );
      if (homeRegion) {
        resolve();
        return;
      }

      let updateRegionAndResolve = () => {
        Services.prefs.setStringPref(
          `${kGlobalPrefBranch}.home-region`,
          currentRegion()
        );
        resolve();
      };

      if (currentRegion()) {
        updateRegionAndResolve();
        return;
      }

      Services.obs.addObserver(function obs() {
        Services.obs.removeObserver(obs, lazy.Region.REGION_TOPIC);
        updateRegionAndResolve();
      }, lazy.Region.REGION_TOPIC);
    });

    await this.updateFromRemoteSettings();
  },

  async init() {
    await this.loadRegion();

    Services.prefs.addObserver(`${kGlobalPrefBranch}.`, this, true);
    Services.obs.addObserver(this, "idle-daily", true);
    Services.obs.addObserver(this, "default-timezone-changed", true);

    gProvidersCollection.on("sync", this.updateFromRemoteSettings);
    gConfigCollection.on("sync", this.updateFromRemoteSettings);

    this._resolveInitComplete();
  },

  async _uninit() {
    await this.initComplete;

    Services.prefs.removeObserver(`${kGlobalPrefBranch}`, this);
    Services.obs.removeObserver(this, "idle-daily");
    Services.obs.removeObserver(this, "default-timezone-changed");

    gProvidersCollection.off("sync", this.updateFromRemoteSettings);
    gConfigCollection.off("sync", this.updateFromRemoteSettings);

    this.initComplete = new Promise(resolve => {
      this._resolveInitComplete = resolve;
    });
  },

  updateRegionIfChanged(trigger) {
    let oldRegion = Services.prefs.getStringPref(
      `${kGlobalPrefBranch}.home-region`,
      undefined
    );
    if (currentRegion() && currentRegion() != oldRegion) {
      let newRegion = currentRegion();
      Services.prefs.setStringPref(
        `${kGlobalPrefBranch}.home-region`,
        newRegion
      );
      this.notifyNewConfig();
    }
  },

  async getRegionAndNotify() {
    await lazy.Region._fetchRegion();
    this.updateRegionIfChanged("timezone-changed");
  },

  observe(subject, topic, data) {
    switch (topic) {
      case kPrefChangedTopic:
        {
          let allowedPrefs = Object.getOwnPropertyNames(kConfigPrefs).map(
            k => kConfigPrefs[k]
          );
          if (
            !allowedPrefs.some(pref =>
              [
                `${regionPrefBranch()}.${pref}`,
                `${kGlobalPrefBranch}.${pref}`,
              ].includes(data)
            )
          ) {
            break;
          }
          this.notifyNewConfig();
        }
        break;
      case "idle-daily":
        this.updateRegionIfChanged("idle-daily");
        break;
      case "default-timezone-changed":
        this.getRegionAndNotify();
        break;
    }
  },

  QueryInterface: ChromeUtils.generateQI([
    "nsIObserver",
    "nsISupportsWeakReference",
  ]),

  async updateFromRemoteSettings() {
    let providers = await gProvidersCollection.get();
    let config = await gConfigCollection.get();

    let providersById = new Map();
    providers.forEach(p => providersById.set(p.id, p));

    let configByRegion = new Map();
    config.forEach(c => {
      c.id = c.id.toLowerCase();
      configByRegion.set(c.id, c);
    });

    let homeRegion = Services.prefs.getStringPref(
      `${kGlobalPrefBranch}.home-region`,
      undefined
    );
    let localConfig =
      configByRegion.get(homeRegion?.toLowerCase()) ||
      configByRegion.get("global");

    let newConfig = makeBaseConfigObject();

    if (!localConfig) {
      DoHConfigController.currentConfig = newConfig;
      DoHConfigController.notifyNewConfig();
      return;
    }

    let isAndroid = Services.appinfo.OS === "Android";
    if (
      (isAndroid && localConfig.androidRolloutEnabled) ||
      (!isAndroid && localConfig.rolloutEnabled)
    ) {
      newConfig.enabled = true;
    }

    let parseProviderList = (list, checkFn) => {
      let parsedList = [];
      list?.split(",")?.forEach(p => {
        p = p.trim();
        if (!p.length) {
          return;
        }
        p = providersById.get(p);
        if (!p || (checkFn && !checkFn(p))) {
          return;
        }
        parsedList.push(p);
      });
      return parsedList;
    };

    let regionalProviders = parseProviderList(localConfig.providers);
    if (regionalProviders?.length) {
      newConfig.providerList = regionalProviders;
    }

    if (localConfig.steeringEnabled) {
      let steeringProviders = parseProviderList(
        localConfig.steeringProviders,
        p => p.canonicalName?.length
      );
      if (steeringProviders?.length) {
        newConfig.providerSteering.providerList = steeringProviders;
        newConfig.providerSteering.enabled = true;
      }
    }

    if (localConfig.autoDefaultEnabled) {
      let defaultProviders = parseProviderList(
        localConfig.autoDefaultProviders
      );
      if (defaultProviders?.length) {
        newConfig.trrSelection.providerList = defaultProviders;
        newConfig.trrSelection.enabled = true;
      }
    }

    DoHConfigController.currentConfig = newConfig;

    function applyHttp3FirstForProviders(providerList = []) {
      for (const provider of providerList) {
        try {
          let uri = Services.io.newURI(provider.uri);
          let host = uri.host;
          Services.dns.setHttp3FirstForServer(host, !!provider.http3First);
        } catch (e) {
          console.error(`Failed to set http3First for ${provider.uri}: ${e}`);
        }
      }
    }

    applyHttp3FirstForProviders(providers);

    DoHConfigController.notifyNewConfig();
  },

  kConfigUpdateTopic: "doh-config-updated",
  notifyNewConfig() {
    Services.obs.notifyObservers(null, this.kConfigUpdateTopic);
  },
};

DoHConfigController.initComplete = new Promise(resolve => {
  DoHConfigController._resolveInitComplete = resolve;
});
DoHConfigController.init();
