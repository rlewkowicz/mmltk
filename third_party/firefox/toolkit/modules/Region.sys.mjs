/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  LocationHelper: "resource://gre/modules/LocationHelper.sys.mjs",
  RemoteSettings: "resource://services-settings/remote-settings.sys.mjs",
  setTimeout: "resource://gre/modules/Timer.sys.mjs",
});


XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "wifiScanningEnabled",
  "browser.region.network.scan",
  true
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "networkTimeout",
  "browser.region.timeout",
  5000
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "retryTimeout",
  "browser.region.retry-timeout",
  60 * 60 * 1000
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "loggingEnabled",
  "browser.region.log",
  false
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "cacheBustEnabled",
  "browser.region.update.enabled",
  false
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "updateDebounce",
  "browser.region.update.debounce",
  60 * 60 * 24
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "lastUpdated",
  "browser.region.update.updated",
  0
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "localGeocodingEnabled",
  "browser.region.local-geocoding",
  false
);

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "timerManager",
  "@mozilla.org/updates/timer-manager;1",
  Ci.nsIUpdateTimerManager
);

const log = console.createInstance({
  prefix: "Region.sys.mjs",
  maxLogLevel: lazy.loggingEnabled ? "All" : "Warn",
});

const REGION_PREF = "browser.search.region";
const COLLECTION_ID = "regions";
const GEOLOCATION_TOPIC = "geolocation-position-events";

const UPDATE_PREFIX = "browser.region.update";

const UPDATE_INTERVAL = 60 * 60 * 24 * 14;

const MAX_RETRIES = 3;

const UPDATE_CHECK_NAME = "region-update-timer";
const UPDATE_CHECK_INTERVAL = 60 * 60 * 24 * 7;

let inChildProcess =
  Services.appinfo.processType != Ci.nsIXULRuntime.PROCESS_TYPE_DEFAULT;

class RegionDetector {
  #home = null;
  #current = null;
  #rsClient;
  #wifiDataPromiseResolver = null;
  _retryCount = 0;
  #initPromise = null;
  REGION_TOPIC = "browser-region-updated";
  TELEMETRY = {
    SUCCESS: 0,
    NO_RESULT: 1,
    TIMEOUT: 2,
    ERROR: 3,
  };

