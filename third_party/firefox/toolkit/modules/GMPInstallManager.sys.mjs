/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const DEFAULT_SECONDS_BETWEEN_CHECKS = 60 * 60 * 24;

import { Log } from "resource://gre/modules/Log.sys.mjs";
import {
  GMPPrefs,
  GMPUtils,
  GMP_PLUGIN_IDS,
  WIDEVINE_L1_ID,
  WIDEVINE_L3_ID,
} from "resource://gre/modules/GMPUtils.sys.mjs";

import { ProductAddonChecker } from "resource://gre/modules/addons/ProductAddonChecker.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  FileUtils: "resource://gre/modules/FileUtils.sys.mjs",
  ServiceRequest: "resource://gre/modules/ServiceRequest.sys.mjs",
  UpdateUtils: "resource://gre/modules/UpdateUtils.sys.mjs",
});

function getScopedLogger(prefix) {
  return Log.repository.getLoggerWithMessagePrefix("Toolkit.GMP", prefix + " ");
}

const LOCAL_GMP_SOURCES = [
  {
    id: "gmp-gmpopenh264",
    src: "chrome://global/content/gmp-sources/openh264.json",
    installByDefault: true,
  },
  {
    id: "gmp-widevinecdm",
    src: "chrome://global/content/gmp-sources/widevinecdm.json",
    installByDefault: true,
  },
  {
    id: "gmp-widevinecdm-l1",
    src: "chrome://global/content/gmp-sources/widevinecdm_l1.json",
    installByDefault: false,
  },
];

function getLocalSources() {
  if (GMPPrefs.getBool(GMPPrefs.KEY_ALLOW_LOCAL_SOURCES, true)) {
    return LOCAL_GMP_SOURCES;
  }

  let log = getScopedLogger("GMPInstallManager.checkForAddons");
  log.info("ignoring local sources");
  return [];
}

function redirectChromiumUpdateService(uri) {
  let log = getScopedLogger("GMPInstallManager.checkForAddons");
  log.info("fetching redirect from: " + uri);
  return new Promise((resolve, reject) => {
    let xmlHttp = new lazy.ServiceRequest({ mozAnon: true });

    xmlHttp.onload = function () {
      resolve(this.responseURL);
    };

    xmlHttp.onerror = function (e) {
      reject("Fetching " + uri + " results in error code: " + e.target.status);
    };

    xmlHttp.open("GET", uri);
    xmlHttp.overrideMimeType("*/*");
    xmlHttp.setRequestHeader("Range", "bytes=0-0");
    xmlHttp.send();
  });
}

function downloadJSON(uri) {
  let log = getScopedLogger("GMPInstallManager.checkForAddons");
  log.info("fetching config from: " + uri);
  return new Promise((resolve, reject) => {
    let xmlHttp = new lazy.ServiceRequest({ mozAnon: true });

    xmlHttp.onload = function () {
      resolve(JSON.parse(this.responseText));
    };

    xmlHttp.onerror = function (e) {
      reject("Fetching " + uri + " results in error code: " + e.target.status);
    };

    xmlHttp.open("GET", uri);
    xmlHttp.overrideMimeType("application/json");
    xmlHttp.send();
  });
}

function downloadLocalConfig(sources) {
  if (!sources.length) {
    return Promise.resolve({ addons: [] });
  }

  let log = getScopedLogger("GMPInstallManager.downloadLocalConfig");
  return Promise.all(
    sources.map(conf => {
      return downloadJSON(conf.src).then(addons => {
        let platforms = addons.vendors[conf.id].platforms;
        let target = Services.appinfo.OS + "_" + lazy.UpdateUtils.ABI;
        let details = null;

        while (!details) {
          if (!(target in platforms)) {
            log.info("no details found for: " + target);
            return false;
          }
          if (platforms[target].alias) {
            target = platforms[target].alias;
          } else {
            details = platforms[target];
          }
        }

        log.info("found plugin: " + conf.id);
        return {
          id: conf.id,
          URL: details.fileUrl,
          mirrorURLs: details.mirrorUrls,
          hashFunction: addons.hashFunction,
          hashValue: details.hashValue,
          version: addons.vendors[conf.id].version,
          size: details.filesize,
          usedFallback: true,
        };
      });
    })
  ).then(addons => {
    return { addons: addons.filter(x => x !== false) };
  });
}

export function GMPInstallManager() {}

