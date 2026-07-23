/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const THREE_DAYS_MS = 3 * 24 * 60 * 1000;

const lazy = {};

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "gClassifier",
  "@mozilla.org/url-classifier/dbservice;1",
  Ci.nsIURIClassifier
);
XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "gStorageActivityService",
  "@mozilla.org/storage/activity-service;1",
  Ci.nsIStorageActivityService
);

ChromeUtils.defineLazyGetter(lazy, "gClassifierFeature", () => {
  return lazy.gClassifier.getFeatureByName("tracking-annotation");
});

ChromeUtils.defineLazyGetter(lazy, "logger", () => {
  return console.createInstance({
    prefix: "*** PurgeTrackerService:",
    maxLogLevelPref: "privacy.purge_trackers.logging.level",
  });
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "gConsiderEntityList",
  "privacy.purge_trackers.consider_entity_list"
);

export function PurgeTrackerService() {}

PurgeTrackerService.prototype = {
  classID: Components.ID("{90d1fd17-2018-4e16-b73c-a04a26fa6dd4}"),
  QueryInterface: ChromeUtils.generateQI(["nsIPurgeTrackerService"]),

  _firstIteration: true,

  _trackingState: new Map(),

  _purgeRunning: false,

  observe(aSubject, aTopic) {
    switch (aTopic) {
      case "idle-daily":
        if (!this._purgeRunning) {
          this._purgeRunning = true;
          this.purgeTrackingCookieJars();
        }
        break;
      case "profile-after-change":
        Services.obs.addObserver(this, "idle-daily");
        Services.obs.addObserver(this, "quit-application");
        break;
      case "quit-application":
        Services.obs.removeObserver(this, "idle-daily");
        Services.obs.removeObserver(this, "quit-application");
        break;
    }
  },

  async isTracker(principal) {
    if (principal.isNullPrincipal || principal.isSystemPrincipal) {
      return false;
    }
    let host;
    try {
      host = principal.asciiHost;
    } catch (error) {
      return false;
    }

    if (!this._trackingState.has(host)) {
      this._trackingState.set(host, false);

      await new Promise(resolve => {
        try {
          lazy.gClassifier.asyncClassifyLocalWithFeatures(
            principal.URI,
            [lazy.gClassifierFeature],
            Ci.nsIUrlClassifierFeature.blocklist,
            list => {
              if (list.length) {
                this._trackingState.set(host, true);
              }
              resolve();
            }
          );
        } catch {
          this._trackingState.set(host, false);
          resolve();
        }
      });
    }

    return this._trackingState.get(host);
  },

  isAllowedThirdParty(firstPartyOriginNoSuffix, thirdPartyHost) {
    let uri = Services.io.newURI(
      `${firstPartyOriginNoSuffix}/?resource=${thirdPartyHost}`
    );
    lazy.logger.debug(`Checking entity list state for`, uri.spec);
    return new Promise(resolve => {
      try {
        lazy.gClassifier.asyncClassifyLocalWithFeatures(
          uri,
          [lazy.gClassifierFeature],
          Ci.nsIUrlClassifierFeature.entitylist,
          list => {
            let sameList = !!list.length;
            lazy.logger.debug(`Is ${uri.spec} on the entity list?`, sameList);
            resolve(sameList);
          }
        );
      } catch {
        resolve(false);
      }
    });
  },

  async maybePurgePrincipal(principal) {
    let origin = principal.origin;
    lazy.logger.debug(`Maybe purging ${origin}.`);

    let hasInteraction = this._baseDomainsWithInteraction.has(
      principal.baseDomain
    );
    if (hasInteraction && !Services.telemetry.canRecordPrereleaseData) {
      lazy.logger.debug(`${origin} has user interaction, exiting.`);
      return;
    }

    let isTracker = await this.isTracker(principal);
    if (!isTracker) {
      lazy.logger.debug(`${origin} is not a tracker, exiting.`);
      return;
    }

    if (hasInteraction) {
      let expireTimeMs = this._baseDomainsWithInteraction.get(
        principal.baseDomain
      );

      let timeRemaining = Math.floor(
        (expireTimeMs - Date.now()) / 1000 / 60 / 60 / 24
      );

      this._telemetryData.notPurged.add(principal.baseDomain);

      lazy.logger.debug(`${origin} is a tracker with interaction, exiting.`);
      return;
    }

    let isAllowedThirdParty = false;
    if (
      lazy.gConsiderEntityList ||
      Services.telemetry.canRecordPrereleaseData
    ) {
      for (let firstPartyPrincipal of this._principalsWithInteraction) {
        if (
          await this.isAllowedThirdParty(
            firstPartyPrincipal.originNoSuffix,
            principal.asciiHost
          )
        ) {
          isAllowedThirdParty = true;
          break;
        }
      }
    }

    if (isAllowedThirdParty && lazy.gConsiderEntityList) {
      lazy.logger.debug(
        `${origin} has interaction on the entity list, exiting.`
      );
      return;
    }

    lazy.logger.log("Deleting data from:", origin);

    await new Promise(resolve => {
      Services.clearData.deleteDataFromPrincipal(
        principal,
        false,
        Ci.nsIClearDataService.CLEAR_STATE_FOR_TRACKER_PURGING,
        resolve
      );
    });
    lazy.logger.log(`Data deleted from:`, origin);

    this._telemetryData.purged.add(principal.baseDomain);
  },

  resetPurgeList() {
    this._purgeRunning = false;
    Services.prefs.setStringPref(
      "privacy.purge_trackers.date_in_cookie_database",
      "0"
    );
  },

  submitTelemetry() {
    let { purged, notPurged, durationIntervals } = this._telemetryData;
    let now = Date.now();
    let lastPurge = Number(
      Services.prefs.getStringPref("privacy.purge_trackers.last_purge", now)
    );
    let hoursBetween = Math.floor((now - lastPurge) / 1000 / 60 / 60);

    Services.prefs.setStringPref(
      "privacy.purge_trackers.last_purge",
      now.toString()
    );

    let duration = durationIntervals
      .map(([start, end]) => end - start)
      .reduce((acc, cur) => acc + cur, 0);
  },

  checkCookiePermissions(httpsPrincipal, httpPrincipal) {
    let httpsCookiePermission;
    let httpCookiePermission;

    if (httpPrincipal) {
      httpCookiePermission = Services.perms.testPermissionFromPrincipal(
        httpPrincipal,
        "cookie"
      );
    }

    if (httpsPrincipal) {
      httpsCookiePermission = Services.perms.testPermissionFromPrincipal(
        httpsPrincipal,
        "cookie"
      );
    }

    if (
      httpCookiePermission == Ci.nsICookiePermission.ACCESS_ALLOW ||
      httpsCookiePermission == Ci.nsICookiePermission.ACCESS_ALLOW
    ) {
      return true;
    }

    return false;
  },
  async purgeTrackingCookieJars() {
    let purgeEnabled = Services.prefs.getBoolPref(
      "privacy.purge_trackers.enabled",
      false
    );

    let sanitizeOnShutdownEnabled = Services.prefs.getBoolPref(
      "privacy.sanitize.sanitizeOnShutdown",
      false
    );

    let clearHistoryOnShutdown = Services.prefs.getBoolPref(
      "privacy.clearOnShutdown.history",
      false
    );

    let clearSiteSettingsOnShutdown = Services.prefs.getBoolPref(
      "privacy.clearOnShutdown.siteSettings",
      false
    );

    if (
      sanitizeOnShutdownEnabled &&
      (clearHistoryOnShutdown || clearSiteSettingsOnShutdown)
    ) {
      lazy.logger.log(
        `
        Purging canceled because interaction permissions are cleared on shutdown.
        sanitizeOnShutdownEnabled: ${sanitizeOnShutdownEnabled},
        clearHistoryOnShutdown: ${clearHistoryOnShutdown},
        clearSiteSettingsOnShutdown: ${clearSiteSettingsOnShutdown},
        `
      );
      this.resetPurgeList();
      return;
    }

    let cookieBehavior = Services.cookies.getCookieBehavior(false);

    let activeWithCookieBehavior =
      cookieBehavior == Ci.nsICookieService.BEHAVIOR_REJECT_FOREIGN ||
      cookieBehavior == Ci.nsICookieService.BEHAVIOR_LIMIT_FOREIGN ||
      cookieBehavior == Ci.nsICookieService.BEHAVIOR_REJECT_TRACKER ||
      cookieBehavior == Ci.nsICookieService.BEHAVIOR_PARTITION_FOREIGN;

    if (!activeWithCookieBehavior || !purgeEnabled) {
      lazy.logger.log(
        `returning early, activeWithCookieBehavior: ${activeWithCookieBehavior}, purgeEnabled: ${purgeEnabled}`
      );
      this.resetPurgeList();
      return;
    }
    lazy.logger.log("Purging trackers enabled, beginning batch.");
    const MAX_PURGE_COUNT = Services.prefs.getIntPref(
      "privacy.purge_trackers.max_purge_count",
      100
    );

    if (this._firstIteration) {
      this._telemetryData = {
        durationIntervals: [],
        purged: new Set(),
        notPurged: new Set(),
      };

      this._baseDomainsWithInteraction = new Map();
      this._principalsWithInteraction = [];
      for (let perm of Services.perms.getAllWithTypePrefix(
        "storageAccessAPI"
      )) {
        this._baseDomainsWithInteraction.set(
          perm.principal.baseDomain,
          perm.expireTime
        );
        this._principalsWithInteraction.push(perm.principal);
      }
    }

    let duration = [ChromeUtils.now()];

    let saved_date = Services.prefs.getStringPref(
      "privacy.purge_trackers.date_in_cookie_database",
      "0"
    );

    let maybeClearPrincipals = new Map();

    let cookies = Services.cookies.getCookiesSince(saved_date);
    cookies = cookies.slice(0, MAX_PURGE_COUNT);

    for (let cookie of cookies) {
      let httpPrincipal;
      let httpsPrincipal;

      let origin =
        "http://" +
        cookie.rawHost +
        ChromeUtils.originAttributesToSuffix(cookie.originAttributes);
      try {
        httpPrincipal =
          Services.scriptSecurityManager.createContentPrincipalFromOrigin(
            origin
          );
      } catch (e) {
        lazy.logger.error(
          `Creating principal from origin ${origin} led to error ${e}.`
        );
      }

      origin =
        "https://" +
        cookie.rawHost +
        ChromeUtils.originAttributesToSuffix(cookie.originAttributes);
      try {
        httpsPrincipal =
          Services.scriptSecurityManager.createContentPrincipalFromOrigin(
            origin
          );
      } catch (e) {
        lazy.logger.error(
          `Creating principal from origin ${origin} led to error ${e}.`
        );
      }

      let purgeCheck = this.checkCookiePermissions(
        httpsPrincipal,
        httpPrincipal
      );

      if (httpPrincipal && !purgeCheck) {
        maybeClearPrincipals.set(httpPrincipal.origin, httpPrincipal);
      }
      if (httpsPrincipal && !purgeCheck) {
        maybeClearPrincipals.set(httpsPrincipal.origin, httpsPrincipal);
      }

      saved_date = cookie.creationTime;
    }

    if (this._firstIteration) {
      let startDate = Date.now() - THREE_DAYS_MS;
      let storagePrincipals = lazy.gStorageActivityService.getActiveOrigins(
        startDate * 1000,
        Date.now() * 1000
      );

      for (let principal of storagePrincipals.enumerate()) {
        if (principal.schemeIs("https") || principal.schemeIs("http")) {
          let otherURI;
          let otherPrincipal;

          if (principal.schemeIs("https")) {
            otherURI = principal.URI.mutate().setScheme("http").finalize();
          } else if (principal.schemeIs("http")) {
            otherURI = principal.URI.mutate().setScheme("https").finalize();
          }

          try {
            otherPrincipal =
              Services.scriptSecurityManager.createContentPrincipal(
                otherURI,
                {}
              );
          } catch (e) {
            lazy.logger.error(
              `Creating principal from URI ${otherURI} led to error ${e}.`
            );
          }

          if (!this.checkCookiePermissions(principal, otherPrincipal)) {
            maybeClearPrincipals.set(principal.origin, principal);
          }
        } else {
          maybeClearPrincipals.set(principal.origin, principal);
        }
      }
    }

    for (let principal of maybeClearPrincipals.values()) {
      await this.maybePurgePrincipal(principal);
    }

    Services.prefs.setStringPref(
      "privacy.purge_trackers.date_in_cookie_database",
      saved_date
    );

    duration.push(ChromeUtils.now());
    this._telemetryData.durationIntervals.push(duration);

    if (!cookies.length || cookies.length < 100) {
      lazy.logger.log(
        "All cookie purging finished, resetting list until tomorrow."
      );
      this.resetPurgeList();
      this.submitTelemetry();
      this._firstIteration = true;
      return;
    }

    lazy.logger.log("Batch finished, queueing next batch.");
    this._firstIteration = false;
    await new Promise((resolve, reject) => {
      Services.tm.idleDispatchToMainThread(() => {
        this.purgeTrackingCookieJars().then(resolve, reject);
      });
    });
  },
};
