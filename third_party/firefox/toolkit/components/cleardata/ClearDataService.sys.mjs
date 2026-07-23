/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  Downloads: "resource://gre/modules/Downloads.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  ServiceWorkerCleanUp: "resource://gre/modules/ServiceWorkerCleanUp.sys.mjs",
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "sas",
  "@mozilla.org/storage/activity-service;1",
  Ci.nsIStorageActivityService
);
XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "TrackingDBService",
  "@mozilla.org/tracking-db-service;1",
  Ci.nsITrackingDBService
);
XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "bounceTrackingProtection",
  "@mozilla.org/bounce-tracking-protection;1",
  Ci.nsIBounceTrackingProtection
);
XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "nssComponent",
  "@mozilla.org/psm;1",
  Ci.nsINSSComponent
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "bounceTrackingProtectionMode",
  "privacy.bounceTrackingProtection.mode",
  Ci.nsIBounceTrackingProtection.MODE_DISABLED
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "permissionManagerIsolateByPrivateBrowsing",
  "permissions.isolateBy.privateBrowsing",
  false
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "permissionManagerIsolateByUserContext",
  "permissions.isolateBy.userContext",
  false
);

class PBMCleanupCollector {
  #promises = [];

  addPendingCleanup() {
    let { promise, resolve } = Promise.withResolvers();
    this.#promises.push(promise);
    return {
      complete(aStatus) {
        resolve(aStatus);
      },
      QueryInterface: ChromeUtils.generateQI(["nsIPBMCleanupCallback"]),
    };
  }

  get promise() {
    return Promise.allSettled(this.#promises).then(results => {
      let dominated = false;
      for (let r of results) {
        if (r.status === "fulfilled" && r.value !== Cr.NS_OK) {
          dominated = true;
          break;
        }
        if (r.status === "rejected") {
          dominated = true;
          break;
        }
      }
      return dominated;
    });
  }

  QueryInterface = ChromeUtils.generateQI(["nsIPBMCleanupCollector"]);
}

let gPBMCleanupInProgress = false;

function maybeFixupIpv6(host) {
  if (!host?.includes(":")) {
    return host;
  }

  if (host.startsWith("[") && host.endsWith("]")) {
    return host;
  }

  return `[${host}]`;
}

function hasSite(
  { host = null, originAttributes = null, principal = null },
  aSchemelessSite,
  aOriginAttributesPattern = {}
) {
  if (!aSchemelessSite) {
    throw new Error("Missing aSchemelessSite.");
  }
  if (!host && !originAttributes && !principal) {
    throw new Error(
      "Missing host, originAttributes or principal to match with aSchemelessSite."
    );
  }
  if (principal && (host || originAttributes)) {
    throw new Error(
      "Can only pass either principal or host and originAttributes."
    );
  }

  host = maybeFixupIpv6(host);

  if (
    host &&
    Services.eTLD.hasRootDomain(host, aSchemelessSite) &&
    (!originAttributes ||
      ChromeUtils.originAttributesMatchPattern(
        originAttributes,
        aOriginAttributesPattern
      ))
  ) {
    return true;
  }

  if (
    maybeFixupIpv6(principal?.baseDomain) == aSchemelessSite &&
    ChromeUtils.originAttributesMatchPattern(
      principal.originAttributes,
      aOriginAttributesPattern
    )
  ) {
    return true;
  }

  let oa = originAttributes ?? principal?.originAttributes;
  if (oa == null) {
    return false;
  }

  let patternWithPartitionKey = {
    ...aOriginAttributesPattern,
    partitionKeyPattern: { baseDomain: aSchemelessSite },
  };

  return ChromeUtils.originAttributesMatchPattern(oa, patternWithPartitionKey);
}


const CookieCleaner = {
  deleteByLocalFiles(aOriginAttributes) {
    return new Promise(aResolve => {
      Services.cookies.removeCookiesFromExactHost(
        "",
        JSON.stringify(aOriginAttributes)
      );
      aResolve();
    });
  },

  deleteByHost(aHost, aOriginAttributes) {
    return new Promise(aResolve => {
      Services.cookies.removeCookiesFromExactHost(
        aHost,
        JSON.stringify(aOriginAttributes)
      );
      if (aHost.includes(":") && aHost[0] != "[") {
        aHost = "https://[" + aHost + "]";
      } else {
        aHost = "https://" + aHost;
      }
      let httpsURI = Services.io.newURI(aHost);
      Services.cache2.clearOriginDictionary(httpsURI);
      aResolve();
    });
  },

  deleteByPrincipal(aPrincipal) {
    return this.deleteByHost(aPrincipal.host, aPrincipal.originAttributes);
  },

  async deleteBySite(aSchemelessSite, aOriginAttributesPattern) {
    Services.cookies.cookies
      .filter(({ rawHost, originAttributes }) =>
        hasSite(
          { host: rawHost, originAttributes },
          aSchemelessSite,
          aOriginAttributesPattern
        )
      )
      .forEach(cookie => {
        Services.cookies.removeCookiesFromExactHost(
          cookie.rawHost,
          JSON.stringify(cookie.originAttributes)
        );
      });
    let httpsURI = Services.io.newURI("https://" + aSchemelessSite);
    Services.cache2.clearOriginDictionary(httpsURI);
  },

  deleteByRange(aFrom) {
    return Services.cookies.removeAllSince(aFrom);
  },

  deleteByOriginAttributes(aOriginAttributesString) {
    return new Promise(aResolve => {
      try {
        Services.cookies.removeCookiesWithOriginAttributes(
          aOriginAttributesString
        );
      } catch (ex) {}
      aResolve();
    });
  },

  deleteAll() {
    return new Promise(aResolve => {
      Services.cookies.removeAll();
      Services.cache2.clearAllOriginDictionaries();
      aResolve();
    });
  },
};

