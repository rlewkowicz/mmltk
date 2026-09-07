/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  FXA_PWDMGR_HOST: "resource://gre/modules/FxAccountsCommon.sys.mjs",
  FXA_PWDMGR_REALM: "resource://gre/modules/FxAccountsCommon.sys.mjs",
  LoginBreaches: "resource:///modules/LoginBreaches.sys.mjs",
  LoginHelper: "resource://gre/modules/LoginHelper.sys.mjs",
  PrivacyMetricsService:
    "moz-src:///browser/components/protections/PrivacyMetricsService.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "fxAccounts", () => {
  return ChromeUtils.importESModule(
    "resource://gre/modules/FxAccounts.sys.mjs"
  ).getFxAccountsSingleton();
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "TrackingDBService",
  "@mozilla.org/tracking-db-service;1",
  Ci.nsITrackingDBService
);

let idToTextMap = new Map([
  [Ci.nsITrackingDBService.TRACKERS_ID, "tracker"],
  [Ci.nsITrackingDBService.TRACKING_COOKIES_ID, "cookie"],
  [Ci.nsITrackingDBService.CRYPTOMINERS_ID, "cryptominer"],
  [Ci.nsITrackingDBService.FINGERPRINTERS_ID, "fingerprinter"],
  [Ci.nsITrackingDBService.SUSPICIOUS_FINGERPRINTERS_ID, "fingerprinter"],
  [Ci.nsITrackingDBService.SOCIAL_ID, "social"],
]);

const MONITOR_API_ENDPOINT = Services.urlFormatter.formatURLPref(
  "browser.contentblocking.report.endpoint_url"
);

const SCOPE_MONITOR = [
  "profile:uid",
  "https://identity.mozilla.com/apps/monitor",
];

const SCOPE_VPN = "profile https://identity.mozilla.com/account/subscriptions";
const VPN_ENDPOINT = `${Services.prefs.getStringPref(
  "identity.fxaccounts.auth.uri"
)}oauth/subscriptions/active`;

const VPN_SUB_ID = Services.prefs.getStringPref(
  "browser.contentblocking.report.vpn_sub_id"
);

const INVALID_OAUTH_TOKEN = "Invalid OAuth token";
const USER_UNSUBSCRIBED_TO_MONITOR = "User is not subscribed to Monitor";
const SERVICE_UNAVAILABLE = "Service unavailable";
const UNEXPECTED_RESPONSE = "Unexpected response";
const UNKNOWN_ERROR = "Unknown error";

const MONITOR_RESPONSE_PROPS = [
  "monitoredEmails",
  "numBreaches",
  "passwords",
  "numBreachesResolved",
  "passwordsResolved",
];

let gTestOverride = null;
let monitorResponse = null;
let entrypoint = "direct";

export class AboutProtectionsParent extends JSWindowActorParent {
  constructor() {
    super();
  }

  static setTestOverride(callback) {
    gTestOverride = callback;
  }

  async fetchUserBreachStats(token) {
    if (monitorResponse && monitorResponse.timestamp) {
      var timeDiff = Date.now() - monitorResponse.timestamp;
      let oneDayInMS = 24 * 60 * 60 * 1000;
      if (timeDiff >= oneDayInMS) {
        monitorResponse = null;
      } else {
        return monitorResponse;
      }
    }

    const headers = new Headers();
    headers.append("Authorization", `Bearer ${token}`);
    const request = new Request(MONITOR_API_ENDPOINT, { headers });
    const response = await fetch(request);

    if (response.ok) {
      const json = await response.json();

      let isValid = null;
      for (let prop in json) {
        isValid = MONITOR_RESPONSE_PROPS.includes(prop);

        if (!isValid) {
          break;
        }
      }

      monitorResponse = isValid ? json : new Error(UNEXPECTED_RESPONSE);
      if (isValid) {
        monitorResponse.timestamp = Date.now();
      }
    } else {
      switch (response.status) {
        case 400:
        case 401:
          monitorResponse = new Error(INVALID_OAUTH_TOKEN);
          break;
        case 404:
          monitorResponse = new Error(USER_UNSUBSCRIBED_TO_MONITOR);
          break;
        case 503:
          monitorResponse = new Error(SERVICE_UNAVAILABLE);
          break;
        default:
          monitorResponse = new Error(UNKNOWN_ERROR);
          break;
      }
    }

    if (monitorResponse instanceof Error) {
      throw monitorResponse;
    }
    return monitorResponse;
  }

  async getLoginData() {
    if (gTestOverride && "getLoginData" in gTestOverride) {
      return gTestOverride.getLoginData();
    }

    try {
      if (await lazy.fxAccounts.getSignedInUser()) {
        await lazy.fxAccounts.device.refreshDeviceList();
      }
    } catch (e) {
      console.error("There was an error fetching login data: ", e.message);
    }

    const userFacingLogins =
      (await Services.logins.countLoginsAsync("", "", "")) -
      (await Services.logins.countLoginsAsync(
        lazy.FXA_PWDMGR_HOST,
        null,
        lazy.FXA_PWDMGR_REALM
      ));

    let potentiallyBreachedLogins = null;
    if (userFacingLogins && Services.logins.isLoggedIn) {
      const logins = await lazy.LoginHelper.getAllUserFacingLogins();
      potentiallyBreachedLogins =
        await lazy.LoginBreaches.getPotentialBreachesByLoginGUID(logins);
    }

    let mobileDeviceConnected =
      lazy.fxAccounts.device.recentDeviceList &&
      lazy.fxAccounts.device.recentDeviceList.filter(
        device => device.type == "mobile"
      ).length;

    return {
      numLogins: userFacingLogins,
      potentiallyBreachedLogins: potentiallyBreachedLogins
        ? potentiallyBreachedLogins.size
        : 0,
      mobileDeviceConnected,
    };
  }


