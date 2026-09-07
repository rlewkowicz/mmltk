/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ContextualIdentityService:
    "moz-src:///toolkit/components/contextualidentity/ContextualIdentityService.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  PrincipalsCollector: "resource://gre/modules/PrincipalsCollector.sys.mjs",
});

var logConsole;
function log(...msgs) {
  if (!logConsole) {
    logConsole = console.createInstance({
      prefix: "Sanitizer",
      maxLogLevelPref: "browser.sanitizer.loglevel",
    });
  }

  logConsole.log(...msgs);
}

var gPendingSanitizationSerial = 0;

var gPrincipalsCollector = null;

export var Sanitizer = {
  PREF_SANITIZE_ON_SHUTDOWN: "privacy.sanitize.sanitizeOnShutdown",

  PREF_PENDING_SANITIZATIONS: "privacy.sanitize.pending",

  PREF_CPD_BRANCH: "privacy.cpd.",
  PREF_SHUTDOWN_BRANCH: "privacy.clearOnShutdown_v2.",

  PREF_TIMESPAN: "privacy.sanitize.timeSpan",

  TIMESPAN_EVERYTHING: 0,
  TIMESPAN_HOUR: 1,
  TIMESPAN_2HOURS: 2,
  TIMESPAN_4HOURS: 3,
  TIMESPAN_TODAY: 4,
  TIMESPAN_5MIN: 5,
  TIMESPAN_24HOURS: 6,

  timeSpanMsMap: {
    TIMESPAN_5MIN: 300000, 
    TIMESPAN_HOUR: 3600000, 
    TIMESPAN_2HOURS: 7200000, 
    TIMESPAN_4HOURS: 14400000, 
    TIMESPAN_24HOURS: 86400000, 
    get TIMESPAN_TODAY() {
      return Date.now() - new Date().setHours(0, 0, 0, 0);
    }, 
  },

  shouldSanitizeOnShutdown: false,

  async showUI(parentWindow, mode) {
    if (
      parentWindow?.document.documentURI ==
      "chrome://browser/content/hiddenWindowMac.xhtml"
    ) {
      parentWindow = null;
    }

    let dialogFile = "sanitize_v2.xhtml";
    let deferred = Promise.withResolvers();

    if (parentWindow?.gDialogBox) {
      parentWindow.gDialogBox.open(`chrome://browser/content/${dialogFile}`, {
        inBrowserWindow: true,
        mode,
        onAccept: () => deferred.resolve("accept"),
        onCancel: () => deferred.resolve("cancel"),
      });
    } else {
      Services.ww.openWindow(
        parentWindow,
        `chrome://browser/content/${dialogFile}`,
        "Sanitize",
        "chrome,titlebar,dialog,centerscreen,modal",
        {
          needNativeUI: true,
          mode,
          onAccept: () => deferred.resolve("accept"),
          onCancel: () => deferred.resolve("cancel"),
        }
      );
    }

    return deferred.promise;
  },

  async onStartup() {
    let pendingSanitizations = getAndClearPendingSanitizations();
    log("Pending sanitizations:", pendingSanitizations);

    this.shouldSanitizeOnShutdown = Services.prefs.getBoolPref(
      Sanitizer.PREF_SANITIZE_ON_SHUTDOWN,
      false
    );
    Services.prefs.addObserver(Sanitizer.PREF_SANITIZE_ON_SHUTDOWN, this, true);
    if (this.shouldSanitizeOnShutdown) {
      let itemsToClear = getItemsToClearFromPrefBranch(
        Sanitizer.PREF_SHUTDOWN_BRANCH
      );
      addPendingSanitization("shutdown", itemsToClear, {});
    }
    Services.prefs.addObserver(Sanitizer.PREF_SHUTDOWN_BRANCH, this, true);

    if (AppConstants.MOZ_PLACES) {
      let shutdownClient = lazy.PlacesUtils.history.shutdownClient.jsclient;
      let progress = { isShutdown: true, clearHonoringExceptions: true };
      shutdownClient.addBlocker(
        "sanitize.js: Sanitize on shutdown",
        () => sanitizeOnShutdown(progress),
        { fetchState: () => ({ progress }) }
      );
    }


    for (let { itemsToClear, options } of pendingSanitizations) {
      try {
        options.progress = { clearHonoringExceptions: true };
        await this.sanitize(itemsToClear, options);
      } catch (ex) {
        console.error(
          "A previously pending sanitization failed: ",
          itemsToClear,
          ex
        );
      }
    }
    await cleanupAfterSanitization(Ci.nsIClearDataService.CLEAR_ALL);
  },

  getClearRange(ts) {
    if (ts === undefined) {
      ts = Services.prefs.getIntPref(Sanitizer.PREF_TIMESPAN);
    }
    if (ts === Sanitizer.TIMESPAN_EVERYTHING) {
      return null;
    }

    var endDate = Date.now() * 1000;
    switch (ts) {
      case Sanitizer.TIMESPAN_5MIN:
        var startDate = endDate - 300000000; 
        break;
      case Sanitizer.TIMESPAN_HOUR:
        startDate = endDate - 3600000000; 
        break;
      case Sanitizer.TIMESPAN_2HOURS:
        startDate = endDate - 7200000000; 
        break;
      case Sanitizer.TIMESPAN_4HOURS:
        startDate = endDate - 14400000000; 
        break;
      case Sanitizer.TIMESPAN_TODAY:
        var d = new Date(); 
        d.setHours(0); 
        d.setMinutes(0);
        d.setSeconds(0);
        d.setMilliseconds(0);
        startDate = d.valueOf() * 1000; 
        break;
      case Sanitizer.TIMESPAN_24HOURS:
        startDate = endDate - 86400000000; 
        break;
      default:
        throw new Error("Invalid time span for clear private data: " + ts);
    }
    return [startDate, endDate];
  },

  async sanitize(itemsToClear = null, options = {}) {
    let progress = options.progress;
    gPrincipalsCollector = new lazy.PrincipalsCollector();
    if (!progress) {
      progress = options.progress = {};
    }

    if (!itemsToClear) {
      itemsToClear = getItemsToClearFromPrefBranch(this.PREF_CPD_BRANCH);
    }
    let promise = sanitizeInternal(this.items, itemsToClear, options);

    if (!progress.isShutdown && AppConstants.MOZ_PLACES) {
      let shutdownClient = lazy.PlacesUtils.history.shutdownClient.jsclient;
      shutdownClient.addBlocker("sanitize.js: Sanitize", promise, {
        fetchState: () => ({ progress }),
      });
    }

    try {
      await promise;
    } finally {
      Services.obs.notifyObservers(null, "sanitizer-sanitization-complete");
    }
    return progress;
  },

  observe(subject, topic, data) {
    if (topic == "nsPref:changed") {
      if (
        data.startsWith(this.PREF_SHUTDOWN_BRANCH) &&
        this.shouldSanitizeOnShutdown
      ) {
        removePendingSanitization("shutdown");
        let itemsToClear = getItemsToClearFromPrefBranch(
          Sanitizer.PREF_SHUTDOWN_BRANCH
        );
        addPendingSanitization("shutdown", itemsToClear, {});
      } else if (data == this.PREF_SANITIZE_ON_SHUTDOWN) {
        this.shouldSanitizeOnShutdown = Services.prefs.getBoolPref(
          Sanitizer.PREF_SANITIZE_ON_SHUTDOWN,
          false
        );
        removePendingSanitization("shutdown");
        if (this.shouldSanitizeOnShutdown) {
          let itemsToClear = getItemsToClearFromPrefBranch(
            Sanitizer.PREF_SHUTDOWN_BRANCH
          );
          addPendingSanitization("shutdown", itemsToClear, {});
        }
      }
    }
  },

  QueryInterface: ChromeUtils.generateQI([
    "nsIObserver",
    "nsISupportsWeakReference",
  ]),

  async runSanitizeOnShutdown() {
    gPrincipalsCollector = null;
    return sanitizeOnShutdown({
      isShutdown: true,
      clearHonoringExceptions: true,
    });
  },

  maybeMigratePrefs(context) {
    if (
      Services.prefs.getBoolPref(
        `privacy.sanitize.${context}.hasMigratedToNewPrefs3`
      )
    ) {
      return;
    }

    let newContext =
      context == "clearOnShutdown" ? "clearOnShutdown_v2" : "clearHistory";

    let history = Services.prefs.getBoolPref(`privacy.${context}.history`);
    if (
      Services.prefs.getBoolPref(
        `privacy.sanitize.${context}.hasMigratedToNewPrefs2`
      )
    ) {
      history = Services.prefs.getBoolPref(
        `privacy.${context == "cpd" ? "clearHistory" : "clearOnShutdown_v2"}.historyFormDataAndDownloads`
      );
    } else {

      let cookies = Services.prefs.getBoolPref(`privacy.${context}.cookies`);
      let cache = Services.prefs.getBoolPref(`privacy.${context}.cache`);
      let siteSettings = Services.prefs.getBoolPref(
        `privacy.${context}.siteSettings`
      );

      Services.prefs.setBoolPref(
        `privacy.${newContext}.cookiesAndStorage`,
        cookies
      );

      Services.prefs.setBoolPref(`privacy.${newContext}.cache`, cache);

      Services.prefs.setBoolPref(
        `privacy.${newContext}.siteSettings`,
        siteSettings
      );
    }

    Services.prefs.setBoolPref(
      `privacy.${newContext}.browsingHistoryAndDownloads`,
      history
    );

    Services.prefs.clearUserPref(
      `privacy.sanitize.${context}.hasMigratedToNewPrefs`
    );
    Services.prefs.clearUserPref(
      `privacy.sanitize.${context}.hasMigratedToNewPrefs2`
    );

    Services.prefs.setBoolPref(
      `privacy.sanitize.${context}.hasMigratedToNewPrefs3`,
      true
    );
  },


  items: {
    cache: {
      async clear(range) {
        await clearData(range, Ci.nsIClearDataService.CLEAR_ALL_CACHES);
      },
    },

    cookies: {
      async clear(range, { progress }, clearHonoringExceptions) {
        if (clearHonoringExceptions) {
          progress.step = "getAllPrincipals";
          let principalsForShutdownClearing =
            await gPrincipalsCollector.getAllPrincipals(progress);
          await maybeSanitizeSessionPrincipals(
            progress,
            principalsForShutdownClearing,
            Ci.nsIClearDataService.CLEAR_COOKIES |
              Ci.nsIClearDataService.CLEAR_FINGERPRINTING_PROTECTION_STATE |
              Ci.nsIClearDataService.CLEAR_BOUNCE_TRACKING_PROTECTION_STATE
          );
        } else {
          await clearData(
            range,
            Ci.nsIClearDataService.CLEAR_COOKIES |
              Ci.nsIClearDataService.CLEAR_FINGERPRINTING_PROTECTION_STATE |
              Ci.nsIClearDataService.CLEAR_BOUNCE_TRACKING_PROTECTION_STATE
          );
        }
      },
    },

    offlineApps: {
      async clear(range, { progress }, clearHonoringExceptions) {
        if (clearHonoringExceptions) {
          progress.step = "getAllPrincipals";
          let principalsForShutdownClearing =
            await gPrincipalsCollector.getAllPrincipals(progress);
          await maybeSanitizeSessionPrincipals(
            progress,
            principalsForShutdownClearing,
            Ci.nsIClearDataService.CLEAR_DOM_STORAGES |
              Ci.nsIClearDataService.CLEAR_FINGERPRINTING_PROTECTION_STATE
          );
        } else {
          await clearData(
            range,
            Ci.nsIClearDataService.CLEAR_DOM_STORAGES |
              Ci.nsIClearDataService.CLEAR_FINGERPRINTING_PROTECTION_STATE
          );
        }
      },
    },

    history: {
      async clear(range, { progress }) {
        if (!gPrincipalsCollector) {
          gPrincipalsCollector = new lazy.PrincipalsCollector();
        }
        progress.step = "getAllPrincipals";
        let principals = await gPrincipalsCollector.getAllPrincipals(progress);
        progress.step = "clearing browsing history";
        await clearData(
          range,
          Ci.nsIClearDataService.CLEAR_HISTORY |
            Ci.nsIClearDataService.CLEAR_CONTENT_BLOCKING_RECORDS
        );

        progress.step = "clearing user interaction";
        await new Promise(resolve => {
          Services.clearData.deleteUserInteractionForClearingHistory(
            principals,
            range ? range[0] : 0,
            resolve
          );
        });
      },
    },

    downloads: {
      async clear(range) {
        await clearData(range, Ci.nsIClearDataService.CLEAR_DOWNLOADS);
      },
    },

    sessions: {
      async clear(range) {
        await clearData(range, Ci.nsIClearDataService.CLEAR_AUTH_CACHE);
      },
    },

    siteSettings: {
      async clear(range, _options, clearHonoringExceptions) {
        // fall through to CLEAR_PERMISSIONS which clears that type too.
        let permissionsFlag = clearHonoringExceptions
          ? Ci.nsIClearDataService.CLEAR_SITE_PERMISSIONS
          : Ci.nsIClearDataService.CLEAR_PERMISSIONS;
        await clearData(
          range,
          permissionsFlag |
            Ci.nsIClearDataService.CLEAR_CONTENT_PREFERENCES |
            Ci.nsIClearDataService.CLEAR_CLIENT_AUTH_REMEMBER_SERVICE |
            Ci.nsIClearDataService.CLEAR_CERT_EXCEPTIONS |
            Ci.nsIClearDataService.CLEAR_FINGERPRINTING_PROTECTION_STATE
        );
      },
    },

    openWindows: {
      _canCloseWindow(win) {
        if (win.CanCloseWindow()) {
          win.skipNextCanClose = true;
          return true;
        }
        return false;
      },
      _resetAllWindowClosures(windowList) {
        for (let win of windowList) {
          win.skipNextCanClose = false;
        }
      },
      async clear(range, { privateStateForNewWindow = "non-private" }) {

        let startDate = Date.now();

        let windowList = [];
        for (let someWin of Services.wm.getEnumerator("navigator:browser")) {
          windowList.push(someWin);
          if (!this._canCloseWindow(someWin)) {
            this._resetAllWindowClosures(windowList);
            throw new Error(
              "Sanitize could not close windows: cancelled by user"
            );
          }

          if (Date.now() > startDate + 60 * 1000) {
            this._resetAllWindowClosures(windowList);
            throw new Error("Sanitize could not close windows: timeout");
          }
        }

        if (!windowList.length) {
          return;
        }


        let handler = Cc["@mozilla.org/browser/clh;1"].getService(
          Ci.nsIBrowserHandler
        );
        let defaultArgs = handler.defaultArgs;
        let features = "chrome,all,dialog=no," + privateStateForNewWindow;
        let newWindow = windowList[0].openDialog(
          AppConstants.BROWSER_CHROME_URL,
          "_blank",
          features,
          defaultArgs
        );

        let onFullScreen = null;
        if (AppConstants.platform == "macosx") {
          onFullScreen = function (e) {
            newWindow.removeEventListener("fullscreen", onFullScreen);
            let docEl = newWindow.document.documentElement;
            let sizemode = docEl.getAttribute("sizemode");
            if (!newWindow.fullScreen && sizemode == "fullscreen") {
              docEl.setAttribute("sizemode", "normal");
              e.preventDefault();
              e.stopPropagation();
              return false;
            }
            return undefined;
          };
          newWindow.addEventListener("fullscreen", onFullScreen);
        }

        let promiseReady = new Promise(resolve => {
          let newWindowOpened = false;
          let onWindowOpened = function (subject) {
            if (subject != newWindow) {
              return;
            }

            Services.obs.removeObserver(
              onWindowOpened,
              "browser-delayed-startup-finished"
            );
            if (AppConstants.platform == "macosx") {
              newWindow.removeEventListener("fullscreen", onFullScreen);
            }
            newWindowOpened = true;
            if (numWindowsClosing == 0) {
              resolve();
            }
          };

          let numWindowsClosing = windowList.length;
          let onWindowClosed = function () {
            numWindowsClosing--;
            if (numWindowsClosing == 0) {
              Services.obs.removeObserver(
                onWindowClosed,
                "xul-window-destroyed"
              );
              if (newWindowOpened) {
                resolve();
              }
            }
          };
          Services.obs.addObserver(
            onWindowOpened,
            "browser-delayed-startup-finished"
          );
          Services.obs.addObserver(onWindowClosed, "xul-window-destroyed");
        });

        while (windowList.length) {
          windowList.pop().close();
        }
        newWindow.focus();
        await promiseReady;
      },
    },

    pluginData: {
      async clear() {},
    },

    browsingHistoryAndDownloads: {
      async clear(range, { progress }) {
        progress.step = "getAllPrincipals";
        let principals = await gPrincipalsCollector.getAllPrincipals(progress);
        progress.step = "clearing browsing history";
        await clearData(
          range,
          Ci.nsIClearDataService.CLEAR_HISTORY |
            Ci.nsIClearDataService.CLEAR_CONTENT_BLOCKING_RECORDS
        );

        progress.step = "clearing user interaction";
        await new Promise(resolve => {
          Services.clearData.deleteUserInteractionForClearingHistory(
            principals,
            range ? range[0] : 0,
            resolve
          );
        });

        await clearData(range, Ci.nsIClearDataService.CLEAR_DOWNLOADS);

      },
    },

    cookiesAndStorage: {
      async clear(range, { progress }, clearHonoringExceptions) {
        if (clearHonoringExceptions) {
          progress.step = "getAllPrincipals";
          let principalsForShutdownClearing =
            await gPrincipalsCollector.getAllPrincipals(progress);
          await maybeSanitizeSessionPrincipals(
            progress,
            principalsForShutdownClearing,
            Ci.nsIClearDataService.CLEAR_COOKIES_AND_SITE_DATA
          );
        } else {
          await clearData(
            range,
            Ci.nsIClearDataService.CLEAR_COOKIES_AND_SITE_DATA
          );
        }
      },
    },
  },
};

