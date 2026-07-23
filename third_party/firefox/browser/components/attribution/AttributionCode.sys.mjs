/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export const AttributionIOUtils = {
  write: async (path, bytes) => IOUtils.write(path, bytes),
  read: async path => IOUtils.read(path),
  exists: async path => IOUtils.exists(path),
};

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  MacAttribution:
    "moz-src:///browser/components/attribution/MacAttribution.sys.mjs",
});
ChromeUtils.defineLazyGetter(lazy, "log", () => {
  let { ConsoleAPI } = ChromeUtils.importESModule(
    "resource://gre/modules/Console.sys.mjs"
  );
  let consoleOptions = {
    maxLogLevel: "error",
    maxLogLevelPref: "browser.attribution.loglevel",
    prefix: "AttributionCode",
  };
  return new ConsoleAPI(consoleOptions);
});

const ATTR_CODE_MAX_LENGTH = 1010;
const ATTR_CODE_VALUE_REGEX = /[a-zA-Z0-9_%\\-\\.\\(\\)]*/;
const ATTR_CODE_FIELD_SEPARATOR = "%26"; 
const ATTR_CODE_KEY_VALUE_SEPARATOR = "%3D"; 
const MSCLKID_KEY_PREFIX = "storeBingAd_";
const ATTR_CODE_KEYS = [
  "source",
  "medium",
  "campaign",
  "content",
  "experiment",
  "variation",
  "ua",
  "dltoken",
  "msstoresignedin",
  "msclkid",
  "dlsource",
];

let gCachedAttrData = null;

