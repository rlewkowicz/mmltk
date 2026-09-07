/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "logConsole", () =>
  console.createInstance({
    prefix: "IPProtectionServerlist",
    maxLogLevel: Services.prefs.getBoolPref("browser.ipProtection.log", false)
      ? "Debug"
      : "Warn",
  })
);

ChromeUtils.defineESModuleGetters(lazy, {
  IPPStartupCache:
    "moz-src:///toolkit/components/ipprotection/IPPStartupCache.sys.mjs",
  IPProtectionService:
    "moz-src:///toolkit/components/ipprotection/IPProtectionService.sys.mjs",
  IPProtectionStates:
    "moz-src:///toolkit/components/ipprotection/IPProtectionService.sys.mjs",
  RemoteSettings: "resource://services-settings/remote-settings.sys.mjs",
});

export const RECOMMENDED_COUNTRY_CODE = "REC";

const LIST_CHANGED_EVENT = "IPProtectionServerlist:ListChanged";

export class IProtocol {
  name = "";
  static construct(data) {
    switch (data.name) {
      case "masque":
        return new MasqueProtocol(data);
      case "connect":
        return new ConnectProtocol(data);
      default:
        throw new Error("Unknown protocol: " + data.name);
    }
  }
}

export class MasqueProtocol extends IProtocol {
  name = "masque";
  host = "";
  port = 0;
  templateString = "";
  constructor(data) {
    super();
    this.host = data.host || "";
    this.port = data.port || 0;
    this.templateString = data.templateString || "";
  }
}

export class ConnectProtocol extends IProtocol {
  name = "connect";
  host = "";
  port = 0;
  scheme = "https";
  constructor(data) {
    super();
    this.host = data.host || "";
    this.port = data.port || 0;
    this.scheme = data.scheme || "https";
  }
}

export class Server {
  port = 443;
  hostname = "";
  quarantined = false;

  protocols = [];

  constructor(data) {
    this.port = data.port || 443;
    this.hostname = data.hostname || "";
    this.quarantined = !!data.quarantined;
    this.protocols = (data.protocols || []).map(p => IProtocol.construct(p));

    if (this.protocols.length === 0) {
      this.protocols = [
        new ConnectProtocol({
          name: "connect",
          host: this.hostname,
          port: this.port,
        }),
      ];
    }
  }
}

class City {
  name = "";
  code = "";
  servers = [];

  constructor(data) {
    this.name = data.name || "";
    this.code = data.code || "";
    this.servers = (data.servers || []).map(s => new Server(s));
  }
}

class Country {
  name;
  code;

  cities;

  locked = false;

  constructor(data) {
    this.name = data.name || "";
    this.code = data.code || "";
    this.cities = (data.cities || []).map(c => new City(c));
    this.locked = !!data.locked;
  }
}

export class IPProtectionServerlistBase extends EventTarget {
  __list = null;

  init() {}

  async initOnStartupCompleted() {}

  uninit() {}

  maybeFetchList(_forceUpdate = false) {
    throw new Error("Not implemented");
  }

  get countries() {
    return this.__list
      .filter(country => country.code !== RECOMMENDED_COUNTRY_CODE)
      .map(country => ({
        available: country.cities.some(city =>
          city.servers.some(server => !server.quarantined)
        ),
        code: country.code,
        locked: country.locked,
      }));
  }

  getLocation(countryCode = RECOMMENDED_COUNTRY_CODE) {
    const country = this.__list.find(c => c.code === countryCode);
    if (!country) {
      return null;
    }
    const city = country.cities.find(c => c.servers.length);
    if (!city) {
      return null;
    }
    return { country, city };
  }

  getRecommendedLocation() {
    return this.getLocation(RECOMMENDED_COUNTRY_CODE) ?? this.getLocation("US");
  }

  selectServer(city) {
    if (!city) {
      return null;
    }

    const servers = city.servers.filter(server => !server.quarantined);
    if (servers.length === 1) {
      return servers[0];
    }

    if (servers.length > 1) {
      return servers[Math.floor(Math.random() * servers.length)];
    }

    return null;
  }