async function sanitizeInternal(items, aItemsToClear, options) {
  let { ignoreTimespan = true, range, progress } = options;
  let seenError = false;
  if (!Array.isArray(aItemsToClear)) {
    throw new Error("Must pass an array of items to clear.");
  }
  let itemsToClear = [...aItemsToClear];

  let uid = gPendingSanitizationSerial++;
  if (!progress.isShutdown) {
    addPendingSanitization(uid, itemsToClear, options);
  }

  for (let k of itemsToClear) {
    progress[k] = "ready";
    progress[k + "Progress"] = {};
  }

  let openWindowsIndex = itemsToClear.indexOf("openWindows");
  if (openWindowsIndex != -1) {
    itemsToClear.splice(openWindowsIndex, 1);
    await items.openWindows.clear(
      null,
      Object.assign(options, { progress: progress.openWindowsProgress })
    );
    progress.openWindows = "cleared";
    delete progress.openWindowsProgress;
  }

  if (!ignoreTimespan && !range) {
    range = Sanitizer.getClearRange();
  }


  let annotateError = (name, ex) => {
    progress[name] = "failed";
    seenError = true;
    console.error("Error sanitizing " + name, ex);
  };

  log("Running sanitization for:", itemsToClear);
  let handles = [];
  for (let name of itemsToClear) {
    progress[name] = "blocking";
    let item = items[name];
    try {
      handles.push({
        name,
        promise: item
          .clear(
            range,
            Object.assign(options, { progress: progress[name + "Progress"] }),
            progress.clearHonoringExceptions
          )
          .then(
            () => {
              progress[name] = "cleared";
              delete progress[name + "Progress"];
            },
            ex => annotateError(name, ex)
          ),
      });
    } catch (ex) {
      annotateError(name, ex);
    }
  }
  await Promise.all(handles.map(h => h.promise));

  log("All sanitizations are complete");
  if (!progress.isShutdown) {
    removePendingSanitization(uid);
  }
  progress = {};
  if (seenError) {
    throw new Error("Error sanitizing");
  }
}