const FingerprintingProtectionStateCleaner = {
  async _maybeClearSiteSpecificZoom(aSchemelessSite, aOriginAttributes = {}) {
    if (
      !ChromeUtils.shouldResistFingerprinting("SiteSpecificZoom", null, true)
    ) {
      return;
    }

    const cps2 = Cc["@mozilla.org/content-pref/service;1"].getService(
      Ci.nsIContentPrefService2
    );
    const ZOOM_PREF_NAME = "browser.content.full-zoom";

    await new Promise((aResolve, aReject) => {
      aOriginAttributes =
        ChromeUtils.fillNonDefaultOriginAttributes(aOriginAttributes);

      let loadContext;
      if (
        aOriginAttributes.privateBrowsingId ==
        Services.scriptSecurityManager.DEFAULT_PRIVATE_BROWSING_ID
      ) {
        loadContext = Cu.createLoadContext();
      } else {
        loadContext = Cu.createPrivateLoadContext();
      }

      cps2.removeBySubdomainAndName(
        aSchemelessSite,
        ZOOM_PREF_NAME,
        loadContext,
        {
          handleCompletion: aReason => {
            if (aReason === cps2.COMPLETE_ERROR) {
              aReject();
            } else {
              aResolve();
            }
          },
        }
      );
    });
  },

  async deleteAll() {
    Services.rfp.cleanAllRandomKeys();
  },

  async deleteByPrincipal(aPrincipal) {
    Services.rfp.cleanRandomKeyByPrincipal(aPrincipal);

    await this._maybeClearSiteSpecificZoom(
      aPrincipal.host,
      aPrincipal.originAttributes
    );
  },

  async deleteBySite(aSchemelessSite, aOriginAttributesPattern) {
    Services.rfp.cleanRandomKeyBySite(
      aSchemelessSite,
      aOriginAttributesPattern
    );

    await this._maybeClearSiteSpecificZoom(
      aSchemelessSite,
      aOriginAttributesPattern
    );
  },

  async deleteByHost(aHost, aOriginAttributesPattern) {
    Services.rfp.cleanRandomKeyByHost(
      aHost,
      JSON.stringify(aOriginAttributesPattern)
    );

    await this._maybeClearSiteSpecificZoom(aHost, aOriginAttributesPattern);
  },

  async deleteByOriginAttributes(aOriginAttributesString) {
    Services.rfp.cleanRandomKeyByOriginAttributesPattern(
      aOriginAttributesString
    );

  },
};

const CertCleaner = {
  async deleteByHost(aHost, aOriginAttributes) {
    let overrideService = Cc["@mozilla.org/security/certoverride;1"].getService(
      Ci.nsICertOverrideService
    );

    overrideService.clearValidityOverride(aHost, -1, aOriginAttributes);
  },

  deleteByPrincipal(aPrincipal) {
    return this.deleteByHost(aPrincipal.host, aPrincipal.originAttributes);
  },

  async deleteBySite(aSchemelessSite, aOriginAttributesPattern) {
    let overrideService = Cc["@mozilla.org/security/certoverride;1"].getService(
      Ci.nsICertOverrideService
    );
    overrideService
      .getOverrides()
      .filter(({ asciiHost, originAttributes }) =>
        hasSite(
          { host: asciiHost, originAttributes },
          aSchemelessSite,
          aOriginAttributesPattern
        )
      )
      .forEach(({ asciiHost, port }) =>
        overrideService.clearValidityOverride(asciiHost, port, {})
      );
  },

  async deleteAll() {
    let overrideService = Cc["@mozilla.org/security/certoverride;1"].getService(
      Ci.nsICertOverrideService
    );

    overrideService.clearAllOverrides();
  },
};

const NetworkCacheCleaner = {
  async deleteByHost(aHost, aOriginAttributes) {
    let httpURI = Services.io.newURI("http://" + aHost);
    let httpsURI = Services.io.newURI("https://" + aHost);
    let httpPrincipal = Services.scriptSecurityManager.createContentPrincipal(
      httpURI,
      aOriginAttributes
    );
    let httpsPrincipal = Services.scriptSecurityManager.createContentPrincipal(
      httpsURI,
      aOriginAttributes
    );

    Services.cache2.clearOriginsByPrincipal(httpPrincipal);
    Services.cache2.clearOriginsByPrincipal(httpsPrincipal);
  },

  async deleteBySite(aSchemelessSite, _aOriginAttributesPattern) {
    Services.cache2.clearBaseDomain(aSchemelessSite);
  },

  deleteByPrincipal(aPrincipal) {
    return new Promise(aResolve => {
      Services.cache2.clearOriginsByPrincipal(aPrincipal);
      aResolve();
    });
  },

  deleteByOriginAttributes(aOriginAttributesString) {
    return new Promise(aResolve => {
      Services.cache2.clearOriginsByOriginAttributes(aOriginAttributesString);
      aResolve();
    });
  },

  deleteAll() {
    return new Promise(aResolve => {
      Services.cache2.clear();
      aResolve();
    });
  },
};

const createResourceCleaner = type => ({
  async deleteByHost(aHost, aOriginAttributes) {
    let httpURI = Services.io.newURI("http://" + aHost);
    let httpsURI = Services.io.newURI("https://" + aHost);
    let httpPrincipal = Services.scriptSecurityManager.createContentPrincipal(
      httpURI,
      aOriginAttributes
    );
    let httpsPrincipal = Services.scriptSecurityManager.createContentPrincipal(
      httpsURI,
      aOriginAttributes
    );

    this.deleteByPrincipal(httpPrincipal);
    this.deleteByPrincipal(httpsPrincipal);
  },

  async deleteByPrincipal(aPrincipal) {
    ChromeUtils.clearResourceCache({
      types: [type],
      principal: aPrincipal,
    });
  },

  async deleteBySite(aSchemelessSite, aOriginAttributesPattern) {
    ChromeUtils.clearResourceCache({
      types: [type],
      schemelessSite: aSchemelessSite,
      pattern: aOriginAttributesPattern,
    });
  },

  async deleteAll() {
    ChromeUtils.clearResourceCache({
      types: [type],
    });
  },
});

const CSSCacheCleaner = createResourceCleaner("stylesheet");
const JSCacheCleaner = createResourceCleaner("script");
const ImageCacheCleaner = createResourceCleaner("image");

const DownloadsCleaner = {
  async _deleteInternal({ host, principal, originAttributes }) {
    originAttributes = originAttributes || principal?.originAttributes || {};

    let list = await lazy.Downloads.getList(lazy.Downloads.ALL);
    list.removeFinished(({ source }) => {
      if (
        "userContextId" in originAttributes &&
        "userContextId" in source &&
        originAttributes.userContextId != source.userContextId
      ) {
        return false;
      }
      if (
        "privateBrowsingId" in originAttributes &&
        !!originAttributes.privateBrowsingId != source.isPrivate
      ) {
        return false;
      }

      let entryURI = Services.io.newURI(source.url);
      if (host) {
        return Services.eTLD.hasRootDomain(entryURI.host, host);
      }
      if (principal) {
        return principal.equalsURI(entryURI);
      }
      return false;
    });
  },

  async deleteByHost(aHost, aOriginAttributes) {
    return this._deleteInternal({
      host: aHost,
      originAttributes: aOriginAttributes,
    });
  },

  deleteByPrincipal(aPrincipal) {
    return this._deleteInternal({ principal: aPrincipal });
  },

  async deleteBySite(aSchemelessSite, aOriginAttributesPattern) {
    let list = await lazy.Downloads.getList(lazy.Downloads.ALL);
    list.removeFinished(({ source }) => {
      if (
        "userContextId" in aOriginAttributesPattern &&
        "userContextId" in source &&
        aOriginAttributesPattern.userContextId != source.userContextId
      ) {
        return false;
      }
      if (
        "privateBrowsingId" in aOriginAttributesPattern &&
        !!aOriginAttributesPattern.privateBrowsingId != source.isPrivate
      ) {
        return false;
      }

      let entryURI = Services.io.newURI(source.url);
      return Services.eTLD.getSchemelessSite(entryURI) == aSchemelessSite;
    });
  },

  deleteByRange(aFrom, aTo) {
    let rangeBeginMs = aFrom / 1000;
    let rangeEndMs = aTo / 1000;

    return lazy.Downloads.getList(lazy.Downloads.ALL).then(aList => {
      aList.removeFinished(
        aDownload =>
          aDownload.startTime >= rangeBeginMs &&
          aDownload.startTime <= rangeEndMs
      );
    });
  },

  deleteAll() {
    return lazy.Downloads.getList(lazy.Downloads.ALL).then(aList => {
      aList.removeFinished(null);
    });
  },
};