  get hasList() {
    return this.__list.length !== 0;
  }

  static dataToList(list) {
    if (!Array.isArray(list)) {
      return [];
    }
    return list.map(c => new Country(c));
  }
}

export class RemoteSettingsServerlist extends IPProtectionServerlistBase {
  #bucket = null;
  #runningPromise = null;

  constructor() {
    super();
    this.handleEvent = this.#handleEvent.bind(this);
    this.__list = IPProtectionServerlistBase.dataToList(
      lazy.IPPStartupCache.locationList
    );
  }
  init() {
    lazy.IPProtectionService.addEventListener(
      "IPProtectionService:StateChanged",
      this.handleEvent
    );
  }

  async initOnStartupCompleted() {
    this.bucket.on("sync", async () => {
      await this.maybeFetchList(true);
    });
  }

  uninit() {
    lazy.IPProtectionService.removeEventListener(
      "IPProtectionService:StateChanged",
      this.handleEvent
    );
  }

  #handleEvent(_event) {
    if (lazy.IPProtectionService.state === lazy.IPProtectionStates.READY) {
      this.maybeFetchList();
    }
  }

  maybeFetchList(forceUpdate = false) {
    if (this.__list.length !== 0 && !forceUpdate) {
      return Promise.resolve();
    }

    if (this.#runningPromise) {
      return this.#runningPromise;
    }

    const fetchList = async () => {
      this.__list = IPProtectionServerlistBase.dataToList(
        await this.bucket.get()
      );

      lazy.IPPStartupCache.storeLocationList(this.__list);
      this.dispatchEvent(new Event(LIST_CHANGED_EVENT));
    };

    this.#runningPromise = fetchList().finally(
      () => (this.#runningPromise = null)
    );

    return this.#runningPromise;
  }

  get bucket() {
    if (!this.#bucket) {
      this.#bucket = lazy.RemoteSettings("vpn-serverlist");
    }
    return this.#bucket;
  }
}
export class PrefServerList extends IPProtectionServerlistBase {
  #observer = null;
  #previousList = null;

  constructor() {
    super();
    this.#observer = this.onPrefChange.bind(this);
    this.maybeFetchList();
  }

  onPrefChange() {
    this.maybeFetchList();
  }

  async initOnStartupCompleted() {
    Services.prefs.addObserver(PrefServerList.PREF_NAME, this.#observer);
    this.maybeFetchList();
  }

  uninit() {
    Services.prefs.removeObserver(PrefServerList.PREF_NAME, this.#observer);
  }

  maybeFetchList(forceUpdate = false) {
    const newList = Services.prefs.getStringPref(PrefServerList.PREF_NAME, "");

    if (!forceUpdate && newList === this.#previousList) {
      return Promise.resolve();
    }
    this.#previousList = newList;
    this.__list = IPProtectionServerlistBase.dataToList(
      PrefServerList.prefValue
    );
    this.dispatchEvent(new Event(LIST_CHANGED_EVENT));
    return Promise.resolve();
  }

  static get PREF_NAME() {
    return "browser.ipProtection.override.serverlist";
  }
  static get hasPrefValue() {
    return (
      Services.prefs.getPrefType(this.PREF_NAME) ===
        Services.prefs.PREF_STRING &&
      !!Services.prefs.getStringPref(this.PREF_NAME).length
    );
  }
  static get prefValue() {
    try {
      const value = Services.prefs.getStringPref(this.PREF_NAME);
      return JSON.parse(value);
    } catch (e) {
      lazy.logConsole.error(
        `IPProtection: Error parsing serverlist pref value: ${e}`
      );
      return null;
    }
  }
}
export function IPProtectionServerlistFactory() {
  if (AppConstants.MOZ_ENTERPRISE) {
    return new PrefServerList();
  }
  return PrefServerList.hasPrefValue
    ? new PrefServerList()
    : new RemoteSettingsServerlist();
}

const IPProtectionServerlist = IPProtectionServerlistFactory();

export { IPProtectionServerlist };