async function sanitizeOnShutdown(progress) {
  log("Sanitizing on shutdown");
  Sanitizer.maybeMigratePrefs("clearOnShutdown");

  progress.sanitizationPrefs = {
    privacy_sanitize_sanitizeOnShutdown: Services.prefs.getBoolPref(
      "privacy.sanitize.sanitizeOnShutdown"
    ),
    privacy_clearOnShutdown_v2_cookiesAndStorage: Services.prefs.getBoolPref(
      "privacy.clearOnShutdown_v2.cookiesAndStorage"
    ),
    privacy_clearOnShutdown_v2_browsingHistoryAndDownloads:
      Services.prefs.getBoolPref(
        "privacy.clearOnShutdown_v2.browsingHistoryAndDownloads"
      ),
    privacy_clearOnShutdown_v2_cache: Services.prefs.getBoolPref(
      "privacy.clearOnShutdown_v2.cache"
    ),
    privacy_clearOnShutdown_v2_siteSettings: Services.prefs.getBoolPref(
      "privacy.clearOnShutdown_v2.siteSettings"
    ),
  };

  let needsSyncSavePrefs = false;
  if (Sanitizer.shouldSanitizeOnShutdown) {
    progress.advancement = "shutdown-cleaner";

    let itemsToClear = getItemsToClearFromPrefBranch(
      Sanitizer.PREF_SHUTDOWN_BRANCH
    );
    await Sanitizer.sanitize(itemsToClear, { progress });

    removePendingSanitization("shutdown");
    needsSyncSavePrefs = true;
  }


  if (needsSyncSavePrefs) {
    Services.prefs.savePrefFile(null);
  }

  if (!Sanitizer.shouldSanitizeOnShutdown) {

    progress.advancement = "session-permission";

    let exceptions = 0;
    let selectedPrincipals = [];
    for (let permission of Services.perms.all) {
      if (
        permission.type != "cookie" ||
        permission.capability != Ci.nsICookiePermission.ACCESS_SESSION
      ) {
        continue;
      }

      if (!isSupportedPrincipal(permission.principal)) {
        continue;
      }

      log(
        "Custom session cookie permission detected for: " +
          permission.principal.asciiSpec
      );
      exceptions++;

      if (!gPrincipalsCollector) {
        gPrincipalsCollector = new lazy.PrincipalsCollector();
      }
      let principals = await gPrincipalsCollector.getAllPrincipals(progress);
      selectedPrincipals.push(
        ...extractMatchingPrincipals(principals, permission.principal.host)
      );
    }
    await maybeSanitizeSessionPrincipals(
      progress,
      selectedPrincipals,
        Ci.nsIClearDataService.CLEAR_ALL_CACHES |
        Ci.nsIClearDataService.CLEAR_COOKIES |
        Ci.nsIClearDataService.CLEAR_DOM_STORAGES |
        Ci.nsIClearDataService.CLEAR_BOUNCE_TRACKING_PROTECTION_STATE
    );
    progress.sanitizationPrefs.session_permission_exceptions = exceptions;
  }

  await cleanupAfterSanitization(Ci.nsIClearDataService.CLEAR_ALL);

  progress.advancement = "done";
}

