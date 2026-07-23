/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const DISTRIBUTION_CUSTOMIZATION_COMPLETE_TOPIC =
  "distribution-customization-complete";
const DISTRIBUTION_PREFERENCES_COMPLETE_TOPIC =
  "distribution-preferences-complete";

const PREF_CACHED_FILE_EXISTENCE = "distribution.iniFile.exists.value";
const PREF_CACHED_FILE_APPVERSION = "distribution.iniFile.exists.appversion";

const BOOKMARK_GUID_PREFIX = "DstB-";
const FOLDER_GUID_PREFIX = "DstF-";

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
});

export function DistributionCustomizer() {}

DistributionCustomizer.prototype = {
  get _iniFile() {
    let loadFromProfile = Services.prefs.getBoolPref(
      "distribution.testing.loadFromProfile",
      false
    );

    let iniFile;
    try {
      iniFile = loadFromProfile
        ? Services.dirsvc.get("ProfD", Ci.nsIFile)
        : Services.dirsvc.get("XREAppDist", Ci.nsIFile);
      if (loadFromProfile) {
        iniFile.leafName = "distribution";
      }
      iniFile.append("distribution.ini");
    } catch (ex) {}

    this.__defineGetter__("_iniFile", () => iniFile);
    return iniFile;
  },

  get _hasDistributionIni() {
    if (Services.prefs.prefHasUserValue(PREF_CACHED_FILE_EXISTENCE)) {
      let knownForVersion = Services.prefs.getStringPref(
        PREF_CACHED_FILE_APPVERSION,
        "unknown"
      );
      if (
        knownForVersion == AppConstants.MOZ_APP_VERSION &&
        (false ||
          Cc["@mozilla.org/startupcacheinfo;1"].getService(
            Ci.nsIStartupCacheInfo
          ).FoundDiskCacheOnInit)
      ) {
        return Services.prefs.getBoolPref(PREF_CACHED_FILE_EXISTENCE);
      }
    }

    let fileExists = this._iniFile.exists();
    Services.prefs.setBoolPref(PREF_CACHED_FILE_EXISTENCE, fileExists);
    Services.prefs.setStringPref(
      PREF_CACHED_FILE_APPVERSION,
      AppConstants.MOZ_APP_VERSION
    );

    this.__defineGetter__("_hasDistributionIni", () => fileExists);
    return fileExists;
  },

  get _ini() {
    let ini = null;
    try {
      if (this._hasDistributionIni) {
        ini = Cc["@mozilla.org/xpcom/ini-parser-factory;1"]
          .getService(Ci.nsIINIParserFactory)
          .createINIParser(this._iniFile);
      }
    } catch (e) {
      if (e.result == Cr.NS_ERROR_FILE_NOT_FOUND) {
        Services.prefs.clearUserPref(PREF_CACHED_FILE_EXISTENCE);
      } else {
        console.error("Unable to parse distribution.ini");
      }
    }
    this.__defineGetter__("_ini", () => ini);
    return this._ini;
  },

  get _locale() {
    const locale = Services.locale.requestedLocale || "en-US";
    this.__defineGetter__("_locale", () => locale);
    return this._locale;
  },

  get _language() {
    let language = this._locale.split("-")[0];
    this.__defineGetter__("_language", () => language);
    return this._language;
  },

  async _removeDistributionBookmarks() {
    await lazy.PlacesUtils.bookmarks.fetch(
      { guidPrefix: BOOKMARK_GUID_PREFIX },
      bookmark =>
        lazy.PlacesUtils.bookmarks.remove(bookmark).catch(console.error)
    );
    await lazy.PlacesUtils.bookmarks.fetch(
      { guidPrefix: FOLDER_GUID_PREFIX },
      folder => {
        lazy.PlacesUtils.bookmarks.remove(folder).catch(console.error);
      }
    );
  },

  async _parseBookmarksSection(parentGuid, section) {
    let keys = Array.from(this._ini.getKeys(section)).sort();
    let re = /^item\.(\d+)\.(\w+)\.?(\w*)/;
    let items = {};
    let defaultIndex = -1;
    let maxIndex = -1;

    for (let key of keys) {
      let m = re.exec(key);
      if (m) {
        let [, itemIndex, iprop, ilocale] = m;
        itemIndex = parseInt(itemIndex);

        if (ilocale) {
          continue;
        }

        if (keys.includes(key + "." + this._locale)) {
          key += "." + this._locale;
        } else if (keys.includes(key + "." + this._language)) {
          key += "." + this._language;
        }

        if (!items[itemIndex]) {
          items[itemIndex] = {};
        }
        items[itemIndex][iprop] = this._ini.getString(section, key);

        if (iprop == "type" && items[itemIndex].type == "default") {
          defaultIndex = itemIndex;
        }

        if (maxIndex < itemIndex) {
          maxIndex = itemIndex;
        }
      } else {
        dump(`Key did not match: ${key}\n`);
      }
    }

    let prependIndex = 0;
    for (let itemIndex = 0; itemIndex <= maxIndex; itemIndex++) {
      if (!items[itemIndex]) {
        continue;
      }

      let index = lazy.PlacesUtils.bookmarks.DEFAULT_INDEX;
      let item = items[itemIndex];

      switch (item.type) {
        case "default":
          break;

        case "folder": {
          if (itemIndex < defaultIndex) {
            index = prependIndex++;
          }

          let folder = await lazy.PlacesUtils.bookmarks.insert({
            type: lazy.PlacesUtils.bookmarks.TYPE_FOLDER,
            guid: lazy.PlacesUtils.generateGuidWithPrefix(FOLDER_GUID_PREFIX),
            parentGuid,
            index,
            title: item.title,
          });

          await this._parseBookmarksSection(
            folder.guid,
            "BookmarksFolder-" + item.folderId
          );
          break;
        }

        case "separator":
          if (itemIndex < defaultIndex) {
            index = prependIndex++;
          }

          await lazy.PlacesUtils.bookmarks.insert({
            type: lazy.PlacesUtils.bookmarks.TYPE_SEPARATOR,
            parentGuid,
            index,
          });
          break;

        case "livemark":
          if (!item.siteLink) {
            break;
          }
          if (itemIndex < defaultIndex) {
            index = prependIndex++;
          }

          await lazy.PlacesUtils.bookmarks.insert({
            parentGuid,
            index,
            title: item.title,
            url: item.siteLink,
          });
          break;

        case "bookmark":
        default:
          if (itemIndex < defaultIndex) {
            index = prependIndex++;
          }

          await lazy.PlacesUtils.bookmarks.insert({
            guid: lazy.PlacesUtils.generateGuidWithPrefix(BOOKMARK_GUID_PREFIX),
            parentGuid,
            index,
            title: item.title,
            url: item.link,
          });

          if (item.icon && item.iconData) {
            try {
              lazy.PlacesUtils.favicons
                .setFaviconForPage(
                  Services.io.newURI(item.link),
                  Services.io.newURI(item.icon),
                  Services.io.newURI(item.iconData)
                )
                .catch(console.error);
            } catch (e) {
              console.error(e);
            }
          }

          break;
      }
    }
  },

  _newProfile: false,
  _customizationsApplied: false,
  applyCustomizations: function DIST_applyCustomizations() {
    this._customizationsApplied = true;

    if (!Services.prefs.prefHasUserValue("browser.migration.version")) {
      this._newProfile = true;
    }

    if (!this._ini) {
      return this._checkCustomizationComplete();
    }

    if (!this._prefDefaultsApplied) {
      this.applyPrefDefaults();
    }
  },

  _bookmarksApplied: false,
  async applyBookmarks() {
    let prefs = Services.prefs
      .getChildList("distribution.yandex")
      .concat(Services.prefs.getChildList("distribution.mailru"))
      .concat(Services.prefs.getChildList("distribution.okru"));
    if (prefs.length) {
      for (let pref of prefs) {
        Services.prefs.clearUserPref(pref);
      }
      await this._removeDistributionBookmarks();
    } else {
      await this._doApplyBookmarks();
    }
    this._bookmarksApplied = true;
    this._checkCustomizationComplete();
  },

  async _doApplyBookmarks() {
    if (!this._ini) {
      return;
    }

    let sections = enumToObject(this._ini.getSections());

    if (!sections.Global) {
      return;
    }

    let globalPrefs = enumToObject(this._ini.getKeys("Global"));
    if (!(globalPrefs.id && globalPrefs.version && globalPrefs.about)) {
      return;
    }

    let bmProcessedPref;
    try {
      bmProcessedPref = this._ini.getString(
        "Global",
        "bookmarks.initialized.pref"
      );
    } catch (e) {
      bmProcessedPref =
        "distribution." +
        this._ini.getString("Global", "id") +
        ".bookmarksProcessed";
    }

    if (Services.prefs.getBoolPref(bmProcessedPref, false)) {
      return;
    }

    let { ProfileAge } = ChromeUtils.importESModule(
      "resource://gre/modules/ProfileAge.sys.mjs"
    );
    let profileAge = await ProfileAge();
    let resetDate = await profileAge.reset;

    if (!resetDate) {
      if (sections.BookmarksMenu) {
        await this._parseBookmarksSection(
          lazy.PlacesUtils.bookmarks.menuGuid,
          "BookmarksMenu"
        );
      }
      if (sections.BookmarksToolbar) {
        await this._parseBookmarksSection(
          lazy.PlacesUtils.bookmarks.toolbarGuid,
          "BookmarksToolbar"
        );
      }
    }
    Services.prefs.setBoolPref(bmProcessedPref, true);
  },

  _prefDefaultsApplied: false,
  applyPrefDefaults: function DIST_applyPrefDefaults() {
    this._prefDefaultsApplied = true;
    if (!this._ini) {
      return this._checkCustomizationComplete();
    }

    let sections = enumToObject(this._ini.getSections());

    if (!sections.Global) {
      return this._checkCustomizationComplete();
    }
    let globalPrefs = enumToObject(this._ini.getKeys("Global"));
    if (!(globalPrefs.id && globalPrefs.version)) {
      return this._checkCustomizationComplete();
    }
    let distroID = this._ini.getString("Global", "id");
    if (!globalPrefs.about && !distroID.startsWith("mozilla-")) {
      return this._checkCustomizationComplete();
    }

    if (
      distroID == "MozillaOnline" &&
      Services.prefs.getBoolPref("distribution.mozillaonline.ignore", false)
    ) {
      this.__defineGetter__("_ini", () => null);
      return this._checkCustomizationComplete();
    }

    let defaults = Services.prefs.getDefaultBranch(null);


    defaults.setStringPref("distribution.id", distroID);

    if (
      distroID.startsWith("yandex") ||
      distroID.startsWith("mailru") ||
      distroID.startsWith("okru")
    ) {
      this.__defineGetter__("_ini", () => null);
      return this._checkCustomizationComplete();
    }

    defaults.setStringPref(
      "distribution.version",
      this._ini.getString("Global", "version")
    );

    let partnerAbout;
    try {
      if (globalPrefs["about." + this._locale]) {
        partnerAbout = this._ini.getString("Global", "about." + this._locale);
      } else if (globalPrefs["about." + this._language]) {
        partnerAbout = this._ini.getString("Global", "about." + this._language);
      } else {
        partnerAbout = this._ini.getString("Global", "about");
      }
      defaults.setStringPref("distribution.about", partnerAbout);
    } catch (e) {
    }


    let preferences = new Map();

    if (sections.Preferences) {
      for (let key of this._ini.getKeys("Preferences")) {
        let value = this._ini.getString("Preferences", key);
        if (value) {
          preferences.set(key, value);
        }
      }
    }

    if (sections["Preferences-" + this._language]) {
      for (let key of this._ini.getKeys("Preferences-" + this._language)) {
        let value = this._ini.getString("Preferences-" + this._language, key);
        if (value) {
          preferences.set(key, value);
        } else {
          preferences.delete(key);
        }
      }
    }

    if (sections["Preferences-" + this._locale]) {
      for (let key of this._ini.getKeys("Preferences-" + this._locale)) {
        let value = this._ini.getString("Preferences-" + this._locale, key);
        if (value) {
          preferences.set(key, value);
        } else {
          preferences.delete(key);
        }
      }
    }

    applyPrefsToDefaults(preferences, this._locale);

    let localizablePreferences = new Map();

    if (sections.LocalizablePreferences) {
      for (let key of this._ini.getKeys("LocalizablePreferences")) {
        let value = this._ini.getString("LocalizablePreferences", key);
        if (value) {
          localizablePreferences.set(key, value);
        }
      }
    }

    this._localizablePreferences = localizablePreferences;
    applyPrefsToDefaults(localizablePreferences, this._locale);

    return this._checkCustomizationComplete();
  },

  _checkCustomizationComplete: function DIST__checkCustomizationComplete() {
    const BROWSER_DOCURL = AppConstants.BROWSER_CHROME_URL;

    if (this._newProfile) {
      try {
        var showPersonalToolbar = Services.prefs.getBoolPref(
          "browser.showPersonalToolbar"
        );
        if (showPersonalToolbar) {
          Services.prefs.setCharPref(
            "browser.toolbars.bookmarks.visibility",
            "always"
          );
        }
      } catch (e) {}
      try {
        var showMenubar = Services.prefs.getBoolPref("browser.showMenubar");
        if (showMenubar) {
          Services.xulStore.setValue(
            BROWSER_DOCURL,
            "toolbar-menubar",
            "autohide",
            "false"
          );
        }
      } catch (e) {}
    }

    let prefDefaultsApplied = this._prefDefaultsApplied || !this._ini;
    if (this._customizationsApplied && prefDefaultsApplied) {
      Services.obs.notifyObservers(
        null,
        DISTRIBUTION_PREFERENCES_COMPLETE_TOPIC
      );

      if (this._bookmarksApplied) {
        Services.obs.notifyObservers(
          null,
          DISTRIBUTION_CUSTOMIZATION_COMPLETE_TOPIC
        );
      }
    }
  },
};