  init() {
    if (inChildProcess) {
      this.#home = Services.prefs.getCharPref(REGION_PREF, null);
      return Promise.resolve();
    }

    if (this.#initPromise) {
      return this.#initPromise;
    }
    if (lazy.cacheBustEnabled) {
      Services.tm.idleDispatchToMainThread(() => {
        lazy.timerManager.registerTimer(
          UPDATE_CHECK_NAME,
          () => this._updateTimer(),
          UPDATE_CHECK_INTERVAL
        );
      });
    }
    let promises = [];
    this.#home = Services.prefs.getCharPref(REGION_PREF, null);
    if (this.#home) {
    } else {
      promises.push(this.#idleDispatch(() => this._fetchRegion()));
    }
    if (lazy.localGeocodingEnabled) {
      promises.push(this.#idleDispatch(() => this.#setupRemoteSettings()));
    }
    return (this.#initPromise = Promise.all(promises));
  }

  get home() {
    return this.#home;
  }

  get current() {
    return this.#current;
  }

  async _fetchRegion() {
    if (this._retryCount >= MAX_RETRIES) {
      return;
    }
    let startTime = Date.now();
    let telemetryResult = this.TELEMETRY.SUCCESS;
    let result = null;

    try {
      result = await this.#getRegion();
    } catch (err) {
      telemetryResult = this.TELEMETRY[err.message] || this.TELEMETRY.ERROR;
      log.error("Failed to fetch region", err);
      if (lazy.retryTimeout) {
        this._retryCount++;
        lazy.setTimeout(() => {
          Services.tm.idleDispatchToMainThread(this._fetchRegion.bind(this));
        }, lazy.retryTimeout);
      }
    }

    let took = Date.now() - startTime;
    if (result) {
      await this.#storeRegion(result);
    }

  }

  async #storeRegion(region) {
    let isTimezoneUS = this._isUSTimezone();
    if (region != "US") {
      this._setCurrentRegion(region);
    } else if (isTimezoneUS) {
      this._setCurrentRegion(region);
    } else {
    }
  }

  _setCurrentRegion(region = "") {
    log.info("Setting current region:", region);
    this.#current = region;

    let now = Math.round(Date.now() / 1000);
    let prefs = Services.prefs;
    prefs.setIntPref(`${UPDATE_PREFIX}.updated`, now);

    let interval = prefs.getIntPref(
      `${UPDATE_PREFIX}.interval`,
      UPDATE_INTERVAL
    );
    let seenRegion = prefs.getCharPref(`${UPDATE_PREFIX}.region`, null);
    let firstSeen = prefs.getIntPref(`${UPDATE_PREFIX}.first-seen`, 0);

    if (!this.#home) {
      this._setHomeRegion(region);
    } else if (region != this.#home && region != seenRegion) {
      prefs.setCharPref(`${UPDATE_PREFIX}.region`, region);
      prefs.setIntPref(`${UPDATE_PREFIX}.first-seen`, now);
    } else if (region != this.#home && region == seenRegion) {
      if (now >= firstSeen + interval) {
        this._setHomeRegion(region);
      }
    } else {
      prefs.clearUserPref(`${UPDATE_PREFIX}.region`);
      prefs.clearUserPref(`${UPDATE_PREFIX}.first-seen`);
    }
  }

  #createSupportsString(data) {
    let string = Cc["@mozilla.org/supports-string;1"].createInstance(
      Ci.nsISupportsString
    );
    string.data = data;
    return string;
  }

  _setHomeRegion(region, notify = true) {
    if (region == this.#home) {
      return;
    }
    log.info("Updating home region:", region);
    this.#home = region;
    Services.prefs.setCharPref("browser.search.region", region);
    if (notify) {
      Services.obs.notifyObservers(
        this.#createSupportsString(region),
        this.REGION_TOPIC
      );
    }
  }

  async #getRegion() {
    log.info("#getRegion called");
    let fetchOpts = {
      headers: { "Content-Type": "application/json" },
      credentials: "omit",
    };
    if (lazy.wifiScanningEnabled) {
      let wifiData = await this.#fetchWifiData();
      if (wifiData) {
        let postData = JSON.stringify({ wifiAccessPoints: wifiData });
        log.info("Sending wifi details: ", wifiData);
        fetchOpts.method = "POST";
        fetchOpts.body = postData;
      }
    }
    let url = Services.urlFormatter.formatURLPref("browser.region.network.url");
    log.info("#getRegion url is: ", url);

    if (!url) {
      return null;
    }

    try {
      let req = await this.#fetchTimeout(url, fetchOpts, lazy.networkTimeout);
      // @ts-ignore
      let res =  (await req.json());
      log.info("_#getRegion returning ", res.country_code);
      return res.country_code;
    } catch (err) {
      log.error("Error fetching region", err);
      let errCode = err.message in this.TELEMETRY ? err.message : "NO_RESULT";
      throw new Error(errCode);
    }
  }

