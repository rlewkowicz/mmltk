/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BackupService: "resource:///modules/backup/BackupService.sys.mjs",
  ERRORS: "chrome://browser/content/backup/backup-constants.mjs",
  E10SUtils: "resource://gre/modules/E10SUtils.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "logConsole", function () {
  return console.createInstance({
    prefix: "BackupUIParent",
    maxLogLevel: Services.prefs.getBoolPref("browser.backup.log", false)
      ? "Debug"
      : "Warn",
  });
});

const BACKUP_ERROR_CODE_PREF_NAME = "browser.backup.errorCode";

export class BackupUIParent extends JSWindowActorParent {
  #bs;

  #obs;

  constructor() {
    super();
    this.#bs = lazy.BackupService.init();

    this.#obs = (_subject, topic) => {
      if (topic == "backup-service-status-updated") {
        this.sendState();
      }
    };
  }

  actorCreated() {
    this.#bs.addEventListener("BackupService:StateUpdate", this);
    Services.obs.addObserver(this.#obs, "backup-service-status-updated");
    this.#bs.loadEncryptionState();
  }

  didDestroy() {
    this.#bs.removeEventListener("BackupService:StateUpdate", this);
    Services.obs.removeObserver(this.#obs, "backup-service-status-updated");
  }

  handleEvent(event) {
    if (event.type == "BackupService:StateUpdate") {
      this.sendState();
    }
  }

  async #triggerCreateBackup(...args) {
    try {
      await this.#bs.createBackup(...args);
      return { success: true };
    } catch (e) {
      lazy.logConsole.error(`Failed to retrigger backup`, e);
      return { success: false, errorCode: e.cause || lazy.ERRORS.UNKNOWN };
    }
  }

  async receiveMessage(message) {
    let windowGlobal = this.manager;
    if (
      !windowGlobal ||
      (!windowGlobal.isInProcess &&
        windowGlobal.remoteType != lazy.E10SUtils.PRIVILEGEDABOUT_REMOTE_TYPE)
    ) {
      lazy.logConsole.debug(
        "BackupUIParent: received message from the wrong content process type."
      );
      return null;
    }

    if (message.name == "RequestState") {
      this.sendState();
    } else if (message.name == "TriggerCreateBackup") {
      return await this.#triggerCreateBackup({ reason: "manual" });
    } else if (message.name == "EnableScheduledBackups") {
      try {
        let { password, source } = message.data;

        if (!this.#bs.state.backupDirPath) {
          let defaultPath = lazy.BackupService.DEFAULT_PARENT_DIR_PATH;
          if (defaultPath) {
            await this.#bs.setParentDirPath(defaultPath);
          }
        }

        if (!this.#bs.state.backupDirPath) {
          lazy.logConsole.error(
            "No backup directory path set when enabling scheduled backups."
          );
          return {
            success: false,
            errorCode: lazy.ERRORS.UNKNOWN,
          };
        }

        if (password) {
          if (await this.#bs.loadEncryptionState()) {
            await this.#bs.disableEncryption();
          }
          await this.#bs.enableEncryption(password);
        }
        this.#bs.setScheduledBackups(true, source);
      } catch (e) {
        lazy.logConsole.error(`Failed to enable scheduled backups`, e);
        return { success: false, errorCode: e.cause || lazy.ERRORS.UNKNOWN };
      }

      this.#triggerCreateBackup({ reason: "first" });
      return { success: true };
    } else if (message.name == "DisableScheduledBackups") {
      let { source } = message.data;
      await this.#bs.cleanupBackupFiles();
      this.#bs.setScheduledBackups(false, source);
    } else if (message.name == "ShowFilepicker") {
      let { win, filter, existingBackupPath, alsoDeleteLastBackup } =
        message.data;

      let fp = Cc["@mozilla.org/filepicker;1"].createInstance(Ci.nsIFilePicker);

      let mode = filter
        ? Ci.nsIFilePicker.modeOpen
        : Ci.nsIFilePicker.modeGetFolder;
      fp.init(win || this.browsingContext, "", mode);

      if (filter) {
        fp.appendFilters(Ci.nsIFilePicker[filter]);
      }

      if (existingBackupPath) {
        try {
          let parentPath = PathUtils.parent(existingBackupPath);
          if (await IOUtils.exists(parentPath)) {
            let dir = Cc["@mozilla.org/file/local;1"].createInstance(
              Ci.nsIFile
            );
            dir.initWithPath(parentPath);
            fp.displayDirectory = dir;
          }
        } catch (_) {
        }
      }

      let result = await new Promise(resolve => fp.open(resolve));

      if (result === Ci.nsIFilePicker.returnCancel) {
        return null;
      }

      let path = fp.file.path;
      let iconURL = this.#bs.getIconFromFilePath(path);
      let filename = PathUtils.filename(path);

      if (!filter) {
        if (alsoDeleteLastBackup) {
          try {
            await this.#bs.deleteLastBackup();
          } catch (e) {
            lazy.logConsole.error(
              "Error deleting last backup while editing the backup location.",
              e
            );
          }
        }
        await this.#bs.setParentDirPath(path);
      }

      return {
        path,
        filename,
        iconURL,
      };
    } else if (message.name == "GetBackupFileInfo") {
      let { backupFile } = message.data;
      try {
        await this.#bs.getBackupFileInfo(backupFile);
      } catch (e) {
      }
    } else if (message.name == "FindBackupsInWellKnownLocations") {
      let { source } = message.data;
      await this.#bs.findBackupsInWellKnownLocations({
        source,
      });
    } else if (message.name == "ProbeDefaultBackupDir") {
      let readAccessGranted = await this.#bs.probeDefaultDirAccess(
        message.data?.parentDirPath
      );
      return { readAccessGranted };
    } else if (message.name == "PrepareRestoreDialog") {
      let { source } = message.data;
      let result = await this.#bs.findBackupsInWellKnownLocations({
        source,
      });
      return result;
    } else if (message.name == "RestoreFromBackupChooseFile") {
      const window = this.browsingContext.topChromeWindow;
      this.#bs.filePickerForRestore(window);
    } else if (message.name == "RestoreFromBackupFile") {
      let { backupPassword, restoreType, source } = message.data;
      let backupFile = this.#bs.state.backupFileToRestore;
      if (!backupFile) {
        lazy.logConsole.error("No backup file to restore from in state.");
        return { success: false, errorCode: lazy.ERRORS.UNKNOWN };
      }
      try {
        await this.#bs.recoverFromBackupArchive(
          backupFile,
          backupPassword,
          true ,
          undefined,
          undefined,
          restoreType === "replace" ,
          source
        );
      } catch (e) {
        lazy.logConsole.error(`Failed to restore file: ${backupFile}`, e);
        this.#bs.setRecoveryError(e.cause || lazy.ERRORS.UNKNOWN);
        return { success: false, errorCode: e.cause || lazy.ERRORS.UNKNOWN };
      }
      return { success: true };
    } else if (message.name == "EnableEncryption") {
      try {
        let wasEncrypted = this.#bs.state.encryptionEnabled;
        await this.#bs.enableEncryption(message.data.password);
        if (wasEncrypted) {
        } else {
        }
      } catch (e) {
        lazy.logConsole.error(`Failed to enable encryption`, e);
        return { success: false, errorCode: e.cause || lazy.ERRORS.UNKNOWN };
      }

      return await this.#triggerCreateBackup({ reason: "encryption" });
    } else if (message.name == "DisableEncryption") {
      try {
        await this.#bs.disableEncryption();
      } catch (e) {
        lazy.logConsole.error(`Failed to disable encryption`, e);
        return { success: false, errorCode: e.cause || lazy.ERRORS.UNKNOWN };
      }

      return await this.#triggerCreateBackup({ reason: "encryption" });
    } else if (message.name == "ShowBackupLocation") {
      this.#bs.showBackupLocation();
    } else if (message.name == "QuitCurrentProfile") {
      let cancelQuit = Cc["@mozilla.org/supports-PRBool;1"].createInstance(
        Ci.nsISupportsPRBool
      );
      Services.obs.notifyObservers(cancelQuit, "quit-application-requested");
      if (cancelQuit.data) {
        return null;
      }

      try {
        Services.startup.quit(Services.startup.eAttemptQuit);
      } catch (e) {
        lazy.logConsole.error(
          `There was a problem while quitting the current profile: `,
          e
        );
      }
    } else if (message.name == "SetEmbeddedComponentPersistentData") {
      this.#bs.setEmbeddedComponentPersistentData(message.data);
    } else if (message.name == "FlushEmbeddedComponentPersistentData") {
      this.#bs.setEmbeddedComponentPersistentData({});
    } else if (message.name == "ErrorBarDismissed") {
      Services.prefs.setIntPref(BACKUP_ERROR_CODE_PREF_NAME, lazy.ERRORS.NONE);
    }

    return null;
  }

  sendState() {
    this.sendAsyncMessage("StateUpdate", {
      state: this.#bs.state,
    });
  }
}
