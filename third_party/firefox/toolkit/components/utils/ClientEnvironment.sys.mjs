/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AttributionCode:
    "moz-src:///browser/components/attribution/AttributionCode.sys.mjs",
  NormandyUtils: "resource://normandy/lib/NormandyUtils.sys.mjs",
  Region: "resource://gre/modules/Region.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  ShellService: "moz-src:///browser/components/shell/ShellService.sys.mjs",
  TelemetryArchive: "resource://gre/modules/TelemetryArchive.sys.mjs",
  TelemetryController: "resource://gre/modules/TelemetryController.sys.mjs",
  UpdateUtils: "resource://gre/modules/UpdateUtils.sys.mjs",
  WindowsRegistry: "resource://gre/modules/WindowsRegistry.sys.mjs",
});

function getOsVersion() {
  let version = null;
  try {
    version = Services.sysinfo.getProperty("version", null);
  } catch (_e) {
  }
  if (version) {
    version = version.toString();
  }
  return version;
}

export class ClientEnvironmentBase {
  static get country() {
    return lazy.Region.home;
  }

  static get distribution() {
    return Services.prefs
      .getDefaultBranch(null)
      .getCharPref("distribution.id", "default");
  }

  static get formFactor() {
    return ["android", "ios"].includes(AppConstants.platform)
      ? "phone"
      : "desktop";
  }

  static get telemetry() {
    return (async () => {
      const pings = await lazy.TelemetryArchive.promiseArchivedPingList();

      const mostRecentPings = {};
      for (const ping of pings) {
        if (ping.type in mostRecentPings) {
          if (
            mostRecentPings[ping.type].timestampCreated < ping.timestampCreated
          ) {
            mostRecentPings[ping.type] = ping;
          }
        } else {
          mostRecentPings[ping.type] = ping;
        }
      }

      const telemetry = {};
      for (const key in mostRecentPings) {
        const ping = mostRecentPings[key];
        telemetry[ping.type] =
          await lazy.TelemetryArchive.promiseArchivedPingById(ping.id);
      }
      return telemetry;
    })();
  }

  static get liveTelemetry() {
    let target = {};
    try {
      target.main = lazy.TelemetryController.getCurrentPingData();
    } catch (err) {
      console.error(err);
    }

    return new Proxy(target, {
      get(target, prop) {
        if (prop == "main") {
          return target.main;
        }
        if (prop == "then") {
          return undefined;
        }
        throw new Error(
          `Live telemetry only includes the main ping, not the ${prop} ping`
        );
      },
      has(target, prop) {
        return prop == "main";
      },
    });
  }

  static get randomizationId() {
    let id = Services.prefs.getCharPref("app.normandy.user_id", "");
    if (!id) {
      id = lazy.NormandyUtils.generateUuid();
      Services.prefs.setCharPref("app.normandy.user_id", id);
    }
    return id;
  }

  static get version() {
    return AppConstants.MOZ_APP_VERSION_DISPLAY;
  }

  static get channel() {
    return lazy.UpdateUtils.getUpdateChannel(false);
  }

  static get isDefaultBrowser() {
    return lazy.ShellService.isDefaultBrowser();
  }

  static get searchEngine() {
    return (async () => {
      const defaultEngineInfo = await lazy.SearchService.getDefault();
      return defaultEngineInfo.telemetryId;
    })();
  }

  static get locale() {
    return Services.locale.appLocaleAsBCP47;
  }

  static get doNotTrack() {
    return Services.prefs.getBoolPref(
      "privacy.donottrackheader.enabled",
      false
    );
  }

  static get os() {
    function coerceToNumber(version) {
      if (!version) {
        return null;
      }
      const parts = version.split(".");
      return parseFloat(parts.slice(0, 2).join("."));
    }

    let osInfo = {
      isWindows: AppConstants.platform == "win",
      isMac: AppConstants.platform === "macosx",
      isLinux: AppConstants.platform === "linux",

      get name() {
        return ClientEnvironmentBase.appinfo.OS;
      },

      get version() {
        return getOsVersion();
      },

      get windowsVersion() {
        if (!osInfo.isWindows) {
          return null;
        }
        return coerceToNumber(getOsVersion());
      },

      get windowsBuildNumber() {
        if (!osInfo.isWindows) {
          return null;
        }

        return lazy.WindowsVersionInfo.get({ throwOnError: false }).buildNumber;
      },

      get windowsUBR() {
        if (!osInfo.isWindows) {
          return null;
        }

        const ubr = lazy.WindowsRegistry.readRegKey(
          Ci.nsIWindowsRegKey.ROOT_KEY_LOCAL_MACHINE,
          "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
          "UBR",
          Ci.nsIWindowsRegKey.WOW64_64
        );
        return Number.isInteger(ubr) ? ubr : null;
      },

      get macVersion() {
        const darwinVersion = osInfo.darwinVersion;
        if (darwinVersion >= 5) {
          const intPart = Math.floor(darwinVersion);
          return 10 + 0.1 * (intPart - 4);
        }
        return null;
      },

      get darwinVersion() {
        if (!osInfo.isMac) {
          return null;
        }
        return coerceToNumber(getOsVersion());
      },

    };

    return osInfo;
  }

  static get attribution() {
    return lazy.AttributionCode.getAttrDataAsync();
  }

  static get appinfo() {
    Services.appinfo.QueryInterface(Ci.nsIXULAppInfo);
    Services.appinfo.QueryInterface(Ci.nsIPlatformInfo);
    return Services.appinfo;
  }
}