async function cleanupAfterSanitization(flags) {
  await new Promise(resolve =>
    Services.clearData.cleanupAfterDeletionAtShutdown(flags, resolve)
  );
}

function extractMatchingPrincipals(principals, matchHost) {
  return principals.filter(principal => {
    return Services.eTLD.hasRootDomain(matchHost, principal.host);
  });
}

/**
 * This method receives a list of principals and it checks if some of them or
 * some of their sub-domain need to be sanitize.
 *
 * @param {object} progress - Object to keep track of the sanitization progress, prefs and mode
 * @param {nsIPrincipal[]} principals - The principals generated by the PrincipalsCollector
 * @param {int} flags - The cleaning categories that need to be cleaned for the principals.
 * @returns {Promise} - Resolves once the clearing of the principals to be cleared is done
 */
async function maybeSanitizeSessionPrincipals(progress, principals, flags) {
  log("Sanitizing " + principals.length + " principals");

  let promises = [];
  let exceptionPartitionSites = new Set();
  let shutdownExceptionHosts = [];
  for (let perm of Services.perms.getAllByTypes(["persist-data-on-shutdown"])) {
    if (
      perm.capability == Ci.nsIPermissionManager.ALLOW_ACTION &&
      isSupportedPrincipal(perm.principal)
    ) {
      exceptionPartitionSites.add(perm.principal.baseDomain);
      shutdownExceptionHosts.push(perm.principal.host);
    }
  }

  principals.forEach(principal => {
    progress.step = "checking-principal";
    let preserve;
    if (isCookieSession(principal)) {
      preserve = false;
    } else {
      preserve = isShutdownExceptionApplicable(
        principal,
        exceptionPartitionSites,
        shutdownExceptionHosts
      );
    }
    progress.step = "principal-checked:" + preserve;

    if (!preserve) {
      promises.push(sanitizeSessionPrincipal(progress, principal, flags));
    }
  });

  progress.step = "promises:" + promises.length;
  if (promises.length) {
    await Promise.all(promises);
  }
  progress.step = "promises resolved";
}

