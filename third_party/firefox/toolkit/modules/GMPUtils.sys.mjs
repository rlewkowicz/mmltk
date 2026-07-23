/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  UpdateUtils: "resource://gre/modules/UpdateUtils.sys.mjs",
});

export const OPEN_H264_ID = "gmp-gmpopenh264";
export const WIDEVINE_L1_ID = "gmp-widevinecdm-l1";
export const WIDEVINE_L3_ID = "gmp-widevinecdm";

export const GMP_PLUGIN_IDS = [OPEN_H264_ID, WIDEVINE_L1_ID, WIDEVINE_L3_ID];

export var GMPUtils = {
  isPluginHidden(aPlugin) {
    if (!this._isPluginSupported(aPlugin) || !this._isPluginVisible(aPlugin)) {
      return true;
    }

    if (!aPlugin.isEME) {
      return false;
    }

    if (!GMPPrefs.getBool(GMPPrefs.KEY_EME_ENABLED, true)) {
      return true;
    }

    return false;
  },

  _isPluginSupported(aPlugin) {
    if (this._isPluginForceSupported(aPlugin)) {
      return true;
    }
    if (aPlugin.id == WIDEVINE_L1_ID) {
      return (
        AppConstants.MOZ_WMF_CDM &&
        AppConstants.platform == "win" &&
        lazy.UpdateUtils.ABI.match(/x64/)
      );
    }
    if (aPlugin.id == WIDEVINE_L1_ID || aPlugin.id == WIDEVINE_L3_ID) {
      return (
        AppConstants.platform == "win" ||
        AppConstants.platform == "macosx" ||
        AppConstants.platform == "linux"
      );
    }

    return true;
  },

  _isPluginVisible(aPlugin) {
    return GMPPrefs.getBool(GMPPrefs.KEY_PLUGIN_VISIBLE, false, aPlugin.id);
  },

  _isPluginForceSupported(aPlugin) {
    return GMPPrefs.getBool(
      GMPPrefs.KEY_PLUGIN_FORCE_SUPPORTED,
      false,
      aPlugin.id
    );
  },

  _isWindowsOnARM64() {
    return (
      AppConstants.platform == "win" && lazy.UpdateUtils.ABI.match(/aarch64/)
    );
  },

  _getChromiumUpdateParameters(aPlugin) {
    let params = "";
    if (AppConstants.platform === "win") {
      params += "&os=win";
    } else if (AppConstants.platform === "macosx") {
      params += "&os=mac";
    } else if (AppConstants.platform === "linux") {
      params += "&os=Linux";
    } else {
      throw new Error("Unknown platform " + AppConstants.platform);
    }

    const abi = Services.appinfo.XPCOMABI;
    if (abi.match(/aarch64/)) {
      params += "&arch=arm64&os_arch=arm64";
    } else if (abi.match(/x86_64/)) {
      params += "&arch=x64&os_arch=x64";
    } else if (abi.match(/x86/)) {
      params += "&arch=x86&os_arch=x86";
    } else {
      throw new Error("Unknown ABI " + abi);
    }

    if (
      GMPPrefs.getBool(
        GMPPrefs.KEY_PLUGIN_FORCE_CHROMIUM_BETA,
        false,
        aPlugin.id
      )
    ) {
      params += "&testrequest=1";
    }

    return params;
  },

  _expectedABI(aPlugin) {
    let defaultABI = lazy.UpdateUtils.ABI;
    let expectedABIs = [defaultABI];
    if (aPlugin.id == WIDEVINE_L3_ID && this._isWindowsOnARM64()) {
      expectedABIs.push(defaultABI.replace(/aarch64/g, "x86"));
    }
    return expectedABIs.join(",");
  },
};