const QuotaCleaner = {
  async _qmsClearStoragesForPrincipalsMatching(filterFn) {
    let origins = await new Promise((resolve, reject) => {
      Services.qms.listOrigins().callback = request => {
        if (request.resultCode != Cr.NS_OK) {
          reject({ message: "Deleting quota storages failed" });
          return;
        }
        resolve(request.result);
      };
    });

    let clearPromises = origins
      .map(Services.scriptSecurityManager.createContentPrincipalFromOrigin)
      .filter(filterFn)
      .map(
        principal =>
          new Promise((resolve, reject) => {
            let clearRequest =
              Services.qms.clearStoragesForPrincipal(principal);
            clearRequest.callback = () => {
              if (clearRequest.resultCode != Cr.NS_OK) {
                reject({ message: "Deleting quota storages failed" });
                return;
              }
              resolve();
            };
          })
      );
    return Promise.all(clearPromises);
  },

  deleteByPrincipal(aPrincipal) {
    Services.obs.notifyObservers(
      null,
      "extension:purge-localStorage",
      aPrincipal.host
    );

    Services.sessionStorage.clearStoragesForOrigin(aPrincipal);

    return lazy.ServiceWorkerCleanUp.removeFromPrincipal(aPrincipal)
      .then(
        _ =>  false,
        _ =>  true
      )
      .then(exceptionThrown => {
        return new Promise((aResolve, aReject) => {
          let req = Services.qms.clearStoragesForPrincipal(aPrincipal);
          req.callback = () => {
            if (exceptionThrown || req.resultCode != Cr.NS_OK) {
              aReject({ message: "Delete by principal failed" });
            } else {
              aResolve();
            }
          };
        });
      });
  },

  async deleteBySite(aSchemelessSite, aOriginAttributesPattern) {
    Services.obs.notifyObservers(
      null,
      "extension:purge-localStorage",
      aSchemelessSite
    );

    let entry = Cc["@mozilla.org/clear-by-site-entry;1"].createInstance(
      Ci.nsIClearBySiteEntry
    );
    entry.schemelessSite = aSchemelessSite;
    entry.patternJSON = JSON.stringify(aOriginAttributesPattern);

    Services.obs.notifyObservers(entry, "browser:purge-sessionStorage");

    Services.obs.notifyObservers(
      null,
      "dom-storage:clear-origin-attributes-data",
      JSON.stringify({
        ...aOriginAttributesPattern,
        partitionKeyPattern: { baseDomain: aSchemelessSite },
      })
    );

    let swCleanupError;
    try {
      await lazy.ServiceWorkerCleanUp.removeFromSite(
        aSchemelessSite,
        aOriginAttributesPattern
      );
    } catch (error) {
      swCleanupError = error;
    }

    await this._qmsClearStoragesForPrincipalsMatching(principal =>
      hasSite({ principal }, aSchemelessSite, aOriginAttributesPattern)
    );

    if (swCleanupError) {
      throw swCleanupError;
    }
  },

  async deleteByHost(aHost) {

    Services.obs.notifyObservers(null, "extension:purge-localStorage", aHost);

    Services.obs.notifyObservers(null, "browser:purge-sessionStorage", aHost);

    let swCleanupError;
    try {
      await lazy.ServiceWorkerCleanUp.removeFromHost(aHost);
    } catch (error) {
      swCleanupError = error;
    }

    await this._qmsClearStoragesForPrincipalsMatching(principal => {
      try {
        return Services.eTLD.hasRootDomain(principal.host, aHost);
      } catch (e) {
        return false;
      }
    });

    if (swCleanupError) {
      throw swCleanupError;
    }
  },

  deleteByRange(aFrom, aTo) {
    let principals = lazy.sas
      .getActiveOrigins(aFrom, aTo)
      .QueryInterface(Ci.nsIArray);

    let promises = [];
    for (let i = 0; i < principals.length; ++i) {
      let principal = principals.queryElementAt(i, Ci.nsIPrincipal);

      if (
        !principal.schemeIs("http") &&
        !principal.schemeIs("https") &&
        !principal.schemeIs("file")
      ) {
        continue;
      }

      promises.push(this.deleteByPrincipal(principal));
    }

    return Promise.all(promises);
  },

  deleteByOriginAttributes(aOriginAttributesString) {

    return lazy.ServiceWorkerCleanUp.removeFromOriginAttributes(
      aOriginAttributesString
    )
      .then(
        _ =>  false,
        _ =>  true
      )
      .then(() => {
        return new Promise((aResolve, aReject) => {
          let req = Services.qms.clearStoragesForOriginAttributesPattern(
            aOriginAttributesString
          );
          req.callback = () => {
            if (req.resultCode == Cr.NS_OK) {
              aResolve();
            } else {
              aReject({ message: "Delete by origin attributes failed" });
            }
          };
        });
      });
  },

  async deleteAll() {
    Services.obs.notifyObservers(null, "extension:purge-localStorage");

    Services.obs.notifyObservers(null, "browser:purge-sessionStorage");

    let swCleanupError;
    try {
      await lazy.ServiceWorkerCleanUp.removeAll();
    } catch (error) {
      swCleanupError = error;
    }

    await this._qmsClearStoragesForPrincipalsMatching(
      principal =>
        principal.schemeIs("http") ||
        principal.schemeIs("https") ||
        principal.schemeIs("file")
    );

    if (swCleanupError) {
      throw swCleanupError;
    }
  },

  async cleanupAfterDeletionAtShutdown() {
    const tobeRemoveDirName = "to-be-removed";
    const storageName = Services.prefs.getStringPref(
      "dom.quotaManager.storageName"
    );

    if (!storageName) {
      throw new Error("storage name must not be empty");
    }

    const toBeRemovedDir = PathUtils.join(
      PathUtils.profileDir,
      storageName,
      tobeRemoveDirName
    );

    if (
      !AppConstants.MOZ_BACKGROUNDTASKS ||
      !Services.prefs.getBoolPref("dom.quotaManager.backgroundTask.enabled")
    ) {

      await IOUtils.remove(toBeRemovedDir, { recursive: true });
      return;
    }
    if (!(await IOUtils.hasChildren(toBeRemovedDir, { ignoreAbsent: true }))) {
      return;
    }

    const runner = Cc["@mozilla.org/backgroundtasksrunner;1"].getService(
      Ci.nsIBackgroundTasksRunner
    );

    runner.removeDirectoryInDetachedProcess(
      toBeRemovedDir,
      "",
      "0",
      "*", 
      "Quota"
    );
  },
};