function applyPrefsToDefaults(
  prefs,
  locale = Services.locale.requestedLocale || "en-US"
) {
  let language = locale.split("-")[0];
  let defaults = Services.prefs.getDefaultBranch(null);
  for (let [prefName, prefValue] of prefs) {
    prefValue = parseValue(prefValue);
    if (typeof prefValue == "string") {
      prefValue = prefValue.replace(/%LOCALE%/g, locale);
      prefValue = prefValue.replace(/%LANGUAGE%/g, language);
    }
    try {
      if (prefName == "general.useragent.locale") {
        defaults.setStringPref("intl.locale.requested", prefValue);
      } else {
        switch (typeof prefValue) {
          case "boolean":
            defaults.setBoolPref(prefName, prefValue);
            break;
          case "number":
            defaults.setIntPref(prefName, prefValue);
            break;
          case "string":
            defaults.setStringPref(prefName, prefValue);
            break;
        }
      }
    } catch (e) {
    }
  }
}

function parseValue(value) {
  try {
    value = JSON.parse(value);
  } catch (e) {
    value = value.replace(/^"/, "");
    value = value.replace(/"$/, "");
  }
  return value;
}

function enumToObject(UTF8Enumerator) {
  let ret = {};
  for (let i of UTF8Enumerator) {
    ret[i] = 1;
  }
  return ret;
}

export let DistributionManagement = {
  _distributionCustomizer: null,
  _localizablePreferences: null,
  get BOOKMARK_GUID_PREFIX() {
    return BOOKMARK_GUID_PREFIX;
  },
  get FOLDER_GUID_PREFIX() {
    return FOLDER_GUID_PREFIX;
  },

  _ensureCustomizer() {
    if (this._distributionCustomizer) {
      return;
    }
    this._distributionCustomizer = new DistributionCustomizer();
    Services.obs.addObserver(this, DISTRIBUTION_CUSTOMIZATION_COMPLETE_TOPIC);
  },

  observe(_subject, topic) {
    if (topic == DISTRIBUTION_CUSTOMIZATION_COMPLETE_TOPIC) {
      Services.obs.removeObserver(
        this,
        DISTRIBUTION_CUSTOMIZATION_COMPLETE_TOPIC
      );
      this._distributionCustomizer = null;
    } else if (topic == "intl:requested-locales-changed") {
      applyPrefsToDefaults(this._localizablePreferences);
    }
  },

  applyCustomizations() {
    this._ensureCustomizer();
    this._distributionCustomizer.applyCustomizations();
    let localizablePreferences =
      this._distributionCustomizer._localizablePreferences;
    if (localizablePreferences?.size) {
      this._localizablePreferences = localizablePreferences;
      Services.obs.addObserver(this, "intl:requested-locales-changed");
    }
  },

  applyBookmarks() {
    this._ensureCustomizer();
    this._distributionCustomizer.applyBookmarks();
  },

  QueryInterface: ChromeUtils.generateQI([Ci.nsIObserver]),
};
