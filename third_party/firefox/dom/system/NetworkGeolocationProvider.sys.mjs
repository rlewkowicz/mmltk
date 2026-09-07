/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  LocationHelper: "resource://gre/modules/LocationHelper.sys.mjs",
  clearTimeout: "resource://gre/modules/Timer.sys.mjs",
  setTimeout: "resource://gre/modules/Timer.sys.mjs",
});

const POSITION_UNAVAILABLE = 2;

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "gNetworkGeolocationLogLevel",
  "geo.provider.network.loglevel",
  "Off"
);

ChromeUtils.defineLazyGetter(lazy, "log", () => {
  let consoleOptions = {
    maxLogLevelPref: lazy.gNetworkGeolocationLogLevel,
    prefix: "NetworkGeolocationProvider",
  };
  return console.createInstance(consoleOptions);
});

function CachedRequest(loc, wifiList) {
  this.location = loc;

  let wifis = new Set();
  if (wifiList) {
    for (let i = 0; i < wifiList.length; i++) {
      wifis.add(wifiList[i].macAddress);
    }
  }

  this.hasWifis = () => wifis.size > 0;

  this.isWifiApproxEqual = function (wifiList) {
    if (!this.hasWifis()) {
      return false;
    }

    let common = 0;
    for (let i = 0; i < wifiList.length; i++) {
      if (wifis.has(wifiList[i].macAddress)) {
        common++;
      }
    }
    let kPercentMatch = 0.5;
    return common >= Math.max(wifis.size, wifiList.length) * kPercentMatch;
  };

  this.isGeoip = function () {
    return !this.hasWifis();
  };
}

var gCachedRequest = null;
var gDebugCacheReasoning = ""; 

function isCachedRequestMoreAccurateThanServerRequest(newWifiList) {
  gDebugCacheReasoning = "";
  let isNetworkRequestCacheEnabled = Services.prefs.getBoolPref(
    "geo.provider.network.debug.requestCache.enabled",
    true
  );
  if (!isNetworkRequestCacheEnabled) {
    gCachedRequest = null;
  }

  if (!gCachedRequest || !isNetworkRequestCacheEnabled) {
    gDebugCacheReasoning = "No cached data";
    return false;
  }

  if (!newWifiList) {
    gDebugCacheReasoning = "New req. is GeoIP.";
    return true;
  }

  let hasEqualWifis = false;
  if (newWifiList) {
    hasEqualWifis = gCachedRequest.isWifiApproxEqual(newWifiList);
  }

  gDebugCacheReasoning = `EqualWifis: ${hasEqualWifis}`;

  if (gCachedRequest.hasWifis() && hasEqualWifis) {
    gDebugCacheReasoning += ", Wifi only.";
    return true;
  }

  return false;
}

function NetworkGeoCoordsObject(lat, lon, acc) {
  this.latitude = lat;
  this.longitude = lon;
  this.accuracy = acc;

  this.altitude = NaN;
  this.altitudeAccuracy = NaN;
  this.heading = NaN;
  this.speed = NaN;
}

NetworkGeoCoordsObject.prototype = {
  QueryInterface: ChromeUtils.generateQI(["nsIDOMGeoPositionCoords"]),
};

function NetworkGeoPositionObject(lat, lng, acc) {
  this.coords = new NetworkGeoCoordsObject(lat, lng, acc);
  this.address = null;
  this.timestamp = Date.now();
}

NetworkGeoPositionObject.prototype = {
  QueryInterface: ChromeUtils.generateQI(["nsIDOMGeoPosition"]),
};

export function NetworkGeolocationProvider() {
  XPCOMUtils.defineLazyPreferenceGetter(
    this,
    "_wifiMonitorTimeout",
    "geo.provider.network.timeToWaitBeforeSending",
    5000
  );

  XPCOMUtils.defineLazyPreferenceGetter(
    this,
    "_wifiScanningEnabled",
    "geo.provider.network.scan",
    true
  );

  this.wifiService = null;
  this.timer = null;
  this.started = false;
}