const StorageAccessCleaner = {
  async deleteExceptPrincipals(aPrincipalsWithStorage, aFrom) {
    let baseDomainsWithStorage = new Set();
    for (let principal of aPrincipalsWithStorage) {
      baseDomainsWithStorage.add(principal.baseDomain);
    }
    for (let perm of Services.perms.getAllByTypeSince(
      "storageAccessAPI",
      aFrom / 1000
    )) {
      if (!baseDomainsWithStorage.has(perm.principal.baseDomain)) {
        Services.perms.removePermission(perm);
      }
    }
  },

  async deleteByPrincipal(aPrincipal) {
    return Services.perms.removeFromPrincipal(aPrincipal, "storageAccessAPI");
  },

  _deleteInternal(filter) {
    Services.perms.all
      .filter(({ type }) => type == "storageAccessAPI")
      .filter(filter)
      .forEach(perm => {
        try {
          Services.perms.removePermission(perm);
        } catch (ex) {
          console.error(ex);
        }
      });
  },

  async deleteByHost(aHost) {
    this._deleteInternal(({ principal }) => {
      let toBeRemoved = false;
      try {
        toBeRemoved = Services.eTLD.hasRootDomain(principal.host, aHost);
      } catch (ex) {}
      return toBeRemoved;
    });
  },

  async deleteBySite(aSchemelessSite, aOriginAttributesPattern) {
    if (!lazy.permissionManagerIsolateByPrivateBrowsing) {
      delete aOriginAttributesPattern.privateBrowsingId;
    }
    if (!lazy.permissionManagerIsolateByUserContext) {
      delete aOriginAttributesPattern.userContextId;
    }
    this._deleteInternal(({ principal }) =>
      hasSite({ principal }, aSchemelessSite, aOriginAttributesPattern)
    );
  },

  async deleteByRange(aFrom) {
    Services.perms.removeByTypeSince("storageAccessAPI", aFrom / 1000);
  },

  async deleteAll() {
    Services.perms.removeByType("storageAccessAPI");
  },
};

const HistoryCleaner = {
  deleteByHost(aHost) {
    if (!AppConstants.MOZ_PLACES) {
      return Promise.resolve();
    }
    return lazy.PlacesUtils.history.removeByFilter({ host: "." + aHost });
  },

  deleteByPrincipal(aPrincipal) {
    if (!AppConstants.MOZ_PLACES) {
      return Promise.resolve();
    }
    return lazy.PlacesUtils.history.removeByFilter({ host: aPrincipal.host });
  },

  deleteBySite(aSchemelessSite) {
    return this.deleteByHost(aSchemelessSite);
  },

  deleteByRange(aFrom, aTo) {
    if (!AppConstants.MOZ_PLACES) {
      return Promise.resolve();
    }
    return lazy.PlacesUtils.history.removeVisitsByFilter({
      beginDate: new Date(aFrom / 1000),
      endDate: new Date(aTo / 1000),
    });
  },

  deleteAll() {
    if (!AppConstants.MOZ_PLACES) {
      return Promise.resolve();
    }
    return lazy.PlacesUtils.history.clear();
  },
};

const SessionHistoryCleaner = {
  async deleteByHost(aHost) {
    Services.obs.notifyObservers(null, "browser:purge-sessionStorage", aHost);
    Services.obs.notifyObservers(
      null,
      "browser:purge-session-history-for-domain",
      aHost
    );
  },

  deleteByPrincipal(aPrincipal) {
    return this.deleteByHost(aPrincipal.host, aPrincipal.originAttributes);
  },

  deleteBySite(aSchemelessSite, _aOriginAttributesPattern) {
    return this.deleteByHost(aSchemelessSite, {});
  },

  async deleteByRange(aFrom) {
    Services.obs.notifyObservers(
      null,
      "browser:purge-session-history",
      String(aFrom)
    );
  },

  async deleteAll() {
    Services.obs.notifyObservers(null, "browser:purge-session-history");
  },
};

const AuthCacheCleaner = {
  async deleteByPrincipal(aPrincipal, aIsUserRequest) {
    if (!aIsUserRequest) {
      return;
    }
    await this.deleteAll();
  },

  async deleteBySite(
    _aSchemelessSite,
    _aOriginAttributesPattern,
    aIsUserRequest
  ) {
    if (!aIsUserRequest) {
      return;
    }
    await this.deleteAll();
  },

  deleteAll() {
    return new Promise(aResolve => {
      Services.obs.notifyObservers(null, "net:clear-active-logins");
      aResolve();
    });
  },
};

const SHUTDOWN_EXCEPTION_PERMISSION = "persist-data-on-shutdown";

const ShutdownExceptionsCleaner = {
  async _deleteInternal(filter) {
    Services.perms.all
      .filter(({ type }) => type == SHUTDOWN_EXCEPTION_PERMISSION)
      .filter(filter)
      .forEach(perm => {
        try {
          Services.perms.removePermission(perm);
        } catch (ex) {
          console.error(ex);
        }
      });
  },

  async deleteByHost(aHost) {
    this._deleteInternal(({ principal }) => {
      let { host: principalHost } = principal;
      if (!principalHost?.length) {
        return false;
      }
      return Services.eTLD.hasRootDomain(principal.host, aHost);
    });
  },

  async deleteByPrincipal(aPrincipal) {
    Services.perms.removeFromPrincipal(
      aPrincipal,
      SHUTDOWN_EXCEPTION_PERMISSION
    );
  },

  async deleteBySite(aSchemelessSite, aOriginAttributesPattern) {
    if (!lazy.permissionManagerIsolateByPrivateBrowsing) {
      delete aOriginAttributesPattern.privateBrowsingId;
    }
    if (!lazy.permissionManagerIsolateByUserContext) {
      delete aOriginAttributesPattern.userContextId;
    }

    this._deleteInternal(({ principal }) =>
      hasSite({ principal }, aSchemelessSite, aOriginAttributesPattern)
    );
  },

  async deleteByRange(aFrom) {
    Services.perms.removeByTypeSince(
      SHUTDOWN_EXCEPTION_PERMISSION,
      aFrom / 1000
    );
  },

  async deleteByOriginAttributes(aOriginAttributesString) {
    Services.perms.removePermissionsWithAttributes(
      aOriginAttributesString,
      [SHUTDOWN_EXCEPTION_PERMISSION],
      []
    );
  },

  async deleteAll() {
    Services.perms.removeByType(SHUTDOWN_EXCEPTION_PERMISSION);
  },
};

