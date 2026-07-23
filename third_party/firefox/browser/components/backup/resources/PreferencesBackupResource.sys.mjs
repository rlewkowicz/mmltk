/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { BackupResource } from "resource:///modules/backup/BackupResource.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ExperimentAPI: "resource://nimbus/ExperimentAPI.sys.mjs",
  SearchUtils: "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  SelectableProfileService:
    "resource:///modules/profiles/SelectableProfileService.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "logConsole", function () {
  return console.createInstance({
    prefix: "PreferencesBackupResource",
    maxLogLevel: Services.prefs.getBoolPref("browser.backup.log", false)
      ? "Debug"
      : "Warn",
  });
});

const PROFILE_RESTORATION_DATE_PREF = "browser.backup.profile-restoration-date";
const PROFILES_ENABLED_PREF = "browser.profiles.enabled";
const PROFILES_CREATED_PREF = "browser.profiles.created";
const STOREID_PREF = "toolkit.profiles.storeID";
const WALLPAPER_TYPE_PREF =
  "browser.newtabpage.activity-stream.newtabWallpapers.wallpaper";
const CUSTOM_WALLPAPER_UUID_PREF =
  "browser.newtabpage.activity-stream.newtabWallpapers.customWallpaper.uuid";
const CUSTOM_WALLPAPER_FOLDER = "wallpaper";

export class PreferencesBackupResource extends BackupResource {
  static get key() {
    return "preferences";
  }

  static get requiresEncryption() {
    return false;
  }

  static get dataCollectionPrefs() {
    return [
      "browser.discovery.enabled",
      "app.shield.optoutstudies.enabled",
      "datareporting.healthreport.uploadEnabled",
      "datareporting.usage.uploadEnabled",
    ];
  }

  static addPrefsToIgnoreInBackup(prefsOverrideMap) {
    let kIgnoredPrefs = [
      "app.normandy.user_id",
      "browser.profiles.shortcutFileName",
      PROFILE_RESTORATION_DATE_PREF,
    ];

    const backupPrefs = Services.prefs.getChildList("browser.backup.");
    kIgnoredPrefs = kIgnoredPrefs.concat(backupPrefs);

    for (const pref of kIgnoredPrefs) {
      if (Services.prefs.getPrefType(pref) !== Services.prefs.PREF_INVALID) {
        prefsOverrideMap.addEntry(pref, null);
      }
    }

    const kNimbusMetadataPrefPrefix = "nimbus.";
    const kNimbusPrefExceptionList = ["nimbus.rollouts.enabled"];

    const nimbusPrefs = Services.prefs.getChildList(kNimbusMetadataPrefPrefix);
    for (const pref of nimbusPrefs) {
      if (kNimbusPrefExceptionList.includes(pref)) {
        continue;
      }

      prefsOverrideMap.addEntry(pref, null);
    }

    return prefsOverrideMap;
  }

  static getPrefsFromBuffer(prefsBuffer, prefNames = null) {
    const prefSet = prefNames ? new Set(prefNames) : null;
    const prefs = new Map();

    const addPref = (_kind, name, value) => {
      if (!prefSet || prefSet.has(name)) {
        prefs.set(name, value);
      }
    };

    Services.prefs.parsePrefsFromBuffer(prefsBuffer, {
      onStringPref: addPref,
      onIntPref: addPref,
      onBoolPref: addPref,
      onError(_message) {
      },
    });

    return prefs;
  }

  async backup(
    stagingPath,
    profilePath = PathUtils.profileDir,
    _isEncrypting = false
  ) {
    const simpleCopyFiles = [
      "xulstore.json",
      "containers.json",
      "customKeys.json",
      "handlers.json",
      "search.json.mozlz4",
      "user.js",
      "chrome",
    ];
    await BackupResource.copyFiles(profilePath, stagingPath, simpleCopyFiles);

    const WALLPAPER_TYPE = Services.prefs.getStringPref(
      WALLPAPER_TYPE_PREF,
      ""
    );
    const WALLPAPER_UUID = Services.prefs.getStringPref(
      CUSTOM_WALLPAPER_UUID_PREF,
      ""
    );
    if (WALLPAPER_TYPE == "custom" && WALLPAPER_UUID) {
      await BackupResource.copyFiles(
        PathUtils.join(profilePath, CUSTOM_WALLPAPER_FOLDER),
        PathUtils.join(stagingPath, CUSTOM_WALLPAPER_FOLDER),
        [WALLPAPER_UUID]
      );
    }

    let prefsDestPath = PathUtils.join(stagingPath, "prefs.js");
    let prefsDestFile = await IOUtils.getFile(prefsDestPath);
    await lazy.ExperimentAPI._rsLoader.withUpdateLock(async () => {
      await Services.prefs.backupPrefFile(
        prefsDestFile,
        PreferencesBackupResource.addPrefsToIgnoreInBackup(
          lazy.ExperimentAPI.manager.store.getOriginalPrefValuesForAllActiveEnrollments()
        )
      );
    });

    return { profileDirName: PathUtils.filename(profilePath) };
  }