  async getMonitorData() {
    if (gTestOverride && "getMonitorData" in gTestOverride) {
      monitorResponse = gTestOverride.getMonitorData();
      monitorResponse.timestamp = Date.now();
      monitorResponse = await this.fetchUserBreachStats();
      return monitorResponse;
    }

    let monitorData = {};
    let userEmail = null;
    let token = await this.getMonitorScopedOAuthToken();

    try {
      if (token) {
        monitorData = await this.fetchUserBreachStats(token);

        const { email } = await lazy.fxAccounts.getSignedInUser();
        userEmail = email;
      } else {
        monitorData = {
          errorMessage: "No account",
        };
      }
    } catch (e) {
      console.error(e.message);
      monitorData.errorMessage = e.message;

      if (e.message === INVALID_OAUTH_TOKEN) {
        await lazy.fxAccounts.removeCachedOAuthToken({ token });
        token = await this.getMonitorScopedOAuthToken();

        try {
          monitorData = await this.fetchUserBreachStats(token);
        } catch (_) {
          console.error(e.message);
        }
      } else if (e.message === USER_UNSUBSCRIBED_TO_MONITOR) {
        const { email } = await lazy.fxAccounts.getSignedInUser();
        userEmail = email;
      } else {
        monitorData.errorMessage = e.message || "An error ocurred.";
      }
    }

    return {
      ...monitorData,
      userEmail,
      error: !!monitorData.errorMessage,
    };
  }

  async getMonitorScopedOAuthToken() {
    let token = null;

    try {
      token = await lazy.fxAccounts.getOAuthToken({ scope: SCOPE_MONITOR });
    } catch (e) {
      console.error(
        "There was an error fetching the user's token: ",
        e.message
      );
    }

    return token;
  }

  async VPNSubStatus() {
    if (gTestOverride && "vpnOverrides" in gTestOverride) {
      return gTestOverride.vpnOverrides();
    }

    let vpnToken;
    try {
      vpnToken = await lazy.fxAccounts.getOAuthToken({ scope: SCOPE_VPN });
    } catch (e) {
      console.error(
        "There was an error fetching the user's token: ",
        e.message
      );
      return false;
    }
    let headers = new Headers();
    headers.append("Authorization", `Bearer ${vpnToken}`);
    const request = new Request(VPN_ENDPOINT, { headers });
    const res = await fetch(request);
    if (res.ok) {
      const result = await res.json();
      for (let sub of result) {
        if (sub.subscriptionId == VPN_SUB_ID) {
          return true;
        }
      }
      return false;
    }
    return false;
  }

  async receiveMessage(aMessage) {
    let win = this.browsingContext.top.embedderElement.documentGlobal;
    switch (aMessage.name) {
      case "OpenAboutLogins":
        lazy.LoginHelper.openPasswordManager(win, {
          entryPoint: "Aboutprotections",
        });
        break;
      case "OpenContentBlockingPreferences":
        win.openPreferences("privacy-trackingprotection", {
          origin: "about-protections",
        });
        break;
      case "OpenSyncPreferences":
        win.openTrustedLinkIn("about:preferences#sync", "tab");
        break;
      case "FetchContentBlockingEvents": {
        let dataToSend = {};
        let displayNames = new Services.intl.DisplayNames(undefined, {
          type: "weekday",
          style: "abbreviated",
          calendar: "gregory",
        });

        let weekdays = [7, 1, 2, 3, 4, 5, 6].map(day => displayNames.of(day));
        dataToSend.weekdays = weekdays;

        if (lazy.PrivateBrowsingUtils.isWindowPrivate(win)) {
          dataToSend.isPrivate = true;
          return dataToSend;
        }
        let sumEvents = await lazy.TrackingDBService.sumAllEvents();
        let earliestDate =
          await lazy.TrackingDBService.getEarliestRecordedDate();
        let eventsByDate = await lazy.TrackingDBService.getEventsByDateRange(
          aMessage.data.from,
          aMessage.data.to
        );
        let largest = 0;

        for (let result of eventsByDate) {
          let count = result.getResultByName("count");
          let type = result.getResultByName("type");
          let timestamp = result.getResultByName("timestamp");
          let typeStr = idToTextMap.get(type);
          dataToSend[timestamp] = dataToSend[timestamp] ?? { total: 0 };
          let currentCnt = dataToSend[timestamp][typeStr] ?? 0;
          currentCnt += count;
          dataToSend[timestamp][typeStr] = currentCnt;
          dataToSend[timestamp].total += count;
          if (largest < dataToSend[timestamp].total) {
            largest = dataToSend[timestamp].total;
          }
        }
        dataToSend.largest = largest;
        dataToSend.earliestDate = earliestDate;
        dataToSend.sumEvents = sumEvents;

        return dataToSend;
      }

      case "FetchMonitorData":
        return this.getMonitorData();

      case "FetchUserLoginsData":
        return this.getLoginData();

      case "ClearMonitorCache":
        monitorResponse = null;
        break;

      case "RecordEntryPoint":
        entrypoint = aMessage.data.entrypoint;
        break;

      case "FetchEntryPoint":
        return entrypoint;

      case "FetchVPNSubStatus":
        return this.VPNSubStatus();

      case "FetchShowVPNCard":
        return lazy.BrowserUtils.shouldShowVPNPromo();

      case "FetchPrivacyMetrics":
        if (lazy.PrivateBrowsingUtils.isWindowPrivate(win)) {
          return { isPrivate: true };
        }
        return lazy.PrivacyMetricsService.getWeeklyStats();
    }

    return undefined;
  }
}