GMPInstallManager.prototype = {
  async _getURL() {
    let log = getScopedLogger("GMPInstallManager._getURL");
    let url = GMPPrefs.getString(GMPPrefs.KEY_URL_OVERRIDE, "");
    if (url) {
      log.info("Using override url: " + url);
    } else {
      url = GMPPrefs.getString(GMPPrefs.KEY_URL);
      log.info("Using url: " + url);
    }

    url = await lazy.UpdateUtils.formatUpdateURL(url);

    log.info("Using url (with replacement): " + url);
    return url;
  },

  _getContentSignatureRootForURL(url) {
    if (url.startsWith("https://aus")) {
      return Ci.nsIContentSignatureVerifier.ContentSignatureProdRoot;
    }
    if (url.startsWith("https://stage.")) {
      return Ci.nsIContentSignatureVerifier.ContentSignatureStageRoot;
    }
    return Ci.nsIContentSignatureVerifier.ContentSignatureProdRoot;
  },


  async checkForAddons() {
    let log = getScopedLogger("GMPInstallManager.checkForAddons");
    if (this._deferred) {
      log.error("checkForAddons already called");
      return Promise.reject({ type: "alreadycalled" });
    }

    if (!GMPPrefs.getBool(GMPPrefs.KEY_UPDATE_ENABLED, true)) {
      log.info("Updates are disabled via media.gmp-manager.updateEnabled");
      return { usedFallback: true, addons: [] };
    }

    this._deferred = Promise.withResolvers();
    let deferredPromise = this._deferred.promise;

    let url = await this._getURL();
    let trustedContentSignatureRoot = this._getContentSignatureRootForURL(url);

    log.info(
      `Fetching product addon list url=${url}, trustedContentSignatureRoot=${trustedContentSignatureRoot}`
    );

    let success = true;
    let res;
    try {
      res = await ProductAddonChecker.getProductAddonList(
        url,
         true,
         true,
        trustedContentSignatureRoot
      );
    } catch (err) {
      success = false;
    }

    let localSources = getLocalSources();

    try {
      if (!success) {
        log.info("Falling back to local config");
        let fallbackSources = localSources.filter(function (gmpSource) {
          return gmpSource.installByDefault;
        });
        res = await downloadLocalConfig(fallbackSources);
      }
    } catch (err) {
      this._deferred.reject(err);
      delete this._deferred;
      return deferredPromise;
    }

    let addons;
    if (res && res.addons) {
      addons = res.addons.map(a => new GMPAddon(a));
    } else {
      addons = [];
    }

    try {
      let forcedSources = localSources.filter(function (gmpSource) {
        return GMPPrefs.getBool(
          GMPPrefs.KEY_PLUGIN_FORCE_INSTALL,
          false,
          gmpSource.id
        );
      });

      let forcedConfigs = await downloadLocalConfig(
        forcedSources.filter(function (gmpSource) {
          return !addons.find(gmpAddon => gmpAddon.id == gmpSource.id);
        })
      );

      let forcedAddons = forcedConfigs.addons.map(
        config => new GMPAddon(config)
      );

      log.info("Forced " + forcedAddons.length + " addons.");
      addons = addons.concat(forcedAddons);
    } catch (err) {
      log.info("Failed to force addons: " + err);
    }

    await this.adjustForChromiumUpdateService(addons);

    this._deferred.resolve({ addons });
    delete this._deferred;
    return deferredPromise;
  },
  async adjustForChromiumUpdateService(addons) {
    let log = getScopedLogger("GMPInstallManager.checkForAddons");
    for (let gmpAddon of addons) {
      try {
        const forced = GMPPrefs.getBool(
          GMPPrefs.KEY_PLUGIN_FORCE_CHROMIUM_UPDATE,
          false,
          gmpAddon.id
        );
        const allowed = GMPPrefs.getBool(
          GMPPrefs.KEY_PLUGIN_ALLOW_CHROMIUM_UPDATE,
          false,
          gmpAddon.id
        );
        if (!allowed && !forced) {
          continue;
        }

        const guid = GMPPrefs.getString(
          GMPPrefs.KEY_PLUGIN_CHROMIUM_GUID,
          "",
          gmpAddon.id
        );
        if (guid === "") {
          log.warn("Skipping chromium update, missing GUID for ", gmpAddon.id);
          continue;
        }

        const params = GMPUtils._getChromiumUpdateParameters(gmpAddon);
        const serviceUrl = GMPPrefs.getString(
          GMPPrefs.KEY_CHROMIUM_UPDATE_URL,
          ""
        );
        const redirectUrl = await redirectChromiumUpdateService(
          serviceUrl.replace("%GUID%", guid) + params
        );
        const versionMatch = redirectUrl.match(/_(\d+\.\d+\.\d+\.\d+)\//);
        if (!versionMatch || versionMatch.length !== 2) {
          log.warn(
            "Skipping chromium update, no version from URL: ",
            redirectUrl
          );
          continue;
        }

        const version = versionMatch[1];
        if (forced) {
          gmpAddon.mirrorURLs = [];
          gmpAddon.version = version;
          gmpAddon.forcedChromiumUpdate = true;

          delete gmpAddon.size;
          delete gmpAddon.hashValue;
          delete gmpAddon.hashFunction;
        } else {
          if (gmpAddon.version !== version) {
            log.warn(
              "Skipping chromium update, expected version " +
                gmpAddon.version +
                ", got " +
                version
            );
            continue;
          }

          gmpAddon.mirrorURLs = gmpAddon.mirrorURLs.filter(
            url => url !== redirectUrl
          );

          if (gmpAddon.URL !== redirectUrl) {
            gmpAddon.mirrorURLs.unshift(gmpAddon.URL);
          }
        }

        gmpAddon.URL = redirectUrl;

        log.info(
          "Downloading " +
            gmpAddon.id +
            " version " +
            version +
            " from chromium update " +
            redirectUrl
        );
      } catch (err) {
        log.info(
          "Failed to switch addon " +
            gmpAddon.id +
            " to Chromium update service: " +
            err
        );
      }
    }
  },
  installAddon(gmpAddon) {
    if (this._deferred) {
      let log = getScopedLogger("GMPInstallManager.installAddon");
      log.error("previous error encountered");
      return Promise.reject({ type: "previouserrorencountered" });
    }
    this.gmpDownloader = new GMPDownloader(gmpAddon);
    return this.gmpDownloader.start();
  },
  _getTimeSinceLastCheck() {
    let now = Math.round(Date.now() / 1000);
    let lastCheck = GMPPrefs.getInt(GMPPrefs.KEY_UPDATE_LAST_CHECK, 0);
    if (now < lastCheck) {
      return now;
    }
    return now - lastCheck;
  },
  get _isEMEEnabled() {
    return GMPPrefs.getBool(GMPPrefs.KEY_EME_ENABLED, true);
  },
  _isAddonEnabled(aAddon) {
    return GMPPrefs.getBool(GMPPrefs.KEY_PLUGIN_ENABLED, true, aAddon);
  },
  _isAddonUpdateEnabled(aAddon) {
    return (
      this._isAddonEnabled(aAddon) &&
      GMPPrefs.getBool(GMPPrefs.KEY_PLUGIN_AUTOUPDATE, true, aAddon)
    );
  },
  _updateLastCheck() {
    let now = Math.round(Date.now() / 1000);
    GMPPrefs.setInt(GMPPrefs.KEY_UPDATE_LAST_CHECK, now);
  },
  _versionchangeOccurred() {
    let savedBuildID = GMPPrefs.getString(GMPPrefs.KEY_BUILDID, "");
    let buildID = Services.appinfo.platformBuildID || "";
    if (savedBuildID == buildID) {
      return false;
    }
    GMPPrefs.setString(GMPPrefs.KEY_BUILDID, buildID);
    return true;
  },
  async simpleCheckAndInstall() {
    let log = getScopedLogger("GMPInstallManager.simpleCheckAndInstall");

    if (this._versionchangeOccurred()) {
      log.info(
        "A version change occurred. Ignoring " +
          "media.gmp-manager.lastCheck to check immediately for " +
          "new or updated GMPs."
      );
    } else {
      let secondsBetweenChecks = GMPPrefs.getInt(
        GMPPrefs.KEY_SECONDS_BETWEEN_CHECKS,
        DEFAULT_SECONDS_BETWEEN_CHECKS
      );
      let secondsSinceLast = this._getTimeSinceLastCheck();
      log.info(
        "Last check was: " +
          secondsSinceLast +
          " seconds ago, minimum seconds: " +
          secondsBetweenChecks
      );
      if (secondsBetweenChecks > secondsSinceLast) {
        log.info("Will not check for updates.");
        return { status: "too-frequent-no-check" };
      }
    }

    try {
      let { addons } = await this.checkForAddons();
      this._updateLastCheck();
      log.info("Found " + addons.length + " addons advertised.");
      let addonsToInstall = addons.filter(function (gmpAddon) {
        log.info("Found addon: " + gmpAddon.toString());

        if (!gmpAddon.isValid) {
          log.info("Addon |" + gmpAddon.id + "| is invalid.");
          return false;
        }

        if (GMPUtils.isPluginHidden(gmpAddon)) {
          log.info("Addon |" + gmpAddon.id + "| has been hidden.");
          return false;
        }

        if (gmpAddon.isInstalled) {
          log.info("Addon |" + gmpAddon.id + "| already installed.");
          return false;
        }

        if (gmpAddon.usedFallback && gmpAddon.isUpdate) {
          log.info(
            "Addon |" +
              gmpAddon.id +
              "| not installing updates based " +
              "on fallback."
          );
          return false;
        }

        let addonUpdateEnabled = false;
        if (GMP_PLUGIN_IDS.includes(gmpAddon.id)) {
          if (!this._isAddonEnabled(gmpAddon.id)) {
            log.info(
              "GMP |" + gmpAddon.id + "| has been disabled; skipping check."
            );
          } else if (!this._isAddonUpdateEnabled(gmpAddon.id)) {
            log.info(
              "Auto-update is off for " + gmpAddon.id + ", skipping check."
            );
          } else {
            addonUpdateEnabled = true;
          }
        } else {
          log.info(
            "Auto-update is off for unknown plugin '" +
              gmpAddon.id +
              "', skipping check."
          );
        }

        return addonUpdateEnabled;
      }, this);

      if (!addonsToInstall.length) {
        let now = Math.round(Date.now() / 1000);
        GMPPrefs.setInt(GMPPrefs.KEY_UPDATE_LAST_EMPTY_CHECK, now);
        log.info("No new addons to install, returning");
        return { status: "nothing-new-to-install" };
      }

      let installResults = [];
      let failureEncountered = false;
      for (let addon of addonsToInstall) {
        try {
          await this.installAddon(addon);
          installResults.push({
            id: addon.id,
            result: "succeeded",
          });
        } catch (e) {
          failureEncountered = true;
          installResults.push({
            id: addon.id,
            result: "failed",
          });
        }
      }
      if (failureEncountered) {
        // eslint-disable-next-line no-throw-literal
        throw { status: "failed", results: installResults };
      }
      return { status: "succeeded", results: installResults };
    } catch (e) {
      log.error("Could not check for addons", e);
      throw e;
    }
  },

  uninit() {
    let log = getScopedLogger("GMPInstallManager.uninit");
    if (this._request) {
      log.info("Aborting request");
      this._request.abort();
    }
    if (this._deferred) {
      log.info("Rejecting deferred");
      this._deferred.reject({ type: "uninitialized" });
    }
    log.info("Done cleanup");
  },

  overrideLeaveDownloadedZip: false,
};

export function GMPAddon(addon) {
  let log = getScopedLogger("GMPAddon.constructor");
  this.usedFallback = false;
  this.forcedChromiumUpdate = false;
  for (let name of Object.keys(addon)) {
    this[name] = addon[name];
  }
  log.info("Created new addon: " + this.toString());
}

GMPAddon.prototype = {
  toString() {
    return (
      this.id +
      " (" +
      "isValid: " +
      this.isValid +
      ", isInstalled: " +
      this.isInstalled +
      ", hashFunction: " +
      this.hashFunction +
      ", hashValue: " +
      this.hashValue +
      (this.size !== undefined ? ", size: " + this.size : "") +
      ")"
    );
  },
  get isValid() {
    return (
      this.id &&
      this.URL &&
      this.version &&
      (this.forcedChromiumUpdate || (this.hashFunction && !!this.hashValue))
    );
  },
  get isInstalled() {
    return (
      this.version &&
      GMPPrefs.getString(GMPPrefs.KEY_PLUGIN_VERSION, "", this.id) ===
        this.version &&
      (this.forcedChromiumUpdate ||
        (!!this.hashValue &&
          GMPPrefs.getString(GMPPrefs.KEY_PLUGIN_HASHVALUE, "", this.id) ===
            this.hashValue))
    );
  },
  get isEME() {
    return this.id == WIDEVINE_L1_ID || this.id == WIDEVINE_L3_ID;
  },
  get isOpenH264() {
    return this.id == "gmp-gmpopenh264";
  },
  get isUpdate() {
    return (
      this.version &&
      GMPPrefs.getBool(GMPPrefs.KEY_PLUGIN_VERSION, false, this.id)
    );
  },
};

export function GMPExtractor(zipPath, relativeInstallPath) {
  this.zipPath = zipPath;
  this.relativeInstallPath = relativeInstallPath;
}

GMPExtractor.prototype = {
  install() {
    this._deferred = Promise.withResolvers();
    let deferredPromise = this._deferred;
    let { zipPath, relativeInstallPath } = this;
    let zipFile = new lazy.FileUtils.File(zipPath);
    let zipURI = Services.io.newFileURI(zipFile).spec;
    let worker = new ChromeWorker(
      "resource://gre/modules/GMPExtractor.worker.js"
    );
    worker.onmessage = function (msg) {
      let log = getScopedLogger("GMPExtractor");
      worker.terminate();
      if (msg.data.result != "success") {
        log.error("Failed to extract zip file: " + zipURI);
        log.error("Exception: " + msg.data.exception);
        return deferredPromise.reject({
          target: this,
          status: msg.data.exception,
          type: "exception",
        });
      }
      log.info("Successfully extracted zip file: " + zipURI);
      return deferredPromise.resolve(msg.data.extractedPaths);
    };
    worker.postMessage({ zipURI, relativeInstallPath });
    return this._deferred.promise;
  },
};

export function GMPDownloader(gmpAddon) {
  this._gmpAddon = gmpAddon;
}

GMPDownloader.prototype = {
  start() {
    let log = getScopedLogger("GMPDownloader");
    let gmpAddon = this._gmpAddon;
    let now = Math.round(Date.now() / 1000);
    GMPPrefs.setInt(GMPPrefs.KEY_PLUGIN_LAST_INSTALL_START, now, gmpAddon.id);

    if (!gmpAddon.isValid) {
      log.info("gmpAddon is not valid, will not continue");
      return Promise.reject({
        target: this,
        type: "downloaderr",
      });
    }
    return ProductAddonChecker.downloadAddon(gmpAddon).then(
      zipPath => {
        now = Math.round(Date.now() / 1000);
        GMPPrefs.setInt(GMPPrefs.KEY_PLUGIN_LAST_DOWNLOAD, now, gmpAddon.id);
        log.info(
          `install to directory path: ${gmpAddon.id}/${gmpAddon.version}`
        );
        let gmpInstaller = new GMPExtractor(zipPath, [
          gmpAddon.id,
          gmpAddon.version,
        ]);
        let installPromise = gmpInstaller.install();
        return installPromise
          .then(
            extractedPaths => {
              now = Math.round(Date.now() / 1000);
              GMPPrefs.setInt(
                GMPPrefs.KEY_PLUGIN_LAST_UPDATE,
                now,
                gmpAddon.id
              );
              let abi = GMPUtils._expectedABI(gmpAddon);
              log.info("Setting ABI to '" + abi + "' for " + gmpAddon.id);
              GMPPrefs.setString(GMPPrefs.KEY_PLUGIN_ABI, abi, gmpAddon.id);
              if (!gmpAddon.forcedChromiumUpdate) {
                GMPPrefs.setString(
                  GMPPrefs.KEY_PLUGIN_HASHVALUE,
                  gmpAddon.hashValue,
                  gmpAddon.id
                );
              } else {
                GMPPrefs.reset(GMPPrefs.KEY_PLUGIN_HASHVALUE, gmpAddon.id);
              }
              GMPPrefs.setString(
                GMPPrefs.KEY_PLUGIN_VERSION,
                gmpAddon.version,
                gmpAddon.id
              );
              return extractedPaths;
            },
            reason => {
              GMPPrefs.setString(
                GMPPrefs.KEY_PLUGIN_LAST_INSTALL_FAIL_REASON,
                reason,
                gmpAddon.id
              );
              now = Math.round(Date.now() / 1000);
              GMPPrefs.setInt(
                GMPPrefs.KEY_PLUGIN_LAST_INSTALL_FAILED,
                now,
                gmpAddon.id
              );
              throw reason;
            }
          )
          .finally(() => {
            log.info(`Deleting ${gmpAddon.id} temporary zip file ${zipPath}`);
            Services.obs.notifyObservers(null, "flush-cache-entry", zipPath);
            IOUtils.remove(zipPath);
          });
      },
      reason => {
        GMPPrefs.setString(
          GMPPrefs.KEY_PLUGIN_LAST_DOWNLOAD_FAIL_REASON,
          reason,
          gmpAddon.id
        );
        now = Math.round(Date.now() / 1000);
        GMPPrefs.setInt(
          GMPPrefs.KEY_PLUGIN_LAST_DOWNLOAD_FAILED,
          now,
          gmpAddon.id
        );
        throw reason;
      }
    );
  },
};

export const GMPInstallManagerTestUtils = {
  async overrideServiceRequest(mockRequest, callback) {
    let originalServiceRequest = lazy.ServiceRequest;
    lazy.ServiceRequest = function () {
      return mockRequest;
    };
    try {
      return await callback();
    } finally {
      lazy.ServiceRequest = originalServiceRequest;
    }
  },
};