export var AttributionCode = {
  async msixCampaignId() {
    const windowsPackageManager = Cc[
      "@mozilla.org/windows-package-manager;1"
    ].createInstance(Ci.nsIWindowsPackageManager);

    return windowsPackageManager.campaignId();
  },

  get attributionFile() {
    if (AppConstants.platform == "win") {
      let file = Services.dirsvc.get("GreD", Ci.nsIFile);
      file.append("postSigningData");
      return file;
    }

    return null;
  },

  async writeAttributionFile(code) {
    if (
      AppConstants.platform === "win" &&
      Services.sysinfo.getProperty("hasWinPackageId")
    ) {
      Services.console.logStringMessage(
        "Attribution code cannot be written for MSIX builds, aborting."
      );
      return;
    }
    let file = AttributionCode.attributionFile;
    await IOUtils.makeDirectory(file.parent.path);
    let bytes = new TextEncoder().encode(code);
    await AttributionIOUtils.write(file.path, bytes);
  },

  get allowedCodeKeys() {
    return [...ATTR_CODE_KEYS];
  },

  parseAttributionCode(code) {
    if (code.length > ATTR_CODE_MAX_LENGTH) {
      return {};
    }

    let isValid = true;
    let parsed = {};
    for (let param of code.split(ATTR_CODE_FIELD_SEPARATOR)) {
      let [key, value] = param.split(ATTR_CODE_KEY_VALUE_SEPARATOR, 2);
      if (key && ATTR_CODE_KEYS.includes(key)) {
        if (value && ATTR_CODE_VALUE_REGEX.test(value)) {
          if (key === "msstoresignedin") {
            if (value === "true") {
              parsed[key] = true;
            } else if (value === "false") {
              parsed[key] = false;
            } else {
              throw new Error("Couldn't parse msstoresignedin");
            }
          } else {
            parsed[key] = value;
          }
        }
      } else if (param.startsWith(MSCLKID_KEY_PREFIX)) {
        parsed.msclkid = param.substring(MSCLKID_KEY_PREFIX.length);
      } else {
        lazy.log.debug(
          `parseAttributionCode: "${code}" => isValid = false: "${key}", "${value}"`
        );
        isValid = false;
        break;
      }
    }

    if (isValid) {
      return parsed;
    }


    return {};
  },

  serializeAttributionData(data) {
    let s = "";
    for (let key of ATTR_CODE_KEYS) {
      if (key in data) {
        let value = data[key];
        if (s) {
          s += ATTR_CODE_FIELD_SEPARATOR; 
        }
        s += `${key}${ATTR_CODE_KEY_VALUE_SEPARATOR}${value}`; 
      }
    }
    return s;
  },

  async _getMacAttrDataAsync() {
    try {
      let attrStr = await lazy.MacAttribution.getAttributionString();
      lazy.log.debug(
        `_getMacAttrDataAsync: getAttributionString: "${attrStr}"`
      );

      if (attrStr === null) {
        gCachedAttrData = {};

        lazy.log.debug(`_getMacAttrDataAsync: null attribution string`);
      } else if (attrStr == "") {
        gCachedAttrData = {};

        lazy.log.debug(`_getMacAttrDataAsync: empty attribution string`);
      } else {
        gCachedAttrData = this.parseAttributionCode(attrStr);
      }
    } catch (ex) {
      gCachedAttrData = {};

      lazy.log.warn("Caught exception fetching macOS attribution codes!", ex);

      if (
        ex instanceof Ci.nsIException &&
        ex.result == Cr.NS_ERROR_UNEXPECTED
      ) {
      }
    }

    lazy.log.debug(
      `macOS attribution data is ${JSON.stringify(gCachedAttrData)}`
    );

    return gCachedAttrData;
  },

  async _getWindowsNSISAttrDataAsync() {
    return AttributionIOUtils.read(this.attributionFile.path);
  },

  async _getWindowsMSIXAttrDataAsync() {
    lazy.log.debug(
      `winPackageFamilyName is: ${Services.sysinfo.getProperty(
        "winPackageFamilyName"
      )}`
    );
    let encoder = new TextEncoder();
    return encoder.encode(encodeURIComponent(await this.msixCampaignId()));
  },

  async getAttrDataAsync() {
    if (AppConstants.platform != "win" && AppConstants.platform != "macosx") {
      return gCachedAttrData;
    }
    if (gCachedAttrData != null) {
      lazy.log.debug(
        `getAttrDataAsync: attribution is cached: ${JSON.stringify(
          gCachedAttrData
        )}`
      );
      return gCachedAttrData;
    }

    gCachedAttrData = {};

    if (AppConstants.platform == "macosx") {
      lazy.log.debug(`getAttrDataAsync: macOS`);
      return this._getMacAttrDataAsync();
    }

    lazy.log.debug("getAttrDataAsync: !macOS");

    let attributionFile = this.attributionFile;
    let bytes;
    try {
      if (
        AppConstants.platform === "win" &&
        Services.sysinfo.getProperty("hasWinPackageId")
      ) {
        lazy.log.debug("getAttrDataAsync: MSIX");
        bytes = await this._getWindowsMSIXAttrDataAsync();
      } else {
        lazy.log.debug("getAttrDataAsync: NSIS");
        bytes = await this._getWindowsNSISAttrDataAsync();
      }
    } catch (ex) {
      if (DOMException.isInstance(ex) && ex.name == "NotFoundError") {
        lazy.log.debug(
          `getAttrDataAsync: !exists("${
            attributionFile.path
          }"), returning ${JSON.stringify(gCachedAttrData)}`
        );
        return gCachedAttrData;
      }
      lazy.log.debug(
        `other error trying to read attribution data:
          attributionFile.path is: ${attributionFile.path}`
      );
      lazy.log.debug("Full exception is:");
      lazy.log.debug(ex);

    }
    if (bytes) {
      try {
        let decoder = new TextDecoder();
        let code = decoder.decode(bytes);
        lazy.log.debug(
          `getAttrDataAsync: attribution bytes deserializes to ${code}`
        );
        if (AppConstants.platform == "macosx" && !code) {
          return gCachedAttrData;
        }

        gCachedAttrData = this.parseAttributionCode(code);
        lazy.log.debug(
          `getAttrDataAsync: ${code} parses to ${JSON.stringify(
            gCachedAttrData
          )}`
        );
      } catch (ex) {
      }
    }

    return gCachedAttrData;
  },

  getCachedAttributionData() {
    return gCachedAttrData;
  },

  async deleteFileAsync() {
    if (AppConstants.platform == "win") {
      try {
        await IOUtils.remove(this.attributionFile.path);
      } catch (ex) {
      }
    }
  },

};