  async recover(manifestEntry, recoveryPath, destProfilePath) {
    const SEARCH_PREF_FILENAME = "search.json.mozlz4";
    const RECOVERY_SEARCH_PREF_PATH = PathUtils.join(
      recoveryPath,
      SEARCH_PREF_FILENAME
    );

    if (await IOUtils.exists(RECOVERY_SEARCH_PREF_PATH)) {
      let searchPrefs = await IOUtils.readJSON(RECOVERY_SEARCH_PREF_PATH, {
        decompress: true,
      });

      const ORIGINAL_DIR_NAME =
        manifestEntry.profileDirName ??
        (manifestEntry.profilePath
          ? manifestEntry.profilePath.split(/[/\\]/).at(-1)
          : null);

      if (ORIGINAL_DIR_NAME) {
        let destDirName = PathUtils.filename(destProfilePath);

        searchPrefs.engines = searchPrefs.engines.map(engine => {
          if (engine._metaData.loadPathHash) {
            let loadPath = engine._loadPath;
            if (
              engine._metaData.loadPathHash ==
              lazy.SearchUtils.getVerificationHash(loadPath, ORIGINAL_DIR_NAME)
            ) {
              engine._metaData.loadPathHash =
                lazy.SearchUtils.getVerificationHash(loadPath, destDirName);
            }
          }
          return engine;
        });

        if (
          searchPrefs.metaData.defaultEngineIdHash &&
          searchPrefs.metaData.defaultEngineIdHash ==
            lazy.SearchUtils.getVerificationHash(
              searchPrefs.metaData.defaultEngineId,
              ORIGINAL_DIR_NAME
            )
        ) {
          searchPrefs.metaData.defaultEngineIdHash =
            lazy.SearchUtils.getVerificationHash(
              searchPrefs.metaData.defaultEngineId,
              destDirName
            );
        }

        if (
          searchPrefs.metaData.privateDefaultEngineIdHash &&
          searchPrefs.metaData.privateDefaultEngineIdHash ==
            lazy.SearchUtils.getVerificationHash(
              searchPrefs.metaData.privateDefaultEngineId,
              ORIGINAL_DIR_NAME
            )
        ) {
          searchPrefs.metaData.privateDefaultEngineIdHash =
            lazy.SearchUtils.getVerificationHash(
              searchPrefs.metaData.privateDefaultEngineId,
              destDirName
            );
        }
      }

      await IOUtils.writeJSON(
        PathUtils.join(destProfilePath, SEARCH_PREF_FILENAME),
        searchPrefs,
        { compress: true }
      );
    }

    const simpleCopyFiles = [
      "prefs.js",
      "xulstore.json",
      "containers.json",
      "customKeys.json",
      "handlers.json",
      "user.js",
      "chrome",
      CUSTOM_WALLPAPER_FOLDER,
    ];
    await BackupResource.copyFiles(
      recoveryPath,
      destProfilePath,
      simpleCopyFiles
    );

    const LINEBREAK = AppConstants.platform === "win" ? "\r\n" : "\n";
    let prefsFile = await IOUtils.getFile(destProfilePath);
    prefsFile.append("prefs.js");
    const includePreamble = !(await IOUtils.exists(prefsFile.path));
    let addToPrefsJs = includePreamble ? Services.prefs.prefsJsPreamble : "";

    addToPrefsJs += `user_pref("${PROFILE_RESTORATION_DATE_PREF}", ${Math.round(Date.now() / 1000)});${LINEBREAK}`;

    await IOUtils.writeUTF8(prefsFile.path, addToPrefsJs, {
      mode: "appendOrCreate",
    });

    if (!lazy.SelectableProfileService.isEnabled) {
      let setToLegacyPrefs =
        `user_pref("${PROFILES_ENABLED_PREF}", ${Services.prefs.getBoolPref(PROFILES_ENABLED_PREF, false)});${LINEBREAK}` +
        `user_pref("${PROFILES_CREATED_PREF}", ${Services.prefs.getBoolPref(PROFILES_CREATED_PREF, false)});${LINEBREAK}` +
        `user_pref("${STOREID_PREF}", "");${LINEBREAK}`;

      await IOUtils.writeUTF8(prefsFile.path, setToLegacyPrefs, {
        mode: "appendOrCreate",
      });
    } else if (lazy.SelectableProfileService.currentProfile) {
      lazy.logConsole.debug(
        `We're recovering into a profile group, let's make sure to set the right selectable profile prefs`
      );

      const dataCollectionPrefs = PreferencesBackupResource.dataCollectionPrefs;

      const prefsFilePath = PathUtils.join(recoveryPath, "prefs.js");
      const prefsBuffer = await IOUtils.read(prefsFilePath);
      const backupPrefs = PreferencesBackupResource.getPrefsFromBuffer(
        prefsBuffer,
        dataCollectionPrefs
      );
      const defaults = Services.prefs.getDefaultBranch(null);

      for (let pref of dataCollectionPrefs) {
        let groupPrefValue =
          await lazy.SelectableProfileService.getDBPref(pref);
        let backupPrefValue = backupPrefs.has(pref)
          ? backupPrefs.get(pref)
          : defaults.getBoolPref(pref, false);

        if (groupPrefValue && !backupPrefValue) {
          Services.prefs.setBoolPref(pref, false);
        }
      }

      await lazy.SelectableProfileService.addSelectableProfilePrefs(
        destProfilePath
      );
    }

    return null;
  }

}