export var GMPPrefs = {
  KEY_EME_ENABLED: "media.eme.enabled",
  KEY_PLUGIN_ENABLED: "media.{0}.enabled",
  KEY_PLUGIN_LAST_DOWNLOAD: "media.{0}.lastDownload",
  KEY_PLUGIN_LAST_DOWNLOAD_FAILED: "media.{0}.lastDownloadFailed",
  KEY_PLUGIN_LAST_DOWNLOAD_FAIL_REASON: "media.{0}.lastDownloadFailReason",
  KEY_PLUGIN_LAST_INSTALL_FAILED: "media.{0}.lastInstallFailed",
  KEY_PLUGIN_LAST_INSTALL_FAIL_REASON: "media.{0}.lastInstallFailReason",
  KEY_PLUGIN_LAST_INSTALL_START: "media.{0}.lastInstallStart",
  KEY_PLUGIN_LAST_UPDATE: "media.{0}.lastUpdate",
  KEY_PLUGIN_HASHVALUE: "media.{0}.hashValue",
  KEY_PLUGIN_VERSION: "media.{0}.version",
  KEY_PLUGIN_AUTOUPDATE: "media.{0}.autoupdate",
  KEY_PLUGIN_VISIBLE: "media.{0}.visible",
  KEY_PLUGIN_ABI: "media.{0}.abi",
  KEY_PLUGIN_FORCE_SUPPORTED: "media.{0}.forceSupported",
  KEY_PLUGIN_FORCE_INSTALL: "media.{0}.forceInstall",
  KEY_PLUGIN_ALLOW_X64_ON_ARM64: "media.{0}.allow-x64-plugin-on-arm64",
  KEY_PLUGIN_CHROMIUM_GUID: "media.{0}.chromium-guid",
  KEY_PLUGIN_ALLOW_CHROMIUM_UPDATE: "media.{0}.allow-chromium-update",
  KEY_PLUGIN_FORCE_CHROMIUM_UPDATE: "media.{0}.force-chromium-update",
  KEY_PLUGIN_FORCE_CHROMIUM_BETA: "media.{0}.force-chromium-beta",
  KEY_ALLOW_LOCAL_SOURCES: "media.gmp-manager.allowLocalSources",
  KEY_URL: "media.gmp-manager.url",
  KEY_URL_OVERRIDE: "media.gmp-manager.url.override",
  KEY_CHROMIUM_UPDATE_URL: "media.gmp-manager.chromium-update-url",
  KEY_UPDATE_LAST_CHECK: "media.gmp-manager.lastCheck",
  KEY_UPDATE_LAST_EMPTY_CHECK: "media.gmp-manager.lastEmptyCheck",
  KEY_SECONDS_BETWEEN_CHECKS: "media.gmp-manager.secondsBetweenChecks",
  KEY_UPDATE_ENABLED: "media.gmp-manager.updateEnabled",
  KEY_APP_DISTRIBUTION: "distribution.id",
  KEY_APP_DISTRIBUTION_VERSION: "distribution.version",
  KEY_BUILDID: "media.gmp-manager.buildID",
  KEY_PROVIDER_ENABLED: "media.gmp-provider.enabled",
  KEY_LOG_BASE: "media.gmp.log.",
  KEY_LOGGING_LEVEL: "media.gmp.log.level",
  KEY_LOGGING_DUMP: "media.gmp.log.dump",

  getString(aKey, aDefaultValue, aPlugin) {
    if (
      aKey === this.KEY_APP_DISTRIBUTION ||
      aKey === this.KEY_APP_DISTRIBUTION_VERSION
    ) {
      return Services.prefs.getDefaultBranch(null).getCharPref(aKey, "default");
    }
    return Services.prefs.getStringPref(
      this.getPrefKey(aKey, aPlugin),
      aDefaultValue
    );
  },

  getInt(aKey, aDefaultValue, aPlugin) {
    return Services.prefs.getIntPref(
      this.getPrefKey(aKey, aPlugin),
      aDefaultValue
    );
  },

  getBool(aKey, aDefaultValue, aPlugin) {
    return Services.prefs.getBoolPref(
      this.getPrefKey(aKey, aPlugin),
      aDefaultValue
    );
  },

  setString(aKey, aVal, aPlugin) {
    Services.prefs.setStringPref(this.getPrefKey(aKey, aPlugin), aVal);
  },

  setBool(aKey, aVal, aPlugin) {
    Services.prefs.setBoolPref(this.getPrefKey(aKey, aPlugin), aVal);
  },

  setInt(aKey, aVal, aPlugin) {
    Services.prefs.setIntPref(this.getPrefKey(aKey, aPlugin), aVal);
  },

  isSet(aKey, aPlugin) {
    return Services.prefs.prefHasUserValue(this.getPrefKey(aKey, aPlugin));
  },

  reset(aKey, aPlugin) {
    Services.prefs.clearUserPref(this.getPrefKey(aKey, aPlugin));
  },

  getPrefKey(aKey, aPlugin) {
    return aKey.replace("{0}", aPlugin || "");
  },
};