const PermissionsCleaner = {
  async _deleteInternal(filter) {
    Services.perms.all
      .filter(({ type }) => type != SHUTDOWN_EXCEPTION_PERMISSION)
      .filter(filter)
      .forEach(perm => {
        try {
          Services.perms.removePermission(perm);
        } catch (ex) {
          console.error(ex);
        }
      });
    await Services.perms.removeOrphanedInteractionRecords();
  },

  _thirdPartyStoragePermissionMatchesHost(permissionType, aHost) {
    if (
      !permissionType.startsWith("3rdPartyStorage^") &&
      !permissionType.startsWith("3rdPartyFrameStorage^")
    ) {
      return false;
    }
    let [, site] = permissionType.split("^");
    let uri;
    try {
      uri = Services.io.newURI(site);
    } catch (ex) {
      return false;
    }
    return Services.eTLD.hasRootDomain(uri.host, aHost);
  },

  _getPrincipalHost(principal) {
    try {
      return principal.host;
    } catch (e) {
      return null;
    }
  },

  async deleteByHost(aHost) {
    await this._deleteInternal(({ principal, type }) => {
      let principalHost = this._getPrincipalHost(principal);
      if (!principalHost?.length) {
        return false;
      }
      if (Services.eTLD.hasRootDomain(principalHost, aHost)) {
        return true;
      }

      return this._thirdPartyStoragePermissionMatchesHost(type, aHost);
    });
  },

  async deleteByPrincipal(aPrincipal) {
    await this._deleteInternal(({ principal, type }) => {
      if (principal.equals(aPrincipal)) {
        return true;
      }
      let principalHost = this._getPrincipalHost(aPrincipal);
      if (!principalHost?.length) {
        return false;
      }
      return this._thirdPartyStoragePermissionMatchesHost(type, principalHost);
    });
  },

  async deleteBySite(aSchemelessSite, aOriginAttributesPattern) {
    if (!lazy.permissionManagerIsolateByPrivateBrowsing) {
      delete aOriginAttributesPattern.privateBrowsingId;
    }
    if (!lazy.permissionManagerIsolateByUserContext) {
      delete aOriginAttributesPattern.userContextId;
    }

    await this._deleteInternal(
      ({ principal, type }) =>
        hasSite({ principal }, aSchemelessSite, aOriginAttributesPattern) ||
        this._thirdPartyStoragePermissionMatchesHost(type, aSchemelessSite)
    );
  },

  async deleteByRange(aFrom) {
    Services.perms.removeAllSinceWithTypeExceptions(aFrom / 1000, [
      SHUTDOWN_EXCEPTION_PERMISSION,
    ]);
  },

  async deleteByOriginAttributes(aOriginAttributesString) {
    Services.perms.removePermissionsWithAttributes(
      aOriginAttributesString,
      [],
      [SHUTDOWN_EXCEPTION_PERMISSION]
    );
  },

  async deleteAll() {
    Services.perms.removeAllExceptTypes([SHUTDOWN_EXCEPTION_PERMISSION]);
  },
};

const PreferencesCleaner = {
  deleteByHost(aHost, aOriginAttributes = {}) {
    aOriginAttributes =
      ChromeUtils.fillNonDefaultOriginAttributes(aOriginAttributes);

    let loadContext;
    if (
      aOriginAttributes.privateBrowsingId ==
      Services.scriptSecurityManager.DEFAULT_PRIVATE_BROWSING_ID
    ) {
      loadContext = Cu.createLoadContext();
    } else {
      loadContext = Cu.createPrivateLoadContext();
    }

    return new Promise((aResolve, aReject) => {
      let cps2 = Cc["@mozilla.org/content-pref/service;1"].getService(
        Ci.nsIContentPrefService2
      );
      cps2.removeBySubdomain(aHost, loadContext, {
        handleCompletion: aReason => {
          if (aReason === cps2.COMPLETE_ERROR) {
            aReject();
          } else {
            aResolve();
          }
        },
      });
    });
  },

  deleteByPrincipal(aPrincipal) {
    return this.deleteByHost(aPrincipal.host, aPrincipal.originAttributes);
  },

  async deleteBySite(aSchemelessSite, aOriginAttributesPattern) {
    let loadContext = null;

    if (aOriginAttributesPattern.privateBrowsingId != null) {
      let isPrivateBrowsing =
        aOriginAttributesPattern.privateBrowsingId !=
        Ci.nsIScriptSecurityManager.DEFAULT_PRIVATE_BROWSING_ID;
      loadContext = isPrivateBrowsing
        ? Cu.createPrivateLoadContext()
        : Cu.createLoadContext();
    }

    let cps2 = Cc["@mozilla.org/content-pref/service;1"].getService(
      Ci.nsIContentPrefService2
    );

    await new Promise((aResolve, aReject) => {
      cps2.removeBySubdomain(aSchemelessSite, loadContext, {
        handleCompletion: aReason => {
          if (aReason === cps2.COMPLETE_ERROR) {
            aReject();
          } else {
            aResolve();
          }
        },
      });
    });
  },

  async deleteByRange(aFrom) {
    let cps2 = Cc["@mozilla.org/content-pref/service;1"].getService(
      Ci.nsIContentPrefService2
    );

    await new Promise((aResolve, aReject) => {
      cps2.removeAllDomainsSince(aFrom / 1000, null, {
        handleCompletion: aReason => {
          if (aReason === cps2.COMPLETE_ERROR) {
            aReject();
          } else {
            aResolve();
          }
        },
      });
    });
  },

  async deleteAll() {
    let cps2 = Cc["@mozilla.org/content-pref/service;1"].getService(
      Ci.nsIContentPrefService2
    );

    await new Promise((aResolve, aReject) => {
      cps2.removeAllDomains(null, {
        handleCompletion: aReason => {
          if (aReason === cps2.COMPLETE_ERROR) {
            aReject();
          } else {
            aResolve();
          }
        },
      });
    });
  },
};