NetworkGeolocationProvider.prototype = {
  classID: Components.ID("{77DA64D3-7458-4920-9491-86CC9914F904}"),
  name: "NetworkGeolocationProvider",
  QueryInterface: ChromeUtils.generateQI([
    "nsIGeolocationProvider",
    "nsIWifiListener",
    "nsITimerCallback",
    "nsIObserver",
    "nsINamed",
  ]),
  listener: null,

  get isWifiScanningEnabled() {
    return Cc["@mozilla.org/wifi/monitor;1"] && this._wifiScanningEnabled;
  },

  resetTimer() {
    if (this.timer) {
      this.timer.cancel();
      this.timer = null;
    }
    this.timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
    this.timer.initWithCallback(
      this,
      this._wifiMonitorTimeout,
      this.timer.TYPE_REPEATING_SLACK
    );
  },

  startup() {
    lazy.log.debug("startup called.");
    if (this.started) {
      return;
    }

    this.started = true;

    if (this.isWifiScanningEnabled) {
      if (this.wifiService) {
        this.wifiService.stopWatching(this);
      }
      this.wifiService = Cc["@mozilla.org/wifi/monitor;1"].getService(
        Ci.nsIWifiMonitor
      );
      this.wifiService.startWatching(this, false);
    }

    this.resetTimer();
  },

  watch(c) {
    lazy.log.debug("watch called");
    this.listener = c;
    this.notify();
    this.resetTimer();
  },

  shutdown() {
    lazy.log.debug("shutdown called");
    if (!this.started) {
      return;
    }

    gCachedRequest = null;

    if (this.timer) {
      this.timer.cancel();
      this.timer = null;
    }

    if (this.wifiService) {
      this.wifiService.stopWatching(this);
      this.wifiService = null;
    }

    this.listener = null;
    this.started = false;
  },

  setHighAccuracy(enable) {
    if (Services.prefs.getBoolPref("geo.provider.testing", false)) {
      Services.obs.notifyObservers(
        null,
        "testing-geolocation-high-accuracy",
        enable
      );
    }
  },

  onChange(accessPoints) {
    this.resetTimer();

    let wifiData = null;
    if (accessPoints) {
      wifiData = lazy.LocationHelper.formatWifiAccessPoints(accessPoints);
    }
    this.sendLocationRequest(wifiData);
  },

  onError(code) {
    lazy.log.debug("wifi error: " + code);
    this.sendLocationRequest(null);
  },

  onStatus(err, statusMessage) {
    if (!this.listener) {
      return;
    }
    lazy.log.debug("onStatus called." + statusMessage);

    if (statusMessage && this.listener.notifyStatus) {
      this.listener.notifyStatus(statusMessage);
    }

    if (err && this.listener.notifyError) {
      this.listener.notifyError(POSITION_UNAVAILABLE, statusMessage);
    }
  },

  notify() {
    this.onStatus(false, "wifi-timeout");
    this.sendLocationRequest(null);
  },

  async sendLocationRequest(wifiData) {
    let data = { wifiAccessPoints: undefined };
    if (wifiData && wifiData.length >= 2) {
      data.wifiAccessPoints = wifiData;
    }

    let useCached = isCachedRequestMoreAccurateThanServerRequest(
      data.wifiAccessPoints
    );

    lazy.log.debug(
      "Use request cache:" + useCached + " reason:" + gDebugCacheReasoning
    );

    if (useCached) {

      gCachedRequest.location.timestamp = Date.now();
      if (this.listener) {
        this.listener.update(gCachedRequest.location);
      }
      return;
    }

    let url = Services.urlFormatter.formatURLPref("geo.provider.network.url");
    let logStr = data.wifiAccessPoints ? " with wifi APs" : "";
    lazy.log.info(
      `Sending IP-address-based geolocation request${logStr} to network service: ${url}`
    );

    let result;
    try {
      result = await this.fetchLocation(url, wifiData);
      lazy.log.info(
        `geo provider reported: ${result.location.lng}:${result.location.lat}`
      );
      let newLocation = new NetworkGeoPositionObject(
        result.location.lat,
        result.location.lng,
        result.accuracy
      );

      if (this.listener) {
        this.listener.update(newLocation);
      }

      gCachedRequest = new CachedRequest(newLocation, data.wifiAccessPoints);
    } catch (err) {
      lazy.log.error("Location request hit error: " + err.name);
      console.error(err);
      if (err.name == "AbortError") {
        this.onStatus(true, "xhr-timeout");
      } else {
        this.onStatus(true, "xhr-error");
      }
    }
  },

  async fetchLocation(url, wifiData) {
    this.onStatus(false, "xhr-start");

    let fetchController = new AbortController();
    let fetchOpts = {
      method: "POST",
      headers: { "Content-Type": "application/json; charset=UTF-8" },
      credentials: "omit",
      signal: fetchController.signal,
    };

    if (wifiData) {
      fetchOpts.body = JSON.stringify({ wifiAccessPoints: wifiData });
    }

    let timeoutId = lazy.setTimeout(
      () => fetchController.abort(),
      Services.prefs.getIntPref("geo.provider.network.timeout", 60000)
    );

    let isWifi = wifiData && wifiData.length >= 2;
    let label = isWifi ? "network_wifi_and_ip" : "network_ip";

    let response;
    try {
      response = await fetch(url, fetchOpts);
    } catch (err) {
      throw err;
    } finally {
      lazy.clearTimeout(timeoutId);
    }

    if (!response.ok) {
      throw new Error(
        `The geolocation provider returned a non-ok status ${response.status}`,
        { cause: await response.text() }
      );
    }

    let result = response.json();
    return result;
  },
};
