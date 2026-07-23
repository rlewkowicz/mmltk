/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  RemoteSettings: "resource://services-settings/remote-settings.sys.mjs",
});

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const COLLECTION_NAME = "query-stripping";
const SHARED_DATA_KEY = "URLQueryStripping";
const PREF_STRIP_LIST_NAME = "privacy.query_stripping.strip_list";
const PREF_ALLOW_LIST_NAME = "privacy.query_stripping.allow_list";
const PREF_TESTING_ENABLED = "privacy.query_stripping.testing";
const PREF_STRIP_IS_TEST =
  "privacy.query_stripping.strip_on_share.enableTestMode";

ChromeUtils.defineLazyGetter(lazy, "logger", () => {
  return console.createInstance({
    prefix: "URLQueryStrippingListService",
    maxLogLevelPref: "privacy.query_stripping.listService.logLevel",
  });
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "testStripOnShare",
  PREF_STRIP_IS_TEST
);

async function fetchList(fileName) {
  let response = await fetch(
    "chrome://global/content/antitracking/" + fileName
  );
  if (!response.ok) {
    lazy.logger.error(
      "Error fetching strip-on-share strip list" + response.status
    );
    throw new Error(
      "Error fetching strip-on-share strip list" + response.status
    );
  }
  return response.json();
}

ChromeUtils.defineLazyGetter(lazy, "StripOnShareList", async () => {
  let [stripOnShareList, stripOnShareLGPLParams] = await Promise.all([
    fetchList("StripOnShare.json"),
    fetchList("StripOnShareLGPL.json"),
  ]);

  if (!stripOnShareList || !stripOnShareLGPLParams) {
    lazy.logger.error("Error strip-on-share strip list were not loaded");
    throw new Error("Error fetching strip-on-share strip list were not loaded");
  }

  // Combines the mozilla licensed strip on share param
  // list and the LGPL licensed strip on share param list
  return combineAndParseLists(stripOnShareList, [stripOnShareLGPLParams]);
});

function combineAndParseLists(mainList, arrOfLists) {
  arrOfLists.forEach(additionalList => {
    for (let key in additionalList) {
      if (Object.hasOwn(mainList, key)) {
        mainList[key].queryParams.push(...additionalList[key].queryParams);

        mainList[key].origins ??= [];
        additionalList[key].origins ??= [];

        mainList[key].origins.push(...additionalList[key].origins);
      } else {
        mainList[key] = additionalList[key];
      }
    }
  });

  for (let key in mainList) {
    mainList[key].queryParams = (mainList[key].queryParams ?? []).map(param =>
      param.toLowerCase()
    );

    mainList[key].origins = (mainList[key].origins ?? []).map(origin =>
      origin.toLowerCase()
    );

    mainList[key].origins = [...new Set(mainList[key].origins)];

    mainList[key].queryParams = [...new Set(mainList[key].queryParams)];
  }

  return mainList;
}

export class URLQueryStrippingListService {
  classId = Components.ID("{afff16f0-3fd2-4153-9ccd-c6d9abd879e4}");
  QueryInterface = ChromeUtils.generateQI(["nsIURLQueryStrippingListService"]);

  #isInitialized = false;
  #pendingInit = null;
  #initResolver;
  #stripOnShareTestList = null;

  #rs;
  #onSyncCallback;

  constructor() {
    lazy.logger.debug("constructor");
    this.observers = new Set();
    this.stripOnShareObservers = new Set();
    this.stripOnShareParams = null;
    this.prefStripList = new Set();
    this.prefAllowList = new Set();
    this.remoteStripList = new Set();
    this.remoteAllowList = new Set();
    this.isParentProcess =
      Services.appinfo.processType === Services.appinfo.PROCESS_TYPE_DEFAULT;
  }

  #onSync(event) {
    lazy.logger.debug("onSync", event);
    let {
      data: { current },
    } = event;
    this._onRemoteSettingsUpdate(current);
  }

  async testSetList(testList) {
    this.#stripOnShareTestList = combineAndParseLists(testList, []);
    await this._notifyStripOnShareObservers();
  }

  testHasStripOnShareObservers() {
    return !!this.stripOnShareObservers.size;
  }

  testHasQPSObservers() {
    return !!this.observers.size;
  }

  async #init() {
    if (this.#pendingInit) {
      lazy.logger.debug("#init: Waiting for pending init");
      await this.#pendingInit;
      return;
    }

    if (this.#isInitialized) {
      lazy.logger.debug("#init: Skip, already initialized");
      return;
    }
    this.#pendingInit = new Promise(initResolve => {
      this.#initResolver = initResolve;
    });
    this.#isInitialized = true;

    lazy.logger.debug("#init: Run");

    if (this.isParentProcess) {
      this.#rs = lazy.RemoteSettings(COLLECTION_NAME);

      if (!this.#onSyncCallback) {
        this.#onSyncCallback = this.#onSync.bind(this);
        this.#rs.on("sync", this.#onSyncCallback);
      }

      let entries;
      try {
        entries = await this.#rs.get();
      } catch (e) {}
      this._onRemoteSettingsUpdate(entries || []);
    } else {
      Services.cpmm.sharedData.addEventListener("change", this);

      let data = this._getListFromSharedData();

      this._onRemoteSettingsUpdate(data);
    }

    await this._onPrefUpdate(
      PREF_STRIP_LIST_NAME,
      Services.prefs.getStringPref(PREF_STRIP_LIST_NAME, "")
    );
    await this._onPrefUpdate(
      PREF_ALLOW_LIST_NAME,
      Services.prefs.getStringPref(PREF_ALLOW_LIST_NAME, "")
    );

    Services.prefs.addObserver(PREF_STRIP_LIST_NAME, this);
    Services.prefs.addObserver(PREF_ALLOW_LIST_NAME, this);

    Services.obs.addObserver(this, "xpcom-shutdown");

    this.#initResolver();
    this.#pendingInit = null;
  }

  async #shutdown() {
    if (this.#pendingInit) {
      await this.#pendingInit;
    }

    if (!this.#isInitialized) {
      return;
    }
    this.#isInitialized = false;

    lazy.logger.debug("#shutdown");

    if (this.#onSyncCallback) {
      this.#rs.off("sync", this.#onSyncCallback);
      this.#onSyncCallback = null;
    }

    Services.obs.removeObserver(this, "xpcom-shutdown");
    Services.prefs.removeObserver(PREF_STRIP_LIST_NAME, this);
    Services.prefs.removeObserver(PREF_ALLOW_LIST_NAME, this);
  }

  get hasObservers() {
    return !this.observers.size && !this.stripOnShareObservers.size;
  }
  _onRemoteSettingsUpdate(entries) {
    this.remoteStripList.clear();
    this.remoteAllowList.clear();

    for (let entry of entries) {
      for (let item of entry.stripList) {
        this.remoteStripList.add(item);
      }

      for (let item of entry.allowList) {
        this.remoteAllowList.add(item);
      }
    }

    if (this.isParentProcess) {
      Services.ppmm.sharedData.set(SHARED_DATA_KEY, {
        stripList: this.remoteStripList,
        allowList: this.remoteAllowList,
      });

      if (Services.prefs.getBoolPref(PREF_TESTING_ENABLED, false)) {
        Services.ppmm.sharedData.flush();
      }
    }

    this._notifyObservers();
  }

  async _onPrefUpdate(pref, value) {
    switch (pref) {
      case PREF_STRIP_LIST_NAME:
        this.prefStripList = new Set(value ? value.split(" ") : []);
        break;

      case PREF_ALLOW_LIST_NAME:
        this.prefAllowList = new Set(value ? value.split(",") : []);
        break;

      default:
        console.error(`Unexpected pref name ${pref}`);
        return;
    }

    this._notifyObservers();
    await this._notifyStripOnShareObservers();
  }

  _getListFromSharedData() {
    let data = Services.cpmm.sharedData.get(SHARED_DATA_KEY);

    return data ? [data] : [];
  }

  _notifyObservers(observer) {
    let stripEntries = new Set([
      ...this.prefStripList,
      ...this.remoteStripList,
    ]);
    let allowEntries = new Set([
      ...this.prefAllowList,
      ...this.remoteAllowList,
    ]);
    let stripEntriesAsString = Array.from(stripEntries).join(" ").toLowerCase();
    let allowEntriesAsString = Array.from(allowEntries).join(",").toLowerCase();

    let observers = observer ? [observer] : this.observers;

    if (observer || this.observers.size) {
      lazy.logger.debug("_notifyObservers", {
        observerCount: observers.length,
        runObserverAfterRegister: observer != null,
        stripEntriesAsString,
        allowEntriesAsString,
      });
    }

    for (let obs of observers) {
      obs.onQueryStrippingListUpdate(
        stripEntriesAsString,
        allowEntriesAsString
      );
    }
  }

  async _notifyStripOnShareObservers(observer) {
    this.stripOnShareParams = await lazy.StripOnShareList;

    if (lazy.testStripOnShare) {
      this.stripOnShareParams = this.#stripOnShareTestList;
    }

    if (!this.stripOnShareParams) {
      lazy.logger.error("StripOnShare list is undefined");
      return;
    }

    let qpsParams = [...this.prefStripList, ...this.remoteStripList].map(
      param => param.toLowerCase()
    );

    this.stripOnShareParams.global.queryParams.push(...qpsParams);
    this.stripOnShareParams.global.queryParams = [
      ...new Set(this.stripOnShareParams.global.queryParams),
    ];

    let rules = Object.values(this.stripOnShareParams);
    let stringifiedRules = [];
    rules.forEach(rule => {
      stringifiedRules.push(JSON.stringify(rule));
    });

    let observers = observer ? new Set([observer]) : this.stripOnShareObservers;

    if (observers.size) {
      lazy.logger.debug("_notifyStripOnShareObservers", {
        observerCount: observers.size,
        runObserverAfterRegister: observer != null,
        stringifiedRules,
      });
    }
    for (let obs of observers) {
      obs.onStripOnShareUpdate(stringifiedRules);
    }
  }

  async registerAndRunObserver(observer) {
    lazy.logger.debug("registerAndRunObserver", {
      isInitialized: this.#isInitialized,
      pendingInit: this.#pendingInit,
    });

    await this.#init();
    this.observers.add(observer);
    this._notifyObservers(observer);
  }

  async registerAndRunObserverStripOnShare(observer) {
    lazy.logger.debug("registerAndRunObserverStripOnShare", {
      isInitialized: this.#isInitialized,
      pendingInit: this.#pendingInit,
    });

    await this.#init();
    this.stripOnShareObservers.add(observer);
    await this._notifyStripOnShareObservers(observer);
  }

  async unregisterObserver(observer) {
    this.observers.delete(observer);

    if (this.hasObservers) {
      lazy.logger.debug("Last observer unregistered, shutting down...");
      await this.#shutdown();
    }
  }

  async unregisterStripOnShareObserver(observer) {
    this.stripOnShareObservers.delete(observer);

    if (this.hasObservers) {
      lazy.logger.debug("Last observer unregistered, shutting down...");
      await this.#shutdown();
    }
  }

  async clearLists() {
    if (!this.isParentProcess) {
      return;
    }

    await this.#init();

    this._onRemoteSettingsUpdate([]);

    Services.prefs.clearUserPref(PREF_STRIP_LIST_NAME);
    Services.prefs.clearUserPref(PREF_ALLOW_LIST_NAME);
  }

  observe(subject, topic, data) {
    lazy.logger.debug("observe", { topic, data });
    switch (topic) {
      case "xpcom-shutdown":
        this.#shutdown();
        break;
      case "nsPref:changed": {
        let prefValue = Services.prefs.getStringPref(data, "");
        this._onPrefUpdate(data, prefValue);
        break;
      }
      default:
        console.error(`Unexpected event ${topic}`);
    }
  }

  handleEvent(event) {
    if (event.type != "change") {
      return;
    }

    if (!event.changedKeys.includes(SHARED_DATA_KEY)) {
      return;
    }

    let data = this._getListFromSharedData();
    this._onRemoteSettingsUpdate(data);
    this._notifyObservers();
  }

  async testWaitForInit() {
    if (this.#pendingInit) {
      await this.#pendingInit;
    }

    return this.#isInitialized;
  }
}