const TlsTokenCacheCleaner = {
  async deleteByHost(aHost, aOriginAttributes) {
    let pattern = {};
    if (aOriginAttributes.partitionKey) {
      pattern.partitionKey = aOriginAttributes.partitionKey;
    }
    lazy.nssComponent.removeSSLTokensByHostAndOriginAttributesPattern(
      aHost,
      JSON.stringify(pattern)
    );
  },

  async deleteByPrincipal(aPrincipal) {
    return this.deleteByHost(aPrincipal.host, aPrincipal.originAttributes);
  },

  async deleteBySite(aSchemelessSite, aOriginAttributesPattern) {
    lazy.nssComponent.removeSSLTokensBySiteAndOriginAttributesPattern(
      aSchemelessSite,
      JSON.stringify(aOriginAttributesPattern)
    );
  },

  async deleteAll() {
    lazy.nssComponent.clearSSLExternalAndInternalSessionCache();
  },
};

const ClientAuthRememberCleaner = {
  async deleteByHost(aHost, aOriginAttributes) {
    let cars = Cc[
      "@mozilla.org/security/clientAuthRememberService;1"
    ].getService(Ci.nsIClientAuthRememberService);

    cars.deleteDecisionsByHost(aHost, aOriginAttributes);
  },

  deleteByPrincipal(aPrincipal) {
    return this.deleteByHost(aPrincipal.host, aPrincipal.originAttributes);
  },

  async deleteBySite(aSchemelessSite, aOriginAttributesPattern) {
    let cars = Cc[
      "@mozilla.org/security/clientAuthRememberService;1"
    ].getService(Ci.nsIClientAuthRememberService);

    cars
      .getDecisions()
      .filter(({ asciiHost, entryKey }) => {
        let originSuffixEncoded = entryKey.split(",")[2];
        let originAttributes;

        if (originSuffixEncoded) {
          try {
            let originSuffix = decodeURIComponent(originSuffixEncoded);
            originAttributes =
              ChromeUtils.CreateOriginAttributesFromOriginSuffix(originSuffix);
          } catch (e) {
            console.error(e);
          }
        }

        return hasSite(
          {
            host: asciiHost,
            originAttributes,
          },
          aSchemelessSite,
          aOriginAttributesPattern
        );
      })
      .forEach(({ entryKey }) => cars.forgetRememberedDecision(entryKey));
  },

  async deleteAll() {
    let cars = Cc[
      "@mozilla.org/security/clientAuthRememberService;1"
    ].getService(Ci.nsIClientAuthRememberService);
    cars.clearRememberedDecisions();
  },
};

const HSTSCleaner = {
  async deleteByHost(aHost, aOriginAttributes) {
    let sss = Cc["@mozilla.org/ssservice;1"].getService(
      Ci.nsISiteSecurityService
    );
    let uri = Services.io.newURI("https://" + aHost);
    sss.resetState(
      uri,
      aOriginAttributes,
      Ci.nsISiteSecurityService.RootDomain
    );
  },

  deleteByPrincipal(aPrincipal) {
    return this.deleteByHost(aPrincipal.host, aPrincipal.originAttributes);
  },

  async deleteBySite(aSchemelessSite, _aOriginAttributesPattern) {
    let sss = Cc["@mozilla.org/ssservice;1"].getService(
      Ci.nsISiteSecurityService
    );

    let uri = Services.io.newURI("https://" + aSchemelessSite);
    sss.resetState(uri, {}, Ci.nsISiteSecurityService.BaseDomain);
  },

  async deleteAll() {
    let sss = Cc["@mozilla.org/ssservice;1"].getService(
      Ci.nsISiteSecurityService
    );
    sss.clearAll();
  },
};

const ContentBlockingCleaner = {
  deleteAll() {
    return lazy.TrackingDBService.clearAll();
  },

  async deleteByPrincipal(aPrincipal, aIsUserRequest) {
    if (!aIsUserRequest) {
      return;
    }
    await this.deleteAll();
  },

  async deleteBySite(
    _aSchemelessSite,
    _aOriginAttributesPattern,
    aIsUserRequest
  ) {
    if (!aIsUserRequest) {
      return;
    }
    await this.deleteAll();
  },

  deleteByRange(aFrom) {
    return lazy.TrackingDBService.clearSince(aFrom);
  },
};

const PreflightCacheCleaner = {
  async deleteByPrincipal(aPrincipal, aIsUserRequest) {
    if (!aIsUserRequest) {
      return;
    }
    await this.deleteAll();
  },

  async deleteBySite(
    _aSchemelessSite,
    _aOriginAttributesPattern,
    aIsUserRequest
  ) {
    if (!aIsUserRequest) {
      return;
    }
    await this.deleteAll();
  },

  async deleteAll() {
    Cc[`@mozilla.org/network/protocol;1?name=http`]
      .getService(Ci.nsIHttpProtocolHandler)
      .clearCORSPreflightCache();
  },
};

const BounceTrackingProtectionStateCleaner = {
  async deleteAll() {
    if (
      lazy.bounceTrackingProtectionMode ==
      Ci.nsIBounceTrackingProtection.MODE_DISABLED
    ) {
      return;
    }
    lazy.bounceTrackingProtection.clearAll();
  },

  async deleteByPrincipal(aPrincipal) {
    if (
      lazy.bounceTrackingProtectionMode ==
      Ci.nsIBounceTrackingProtection.MODE_DISABLED
    ) {
      return;
    }
    let { baseDomain, originAttributes } = aPrincipal;
    lazy.bounceTrackingProtection.clearBySiteHostAndOriginAttributes(
      baseDomain,
      originAttributes
    );
  },

  async deleteBySite(aSchemelessSite, aOriginAttributesPattern) {
    if (
      lazy.bounceTrackingProtectionMode ==
      Ci.nsIBounceTrackingProtection.MODE_DISABLED
    ) {
      return;
    }
    lazy.bounceTrackingProtection.clearBySiteHostAndOriginAttributesPattern(
      aSchemelessSite,
      aOriginAttributesPattern
    );
  },

  async deleteByRange(aFrom, aTo) {
    if (
      lazy.bounceTrackingProtectionMode ==
      Ci.nsIBounceTrackingProtection.MODE_DISABLED
    ) {
      return;
    }
    lazy.bounceTrackingProtection.clearByTimeRange(aFrom, aTo);
  },

  async deleteByHost(aHost, aOriginAttributesPattern = {}) {
    if (
      lazy.bounceTrackingProtectionMode ==
      Ci.nsIBounceTrackingProtection.MODE_DISABLED
    ) {
      return;
    }
    let baseDomain = Services.eTLD.getSchemelessSiteFromHost(aHost);
    lazy.bounceTrackingProtection.clearBySiteHostAndOriginAttributesPattern(
      baseDomain,
      aOriginAttributesPattern
    );
  },

  async deleteByOriginAttributes(aOriginAttributesPatternString) {
    if (
      lazy.bounceTrackingProtectionMode ==
      Ci.nsIBounceTrackingProtection.MODE_DISABLED
    ) {
      return;
    }
    lazy.bounceTrackingProtection.clearByOriginAttributesPattern(
      aOriginAttributesPatternString
    );
  },
};