  async #setupRemoteSettings() {
    log.info("#setupRemoteSettings");
    this.#rsClient = lazy.RemoteSettings(COLLECTION_ID);
    this.#rsClient.on("sync", this._onRegionFilesSync.bind(this));
    await this.#ensureRegionFilesDownloaded();
    Services.obs.addObserver(this, GEOLOCATION_TOPIC);
  }

  async _onRegionFilesSync({ data: { deleted } }) {
    log.info("_onRegionFilesSync");
    const toDelete = deleted.filter(d => d.attachment);
    await Promise.all(
      toDelete.map(entry => this.#rsClient.attachments.deleteDownloaded(entry))
    );
    await this.#ensureRegionFilesDownloaded();
  }

  async #ensureRegionFilesDownloaded() {
    log.info("#ensureRegionFilesDownloaded");
    let records = (await this.#rsClient.get()).filter(d => d.attachment);
    log.info("#ensureRegionFilesDownloaded", records);
    if (!records.length) {
      log.info("#ensureRegionFilesDownloaded: Nothing to download");
      return;
    }
    await Promise.all(records.map(r => this.#rsClient.attachments.download(r)));
    log.info("#ensureRegionFilesDownloaded complete");
    this._regionFilesReady = true;
  }

  async #fetchAttachment(id) {
    let record = (await this.#rsClient.get({ filters: { id } })).pop();
    let { buffer } = await this.#rsClient.attachments.download(record);
    let text = new TextDecoder("utf-8").decode(buffer);
    return JSON.parse(text);
  }

  async _getPlainMap() {
    return this.#fetchAttachment("world");
  }

  async _getBufferedMap() {
    return this.#fetchAttachment("world-buffered");
  }

  async _getLocation() {
    log.info("_getLocation called");
    let fetchOpts = { headers: { "Content-Type": "application/json" } };
    let url = Services.urlFormatter.formatURLPref("geo.provider.network.url");
    let req = await this.#fetchTimeout(url, fetchOpts, lazy.networkTimeout);
    let result = await req.json();
    log.info("_getLocation returning", result);
    return result;
  }

  async _getRegionLocally() {
    let { location } = await this._getLocation();
    return this.#geoCode(location);
  }

  async #geoCode(location) {
    let plainMap = await this._getPlainMap();
    let polygons = this.#getPolygonsContainingPoint(location, plainMap);
    if (polygons.length == 1) {
      log.info("Found in single exact region");
      return polygons[0].properties.alpha2;
    }
    if (polygons.length) {
      log.info("Found in ", polygons.length, "overlapping exact regions");
      return this.#findFurthest(location, polygons);
    }

    let bufferedMap = await this._getBufferedMap();
    polygons = this.#getPolygonsContainingPoint(location, bufferedMap);

    if (polygons.length === 1) {
      log.info("Found in single buffered region");
      return polygons[0].properties.alpha2;
    }

    if (polygons.length) {
      log.info("Found in ", polygons.length, "overlapping buffered regions");
      let regions = polygons.map(polygon => polygon.properties.alpha2);
      let unBufferedRegions = plainMap.features.filter(feature =>
        regions.includes(feature.properties.alpha2)
      );
      return this.#findClosest(location, unBufferedRegions);
    }
    return null;
  }

  #getPolygonsContainingPoint(point, map) {
    let polygons = [];
    for (const feature of map.features) {
      let coords = feature.geometry.coordinates;
      if (feature.geometry.type === "Polygon") {
        if (this.#polygonInPoint(point, coords[0])) {
          polygons.push(feature);
        }
      } else if (feature.geometry.type === "MultiPolygon") {
        for (const innerCoords of coords) {
          if (this.#polygonInPoint(point, innerCoords[0])) {
            polygons.push(feature);
          }
        }
      }
    }
    return polygons;
  }

  #findFurthest(location, regions) {
    let max = { distance: 0, region: null };
    this.#traverse(regions, ({ lat, lng, region }) => {
      let distance = this.#distanceBetween(location, { lng, lat });
      if (distance > max.distance) {
        max = { distance, region };
      }
    });
    return max.region;
  }

  #findClosest(location, regions) {
    let min = { distance: Infinity, region: null };
    this.#traverse(regions, ({ lat, lng, region }) => {
      let distance = this.#distanceBetween(location, { lng, lat });
      if (distance < min.distance) {
        min = { distance, region };
      }
    });
    return min.region;
  }

  #traverse(regions, fun) {
    for (const region of regions) {
      if (region.geometry.type === "Polygon") {
        for (const [lng, lat] of region.geometry.coordinates[0]) {
          fun({ lat, lng, region: region.properties.alpha2 });
        }
      } else if (region.geometry.type === "MultiPolygon") {
        for (const innerCoords of region.geometry.coordinates) {
          for (const [lng, lat] of innerCoords[0]) {
            fun({ lat, lng, region: region.properties.alpha2 });
          }
        }
      }
    }
  }

  #polygonInPoint({ lng, lat }, poly) {
    let inside = false;
    for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
      let xi = poly[i][0];
      let yi = poly[i][1];
      let xj = poly[j][0];
      let yj = poly[j][1];
      let intersect =
        yi > lat != yj > lat && lng < ((xj - xi) * (lat - yi)) / (yj - yi) + xi;
      if (intersect) {
        inside = !inside;
      }
    }
    return inside;
  }

  #distanceBetween(p1, p2) {
    return Math.hypot(p2.lng - p1.lng, p2.lat - p1.lat);
  }

  async #fetchTimeout(url, opts, timeout) {
    let controller = new AbortController();
    opts.signal = controller.signal;
    return  (
      Promise.race([fetch(url, opts), this.#timeout(timeout, controller)])
    );
  }

  async #timeout(timeout, controller) {
    await new Promise(resolve => lazy.setTimeout(resolve, timeout));
    if (controller) {
      lazy.setTimeout(() => controller.abort(), 0);
    }
    throw new Error("TIMEOUT");
  }

  async #fetchWifiData() {
    log.info("fetchWifiData called");
    this.wifiService = Cc["@mozilla.org/wifi/monitor;1"].getService(
      Ci.nsIWifiMonitor
    );
    this.wifiService.startWatching(this, false);

    return new Promise(resolve => {
      this.#wifiDataPromiseResolver = resolve;
    });
  }

  #needsUpdateCheck() {
    let sinceUpdate = Math.round(Date.now() / 1000) - lazy.lastUpdated;
    let needsUpdate = sinceUpdate >= lazy.updateDebounce;
    if (!needsUpdate) {
      log.info(`Ignoring update check, last seen ${sinceUpdate} seconds ago`);
    }
    return needsUpdate;
  }

  #idleDispatch(fun) {
    return new Promise(resolve => {
      Services.tm.idleDispatchToMainThread(fun().then(resolve));
    });
  }

  async _updateTimer() {
    if (this.#needsUpdateCheck()) {
      await this._fetchRegion();
    }
  }

  async #seenLocation(location) {
    log.info(`Got location update: ${location.lat}:${location.lng}`);
    if (this.#needsUpdateCheck()) {
      let region = await this.#geoCode(location);
      if (region) {
        this._setCurrentRegion(region);
      }
    }
  }

  onChange(accessPoints) {
    log.info("onChange called");
    if (!accessPoints || !this.#wifiDataPromiseResolver) {
      return;
    }

    if (this.wifiService) {
      this.wifiService.stopWatching(this);
      this.wifiService = null;
    }

    if (this.#wifiDataPromiseResolver) {
      let data = lazy.LocationHelper.formatWifiAccessPoints(accessPoints);
      this.#wifiDataPromiseResolver(data);
      this.#wifiDataPromiseResolver = null;
    }
  }

  onError() {}

  _isUSTimezone() {




    let UTCOffset = new Date().getTimezoneOffset();
    return UTCOffset >= 150 && UTCOffset <= 600;
  }

  observe(aSubject, aTopic) {
    log.info(`Observed ${aTopic}`);
    switch (aTopic) {
      case GEOLOCATION_TOPIC: {
        let coords = aSubject.coords || aSubject.wrappedJSObject.coords;
        this.#seenLocation({
          lat: coords.latitude,
          lng: coords.longitude,
        });
        break;
      }
    }
  }

  newInstance() {
    return new RegionDetector();
  }
}

export let Region = new RegionDetector();
Region.init();
