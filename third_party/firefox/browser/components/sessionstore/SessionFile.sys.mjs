/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  sessionStoreLogger: "resource:///modules/sessionstore/SessionLogger.sys.mjs",
  RunState: "resource:///modules/sessionstore/RunState.sys.mjs",
  SessionStore: "resource:///modules/sessionstore/SessionStore.sys.mjs",
  SessionWriter: "resource:///modules/sessionstore/SessionWriter.sys.mjs",
});

const PREF_UPGRADE_BACKUP = "browser.sessionstore.upgradeBackup.latestBuildID";
const PREF_MAX_UPGRADE_BACKUPS =
  "browser.sessionstore.upgradeBackup.maxUpgradeBackups";

const PREF_MAX_SERIALIZE_BACK = "browser.sessionstore.max_serialize_back";
const PREF_MAX_SERIALIZE_FWD = "browser.sessionstore.max_serialize_forward";

export var SessionFile = {
  read() {
    return SessionFileInternal.read();
  },
  write(aData) {
    return SessionFileInternal.write(aData);
  },
  wipe() {
    return SessionFileInternal.wipe();
  },

  get Paths() {
    return SessionFileInternal.Paths;
  },
};

Object.freeze(SessionFile);

const profileDir = PathUtils.profileDir;

var SessionFileInternal = {
  Paths: Object.freeze({
    clean: PathUtils.join(profileDir, "sessionstore.jsonlz4"),

    cleanBackup: PathUtils.join(
      profileDir,
      "sessionstore-backups",
      "previous.jsonlz4"
    ),

    backups: PathUtils.join(profileDir, "sessionstore-backups"),

    recovery: PathUtils.join(
      profileDir,
      "sessionstore-backups",
      "recovery.jsonlz4"
    ),

    recoveryBackup: PathUtils.join(
      profileDir,
      "sessionstore-backups",
      "recovery.baklz4"
    ),

    upgradeBackupPrefix: PathUtils.join(
      profileDir,
      "sessionstore-backups",
      "upgrade.jsonlz4-"
    ),

    get upgradeBackup() {
      let latestBackupID = SessionFileInternal.latestUpgradeBackupID;
      if (!latestBackupID) {
        return "";
      }
      return this.upgradeBackupPrefix + latestBackupID;
    },

    get nextUpgradeBackup() {
      return this.upgradeBackupPrefix + Services.appinfo.platformBuildID;
    },

    get loadOrder() {
      let order = ["clean", "recovery", "recoveryBackup", "cleanBackup"];
      if (SessionFileInternal.latestUpgradeBackupID) {
        order.push("upgradeBackup");
      }
      return order;
    },
  }),

  _attempts: 0,

  _successes: 0,

  _failures: 0,

  _initialized: false,

  _readOrigin: null,

  _usingOldExtension: false,

  get latestUpgradeBackupID() {
    try {
      return Services.prefs.getCharPref(PREF_UPGRADE_BACKUP);
    } catch (ex) {
      return undefined;
    }
  },

  async _readInternal(useOldExtension) {
    let result;
    let noFilesFound = true;
    this._usingOldExtension = useOldExtension;

    for (let key of this.Paths.loadOrder) {
      let corrupted = false;
      let exists = true;
      try {
        let path;
        let startMs = Date.now();

        let options = {};
        if (useOldExtension) {
          path = this.Paths[key]
            .replace("jsonlz4", "js")
            .replace("baklz4", "bak");
        } else {
          path = this.Paths[key];
          options.decompress = true;
        }
        let source = await IOUtils.readUTF8(path, options);
        let parsed = JSON.parse(source);

        if (parsed._cachedObjs) {
          try {
            let cacheMap = new Map(parsed._cachedObjs);
            for (let win of parsed.windows.concat(
              parsed._closedWindows || []
            )) {
              for (let tab of win.tabs.concat(win._closedTabs || [])) {
                tab.image = cacheMap.get(tab.image) || tab.image;
              }
            }
          } catch (e) {
            lazy.sessionStoreLogger.error(e);
          }
        }

        if (
          !lazy.SessionStore.isFormatVersionCompatible(
            parsed.version || [
              "sessionrestore",
              0,
            ] 
          )
        ) {
          lazy.sessionStoreLogger.warn(
            "Cannot extract data from Session Restore file ",
            path,
            ". Wrong format/version: " + JSON.stringify(parsed.version) + "."
          );
          continue;
        }
        result = {
          origin: key,
          source,
          parsed,
          useOldExtension,
        };
        lazy.sessionStoreLogger.debug(`Successful file read of ${key} file`);
        break;
      } catch (ex) {
        if (DOMException.isInstance(ex) && ex.name == "NotFoundError") {
          exists = false;
          lazy.sessionStoreLogger.debug(
            `Can't read session file which doesn't exist: ${key}`
          );
        } else if (
          DOMException.isInstance(ex) &&
          ex.name == "NotReadableError"
        ) {
          lazy.sessionStoreLogger.error(
            `NotReadableError when reading session file: ${key}`,
            ex
          );
          corrupted = true;
        } else if (
          DOMException.isInstance(ex) &&
          ex.name == "NotAllowedError"
        ) {
          lazy.sessionStoreLogger.error(
            `NotAllowedError when reading session file: ${key}`,
            ex
          );
          corrupted = true;
        } else if (ex instanceof SyntaxError) {
          lazy.sessionStoreLogger.error(
            "Corrupt session file (invalid JSON found) ",
            ex,
            ex.stack
          );
          corrupted = true;
        }
      } finally {
        if (exists) {
          noFilesFound = false;
        }
      }
    }
    return { result, noFilesFound };
  },

  async read() {
    let { result, noFilesFound } = await this._readInternal(false);
    if (!result) {
      let r = await this._readInternal(true);
      result = r.result;
    }

    let allCorrupt = !noFilesFound && !result;

    if (!result) {
      lazy.sessionStoreLogger.warn(
        "No readable session files found to restore, starting with empty session"
      );
      result = {
        origin: "empty",
        source: "",
        parsed: null,
        useOldExtension: false,
      };
    }
    this._readOrigin = result.origin;

    result.noFilesFound = noFilesFound;

    return result;
  },

  getWriter() {
    if (!this._initialized) {
      if (!this._readOrigin) {
        return Promise.reject(
          "SessionFileInternal.getWriter() called too early! Please read the session file from disk first."
        );
      }

      this._initialized = true;
      lazy.SessionWriter.init(
        this._readOrigin,
        this._usingOldExtension,
        this.Paths,
        {
          maxUpgradeBackups: Services.prefs.getIntPref(
            PREF_MAX_UPGRADE_BACKUPS,
            3
          ),
          maxSerializeBack: Services.prefs.getIntPref(
            PREF_MAX_SERIALIZE_BACK,
            10
          ),
          maxSerializeForward: Services.prefs.getIntPref(
            PREF_MAX_SERIALIZE_FWD,
            -1
          ),
        }
      );
    }

    return Promise.resolve(lazy.SessionWriter);
  },

  write(aData) {
    if (lazy.RunState.isClosed) {
      return Promise.reject(new Error("SessionFile is closed"));
    }

    let isFinalWrite = false;
    if (lazy.RunState.isClosing) {
      isFinalWrite = true;
      lazy.RunState.setClosed();
    }

    let performShutdownCleanup =
      isFinalWrite && !lazy.SessionStore.willAutoRestore;

    this._attempts++;
    let options = { isFinalWrite, performShutdownCleanup };
    let promise = this.getWriter().then(writer => writer.write(aData, options));

    promise = promise.then(
      msg => {
        this._successes++;
        if (msg.result.upgradeBackup) {
          Services.prefs.setCharPref(
            PREF_UPGRADE_BACKUP,
            Services.appinfo.platformBuildID
          );
        }
      },
      err => {
        lazy.sessionStoreLogger.error(
          "Could not write session state file ",
          err,
          err.stack
        );
        this._failures++;
      }
    );

    IOUtils.profileBeforeChange.addBlocker(
      "SessionFile: Finish writing Session Restore data",
      promise,
      {
        fetchState: () => ({
          options,
          attempts: this._attempts,
          successes: this._successes,
          failures: this._failures,
        }),
      }
    );

    return promise.then(() => {
      IOUtils.profileBeforeChange.removeBlocker(promise);

      if (isFinalWrite) {
        Services.obs.notifyObservers(
          null,
          "sessionstore-final-state-write-complete"
        );
      }
    });
  },

  async wipe() {
    const writer = await this.getWriter();
    await writer.wipe();
    this._initialized = false;
  },
};