const StoragePermissionsCleaner = {
  async deleteByRange(aFrom) {
    Services.perms.removeByTypeSince("storage-access", aFrom / 1000);

    const persistentStoragePermissions = Services.perms.getAllByTypeSince(
      "persistent-storage",
      aFrom / 1000
    );
    persistentStoragePermissions.forEach(perm =>
      Services.perms.removePermission(perm)
    );
    await Services.perms.removeOrphanedInteractionRecords();
  },

  async deleteByPrincipal(aPrincipal) {
    Services.perms.removeFromPrincipal(aPrincipal, "storage-access");

    Services.perms.removeFromPrincipal(aPrincipal, "persistent-storage");
  },

  async deleteByHost(aHost) {
    let permissions = this._getStoragePermissions();
    for (let perm of permissions) {
      if (Services.eTLD.hasRootDomain(perm.principal.host, aHost)) {
        Services.perms.removePermission(perm);
      }
    }
    await Services.perms.removeOrphanedInteractionRecords();
  },

  async deleteBySite(aSchemelessSite, aOriginAttributesPattern) {
    if (!lazy.permissionManagerIsolateByPrivateBrowsing) {
      delete aOriginAttributesPattern.privateBrowsingId;
    }
    if (!lazy.permissionManagerIsolateByUserContext) {
      delete aOriginAttributesPattern.userContextId;
    }

    let permissions = this._getStoragePermissions();
    for (let perm of permissions) {
      let { principal } = perm;
      if (hasSite({ principal }, aSchemelessSite, aOriginAttributesPattern)) {
        Services.perms.removePermission(perm);
      }
    }
    await Services.perms.removeOrphanedInteractionRecords();
  },

  async deleteByLocalFiles() {
    let permissions = this._getStoragePermissions();
    for (let perm of permissions) {
      if (perm.principal.schemeIs("file")) {
        Services.perms.removePermission(perm);
      }
    }
    await Services.perms.removeOrphanedInteractionRecords();
  },

  async deleteAll() {
    Services.perms.removeByType("storage-access");

    Services.perms.removeByType("persistent-storage");
    await Services.perms.removeOrphanedInteractionRecords();
  },

  _getStoragePermissions() {
    let storagePermissions = Services.perms.getAllByTypes([
      "storage-access",
      "persistent-storage",
    ]);

    return storagePermissions;
  },
};

const BfcacheCleaner = {
  async deleteAll() {
  },

  async deleteBySite(_aSchemelessSite, _aOriginAttributesPattern) {
  },

  async deleteByPrincipal(aPrincipal) {
    ChromeUtils.clearBfcacheByPrincipal(aPrincipal);
  },
};