function isShutdownExceptionApplicable(
  principal,
  exceptionPartitionSites,
  shutdownExceptionHosts
) {
  let { partitionKey } = principal.originAttributes;
  if (!partitionKey) {
    if (
      Services.perms.testPermissionFromPrincipal(
        principal,
        "persist-data-on-shutdown"
      ) == Ci.nsIPermissionManager.ALLOW_ACTION
    ) {
      return true;
    }
    return shutdownExceptionHosts.some(host =>
      Services.eTLD.hasRootDomain(host, principal.host)
    );
  }
  let baseDomain;
  try {
    baseDomain = ChromeUtils.getBaseDomainFromPartitionKey(partitionKey);
  } catch {
    return false;
  }
  return exceptionPartitionSites.has(baseDomain);
}

function isCookieSession(principal) {
  return (
    Services.perms.testPermissionFromPrincipal(principal, "cookie") ==
    Ci.nsICookiePermission.ACCESS_SESSION
  );
}

async function sanitizeSessionPrincipal(progress, principal, flags) {
  log("Sanitizing principal: " + principal.asciiSpec);

  await new Promise(resolve => {
    progress.sanitizePrincipal = "started";
    Services.clearData.deleteDataFromPrincipal(
      principal,
      true ,
      flags,
      resolve
    );
  });
  progress.sanitizePrincipal = "completed";
}