const FLAGS_MAP = [
  {
    flag: Ci.nsIClearDataService.CLEAR_CERT_EXCEPTIONS,
    cleaners: [CertCleaner],
  },

  { flag: Ci.nsIClearDataService.CLEAR_COOKIES, cleaners: [CookieCleaner] },

  {
    flag: Ci.nsIClearDataService.CLEAR_NETWORK_CACHE,
    cleaners: [NetworkCacheCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_IMAGE_CACHE,
    cleaners: [ImageCacheCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_CSS_CACHE,
    cleaners: [CSSCacheCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_JS_CACHE,
    cleaners: [JSCacheCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_CLIENT_AUTH_REMEMBER_SERVICE,
    cleaners: [ClientAuthRememberCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_TLS_TOKEN_CACHE,
    cleaners: [TlsTokenCacheCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_DOWNLOADS,
    cleaners: [DownloadsCleaner],
  },

  { flag: Ci.nsIClearDataService.CLEAR_DOM_QUOTA, cleaners: [QuotaCleaner] },

  {
    flag: Ci.nsIClearDataService.CLEAR_HISTORY,
    cleaners: [
      HistoryCleaner,
      SessionHistoryCleaner,
    ],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_AUTH_CACHE,
    cleaners: [AuthCacheCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_SITE_PERMISSIONS,
    cleaners: [PermissionsCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_CONTENT_PREFERENCES,
    cleaners: [PreferencesCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_HSTS,
    cleaners: [HSTSCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_STORAGE_ACCESS,
    cleaners: [StorageAccessCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_CONTENT_BLOCKING_RECORDS,
    cleaners: [ContentBlockingCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_PREFLIGHT_CACHE,
    cleaners: [PreflightCacheCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_FINGERPRINTING_PROTECTION_STATE,
    cleaners: [FingerprintingProtectionStateCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_BOUNCE_TRACKING_PROTECTION_STATE,
    cleaners: [BounceTrackingProtectionStateCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_STORAGE_PERMISSIONS,
    cleaners: [StoragePermissionsCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_SHUTDOWN_EXCEPTIONS,
    cleaners: [ShutdownExceptionsCleaner],
  },

  {
    flag: Ci.nsIClearDataService.CLEAR_BFCACHE,
    cleaners: [BfcacheCleaner],
  },
];

function flagToName(aFlag) {
  return (
    Object.entries(Ci.nsIClearDataService).find(
      ([key, value]) => key.startsWith("CLEAR_") && value === aFlag
    )?.[0] ?? `0x${aFlag.toString(16)}`
  );
}

export function ClearDataService() {
  this._initialize();
}

ClearDataService.prototype = Object.freeze({
  classID: Components.ID("{0c06583d-7dd8-4293-b1a5-912205f779aa}"),
  QueryInterface: ChromeUtils.generateQI(["nsIClearDataService"]),

  _initialize() {

    if (!Services.qms) {
      console.error("Failed initializiation of QuotaManagerService.");
    }
  },

  deleteDataFromLocalFiles(aIsUserRequest, aFlags, aCallback) {
    if (!aCallback) {
      return Cr.NS_ERROR_INVALID_ARG;
    }

    return this._deleteInternal(aFlags, aCallback, aCleaner => {
      if (aCleaner.deleteByLocalFiles) {
        return aCleaner.deleteByLocalFiles({});
      }
      return Promise.resolve();
    });
  },

  deleteDataFromHost(aHost, aIsUserRequest, aFlags, aCallback) {
    if (!aHost || !aCallback) {
      return Cr.NS_ERROR_INVALID_ARG;
    }

    return this._deleteInternal(aFlags, aCallback, aCleaner => {
      if (aCleaner.deleteByHost) {
        return aCleaner.deleteByHost(aHost, {});
      }
      if (aIsUserRequest) {
        return aCleaner.deleteAll();
      }
      return Promise.resolve();
    });
  },

  deleteDataFromSite(
    aSchemelessSite,
    aOriginAttributesPattern,
    aIsUserRequest,
    aFlags,
    aCallback
  ) {
    if (!aSchemelessSite?.length || !aCallback) {
      return Cr.NS_ERROR_INVALID_ARG;
    }

    if (AppConstants.DEBUG) {
      let schemelessSiteComputed =
        Services.eTLD.getSchemelessSiteFromHost(aSchemelessSite);
      if (schemelessSiteComputed != aSchemelessSite) {
        throw new Error(
          `deleteDataFromSite called with invalid aSchemelessSite '${aSchemelessSite}'. Expected site is '${schemelessSiteComputed}'`
        );
      }
    }

    return this._deleteInternal(aFlags, aCallback, aCleaner =>
      aCleaner.deleteBySite(
        aSchemelessSite,
        aOriginAttributesPattern,
        aIsUserRequest
      )
    );
  },

  deleteDataFromSiteAndOriginAttributesPatternString(
    aSchemelessSite,
    aOriginAttributesPatternString,
    aIsUserRequest,
    aFlags,
    aCallback
  ) {
    if (!aSchemelessSite || !aCallback) {
      return Cr.NS_ERROR_INVALID_ARG;
    }

    let originAttributesPattern = {};
    if (aOriginAttributesPatternString?.length) {
      originAttributesPattern = JSON.parse(aOriginAttributesPatternString);
    }

    return this.deleteDataFromSite(
      aSchemelessSite,
      originAttributesPattern,
      aIsUserRequest,
      aFlags,
      aCallback
    );
  },

  deleteDataFromPrincipal(aPrincipal, aIsUserRequest, aFlags, aCallback) {
    if (!aPrincipal || !aCallback) {
      return Cr.NS_ERROR_INVALID_ARG;
    }

    return this._deleteInternal(aFlags, aCallback, aCleaner =>
      aCleaner.deleteByPrincipal(aPrincipal, aIsUserRequest)
    );
  },

  deleteDataInTimeRange(aFrom, aTo, aIsUserRequest, aFlags, aCallback) {
    if (aFrom > aTo || !aCallback) {
      return Cr.NS_ERROR_INVALID_ARG;
    }

    return this._deleteInternal(aFlags, aCallback, aCleaner => {
      if (aCleaner.deleteByRange) {
        return aCleaner.deleteByRange(aFrom, aTo);
      }
      if (aIsUserRequest) {
        return aCleaner.deleteAll();
      }
      return Promise.resolve();
    });
  },

  deleteData(aFlags, aCallback) {
    if (!aCallback) {
      return Cr.NS_ERROR_INVALID_ARG;
    }

    return this._deleteInternal(aFlags, aCallback, aCleaner => {
      return aCleaner.deleteAll();
    });
  },

  deleteDataFromOriginAttributesPattern(aPattern, aCallback) {
    if (!aPattern) {
      return Cr.NS_ERROR_INVALID_ARG;
    }

    let patternString = JSON.stringify(aPattern);
    Services.obs.notifyObservers(
      null,
      "clear-origin-attributes-data",
      patternString
    );

    if (!aCallback) {
      aCallback = {
        onDataDeleted: () => {},
      };
    }
    return this._deleteInternal(
      Ci.nsIClearDataService.CLEAR_ALL,
      aCallback,
      aCleaner => {
        if (aCleaner.deleteByOriginAttributes) {
          return aCleaner.deleteByOriginAttributes(patternString);
        }

        return Promise.resolve();
      }
    );
  },

  deleteUserInteractionForClearingHistory(
    aPrincipalsWithStorage,
    aFrom,
    aCallback
  ) {
    if (!aCallback) {
      return Cr.NS_ERROR_INVALID_ARG;
    }

    StorageAccessCleaner.deleteExceptPrincipals(aPrincipalsWithStorage, aFrom)
      .then(() => {
        aCallback.onDataDeleted(0);
      })
      .catch(() => {
        aCallback.onDataDeleted(Ci.nsIClearDataService.CLEAR_PERMISSIONS);
      });
    return Cr.NS_OK;
  },

  cleanupAfterDeletionAtShutdown(aFlags, aCallback) {
    return this._deleteInternal(aFlags, aCallback, async aCleaner => {
      if (aCleaner.cleanupAfterDeletionAtShutdown) {
        await aCleaner.cleanupAfterDeletionAtShutdown();
      }
    });
  },

  clearPrivateBrowsingData(aCallback) {
    if (gPBMCleanupInProgress) {
      throw Components.Exception(
        "PBM cleanup already in progress",
        Cr.NS_ERROR_ABORT
      );
    }

    if (!aCallback) {
      aCallback = {
        onDataDeleted() {},
      };
    }

    gPBMCleanupInProgress = true;

    let collector = new PBMCleanupCollector();

    Services.obs.notifyObservers(collector, "last-pb-context-exited");

    collector.promise
      .then(hadFailures => {
        gPBMCleanupInProgress = false;
        if (hadFailures) {
          console.error("PBM cleanup: one or more observers reported failure");
        }
        aCallback.onDataDeleted(hadFailures ? 1 : 0);
      })
      .catch(e => {
        gPBMCleanupInProgress = false;
        console.error("PBM cleanup error:", e);
        aCallback.onDataDeleted(1);
      });

    return Cr.NS_OK;
  },

  hostMatchesSite(
    aHost,
    aOriginAttributes,
    aSchemelessSite,
    aOriginAttributesPattern = {}
  ) {
    return hasSite(
      { host: aHost, originAttributes: aOriginAttributes },
      aSchemelessSite,
      aOriginAttributesPattern
    );
  },

  _deleteInternal(aFlags, aCallback, aHelper) {
    let resultFlags = 0;
    let promises = FLAGS_MAP.filter(c => aFlags & c.flag).map(c => {
      return Promise.all(
        c.cleaners.map(cleaner => {
          return aHelper(cleaner).catch(e => {
            console.error(
              `ClearDataService failed to clear ${flagToName(c.flag)}:`,
              e
            );
            resultFlags |= c.flag;
          });
        })
      );
    });
    Promise.all(promises).then(() => {
      aCallback.onDataDeleted(resultFlags);
    });
    return Cr.NS_OK;
  },
});