function getItemsToClearFromPrefBranch(branch) {
  branch = Services.prefs.getBranch(branch);
  return Object.keys(Sanitizer.items).filter(itemName => {
    try {
      return branch.getBoolPref(itemName);
    } catch (ex) {
      return false;
    }
  });
}

function addPendingSanitization(id, itemsToClear, options) {
  let pendingSanitizations = safeGetPendingSanitizations();
  pendingSanitizations.push({ id, itemsToClear, options });
  Services.prefs.setStringPref(
    Sanitizer.PREF_PENDING_SANITIZATIONS,
    JSON.stringify(pendingSanitizations)
  );
}

function removePendingSanitization(id) {
  let pendingSanitizations = safeGetPendingSanitizations();
  let i = pendingSanitizations.findIndex(s => s.id == id);
  let [s] = pendingSanitizations.splice(i, 1);
  Services.prefs.setStringPref(
    Sanitizer.PREF_PENDING_SANITIZATIONS,
    JSON.stringify(pendingSanitizations)
  );
  return s;
}

function getAndClearPendingSanitizations() {
  let pendingSanitizations = safeGetPendingSanitizations();
  if (pendingSanitizations.length) {
    Services.prefs.clearUserPref(Sanitizer.PREF_PENDING_SANITIZATIONS);
  }
  return pendingSanitizations;
}

function safeGetPendingSanitizations() {
  try {
    return JSON.parse(
      Services.prefs.getStringPref(Sanitizer.PREF_PENDING_SANITIZATIONS, "[]")
    );
  } catch (ex) {
    console.error("Invalid JSON value for pending sanitizations: ", ex);
    return [];
  }
}

async function clearData(range, flags) {
  if (range) {
    await new Promise(resolve => {
      Services.clearData.deleteDataInTimeRange(
        range[0],
        range[1],
        true ,
        flags,
        resolve
      );
    });
  } else {
    await new Promise(resolve => {
      Services.clearData.deleteData(flags, resolve);
    });
  }
}

function isSupportedPrincipal(principal) {
  return ["http", "https", "file"].some(scheme => principal.schemeIs(scheme));
}
