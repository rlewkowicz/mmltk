/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import * as DefaultBackupResources from "resource:///modules/backup/BackupResources.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import { BackupResource } from "resource:///modules/backup/BackupResource.sys.mjs";
import {
  MeasurementUtils,
  BYTES_IN_KILOBYTE,
  BYTES_IN_MEBIBYTE,
} from "resource:///modules/backup/MeasurementUtils.sys.mjs";

import {
  ERRORS,
  BACKUP_STEPS,
  RESTORE_STEPS,
  errorString,
} from "chrome://browser/content/backup/backup-constants.mjs";
import { BackupError } from "resource:///modules/backup/BackupError.mjs";

const BACKUP_DIR_PREF_NAME = "browser.backup.location";
const BACKUP_ERROR_CODE_PREF_NAME = "browser.backup.errorCode";
const SCHEDULED_BACKUPS_ENABLED_PREF_NAME = "browser.backup.scheduled.enabled";
const BACKUP_ARCHIVE_ENABLED_PREF_NAME = "browser.backup.archive.enabled";
const BACKUP_RESTORE_ENABLED_PREF_NAME = "browser.backup.restore.enabled";
const IDLE_THRESHOLD_SECONDS_PREF_NAME =
  "browser.backup.scheduled.idle-threshold-seconds";
const MINIMUM_TIME_BETWEEN_BACKUPS_SECONDS_PREF_NAME =
  "browser.backup.scheduled.minimum-time-between-backups-seconds";
const LAST_BACKUP_TIMESTAMP_PREF_NAME =
  "browser.backup.scheduled.last-backup-timestamp";
const LAST_BACKUP_FILE_NAME_PREF_NAME =
  "browser.backup.scheduled.last-backup-file";
const BACKUP_RETRY_LIMIT_PREF_NAME = "browser.backup.backup-retry-limit";
const DISABLED_ON_IDLE_RETRY_PREF_NAME =
  "browser.backup.disabled-on-idle-backup-retry";
const BACKUP_DEBUG_INFO_PREF_NAME = "browser.backup.backup-debug-info";
const MAXIMUM_NUMBER_OF_UNREMOVABLE_STAGING_ITEMS_PREF_NAME =
  "browser.backup.max-num-unremovable-staging-items";
const CREATED_MANAGED_PROFILES_PREF_NAME = "browser.profiles.created";
const SANITIZE_ON_SHUTDOWN_PREF_NAME = "privacy.sanitize.sanitizeOnShutdown";
const BACKUP_ENABLED_ON_PROFILES_PREF_NAME =
  "browser.backup.enabled_on.profiles";
const SQLITE_ENCRYPTION_ENABLED_PREF_NAME =
  "security.storage.encryption.sqlite.enabled";

const SCHEMAS = Object.freeze({
  BACKUP_MANIFEST: 1,
  ARCHIVE_JSON_BLOCK: 2,
});

const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "logConsole", function () {
  return console.createInstance({
    prefix: "BackupService",
    maxLogLevel: Services.prefs.getBoolPref("browser.backup.log", false)
      ? "Debug"
      : "Warn",
  });
});

ChromeUtils.defineLazyGetter(lazy, "fxAccounts", () => {
  return ChromeUtils.importESModule(
    "resource://gre/modules/FxAccounts.sys.mjs"
  ).getFxAccountsSingleton();
});

ChromeUtils.defineESModuleGetters(lazy, {
  ArchiveDecryptor: "resource:///modules/backup/ArchiveEncryption.sys.mjs",
  ArchiveEncryptionState:
    "resource:///modules/backup/ArchiveEncryptionState.sys.mjs",
  ArchiveUtils: "resource:///modules/backup/ArchiveUtils.sys.mjs",
  BasePromiseWorker: "resource://gre/modules/PromiseWorker.sys.mjs",
  DeferredTask: "resource://gre/modules/DeferredTask.sys.mjs",
  DownloadPaths: "resource://gre/modules/DownloadPaths.sys.mjs",
  FileUtils: "resource://gre/modules/FileUtils.sys.mjs",
  JsonSchema: "resource://gre/modules/JsonSchema.sys.mjs",
  NetUtil: "resource://gre/modules/NetUtil.sys.mjs",
  NimbusFeatures: "resource://nimbus/ExperimentAPI.sys.mjs",
  ProfileAge: "resource://gre/modules/ProfileAge.sys.mjs",
  SelectableProfileService:
    "resource:///modules/profiles/SelectableProfileService.sys.mjs",
  UIState: "resource://services-sync/UIState.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "ZipWriter", () =>
  Components.Constructor("@mozilla.org/zipwriter;1", "nsIZipWriter", "open")
);
ChromeUtils.defineLazyGetter(lazy, "ZipReader", () =>
  Components.Constructor(
    "@mozilla.org/libjar/zip-reader;1",
    "nsIZipReader",
    "open"
  )
);
ChromeUtils.defineLazyGetter(lazy, "nsLocalFile", () =>
  Components.Constructor("@mozilla.org/file/local;1", "nsIFile", "initWithPath")
);

ChromeUtils.defineLazyGetter(lazy, "BinaryInputStream", () =>
  Components.Constructor(
    "@mozilla.org/binaryinputstream;1",
    "nsIBinaryInputStream",
    "setInputStream"
  )
);

ChromeUtils.defineLazyGetter(lazy, "gFluentStrings", function () {
  return new Localization(
    ["branding/brand.ftl", "browser/backupSettings.ftl"],
    true
  );
});

ChromeUtils.defineLazyGetter(lazy, "gDOMLocalization", function () {
  return new DOMLocalization([
    "branding/brand.ftl",
    "browser/backupSettings.ftl",
  ]);
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "scheduledBackupsPref",
  SCHEDULED_BACKUPS_ENABLED_PREF_NAME,
  false,
  function onUpdateScheduledBackups(_pref, _prevVal, newVal) {
    let bs = BackupService.get();
    if (bs) {
      bs.onUpdateScheduledBackups(newVal);
    }
  }
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "backupDirPref",
  BACKUP_DIR_PREF_NAME,
  null,
  async function onUpdateLocationDirPath(_pref, _prevVal, newVal) {
    let bs;
    try {
      bs = BackupService.get();
    } catch (e) {
    }
    if (bs) {
      await bs.onUpdateLocationDirPath(newVal);
    }
  }
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "minimumTimeBetweenBackupsSeconds",
  MINIMUM_TIME_BETWEEN_BACKUPS_SECONDS_PREF_NAME,
  86400 
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "backupRetryLimit",
  BACKUP_RETRY_LIMIT_PREF_NAME,
  100
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "isRetryDisabledOnIdle",
  DISABLED_ON_IDLE_RETRY_PREF_NAME,
  false
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "maximumNumberOfUnremovableStagingItems",
  MAXIMUM_NUMBER_OF_UNREMOVABLE_STAGING_ITEMS_PREF_NAME,
  5
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "enabledOnProfilesPref",
  BACKUP_ENABLED_ON_PROFILES_PREF_NAME,
  "[]",
  null,
  function transform(rawValue) {
    let parsed;
    try {
      parsed = JSON.parse(rawValue);
    } catch {
      return [];
    }

    if (Array.isArray(parsed)) {
      return parsed;
    }

    let profilesArray = Object.keys(parsed);
    Services.prefs.setStringPref(
      BACKUP_ENABLED_ON_PROFILES_PREF_NAME,
      JSON.stringify(profilesArray)
    );
    return profilesArray;
  }
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "backupErrorCode",
  BACKUP_ERROR_CODE_PREF_NAME,
  0,
  function onUpdateBackupErrorCode(_pref, _prevVal, newVal) {
    let bs = BackupService.get();
    if (bs) {
      bs.onUpdateBackupErrorCode(newVal);
    }
  }
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "lastBackupFileName",
  LAST_BACKUP_FILE_NAME_PREF_NAME,
  "",
  function onUpdateLastBackupFileName(_pref, _prevVal, newVal) {
    let bs;
    try {
      bs = BackupService.get();
    } catch (e) {
    }
    if (bs) {
      bs.onUpdateLastBackupFileName(newVal);
    }
  }
);

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "idleService",
  "@mozilla.org/widget/useridleservice;1",
  Ci.nsIUserIdleService
);

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "nativeOSKeyStore",
  "@mozilla.org/security/oskeystore;1",
  Ci.nsIOSKeyStore
);

class BinaryReadableStream {
  #channel = null;

  constructor(channel) {
    this.#channel = channel;
  }

  start(controller) {
    let streamConv = Cc["@mozilla.org/streamConverters;1"].getService(
      Ci.nsIStreamConverterService
    );

    let textDecoder = new TextDecoder();

    const EXPECTED_CONTENT_TYPE = "application/octet-stream";

    let multipartListenerForBinary = {
      _enabled: false,

      _done: false,

      QueryInterface: ChromeUtils.generateQI([
        "nsIStreamListener",
        "nsIRequestObserver",
        "nsIMultiPartChannelListener",
      ]),

      onStartRequest(request) {
        if (!(request instanceof Ci.nsIChannel)) {
          throw Components.Exception(
            "onStartRequest expected an nsIChannel request",
            Cr.NS_ERROR_UNEXPECTED
          );
        }
        this._enabled = request.contentType == EXPECTED_CONTENT_TYPE;
      },

      onDataAvailable(request, stream, offset, count) {
        if (this._done) {
          throw Components.Exception(
            "Got binary block - cancelling loading the multipart stream.",
            Cr.NS_BINDING_ABORTED
          );
        }
        if (!this._enabled) {
          return;
        }

        let binStream = new lazy.BinaryInputStream(stream);
        let bytes = new Uint8Array(count);
        binStream.readArrayBuffer(count, bytes.buffer);
        let string = textDecoder.decode(bytes);
        controller.enqueue(string);
      },

      onStopRequest() {
        if (this._enabled && !this._done) {
          this._enabled = false;
          this._done = true;

          controller.close();
        }
      },

      onAfterLastPart() {
        if (!this._done) {
          controller.error(
            new BackupError(
              "Could not find binary block.",
              ERRORS.CORRUPTED_ARCHIVE
            )
          );
        }
      },
    };

    let conv = streamConv.asyncConvertData(
      "multipart/mixed",
      "*/*",
      multipartListenerForBinary,
      null
    );

    this.#channel.asyncOpen(conv);
  }
}

export class DecoderDecryptorTransformer {
  #buffer = "";
  #decryptor = null;

  constructor(decryptor) {
    this.#decryptor = decryptor;
  }

  async transform(chunkPart, controller) {
    if (this.#buffer) {
      this.#buffer += chunkPart;
    } else {
      this.#buffer = chunkPart;
    }

    let chunks = this.#buffer.split("\n").filter(chunk => chunk != "");

    this.#buffer = chunks.pop();
    for (let chunk of chunks) {
      await this.#processChunk(controller, chunk);
    }
  }

  async flush(controller) {
    await this.#processChunk(controller, this.#buffer, true);
    this.#buffer = "";
  }

  async #processChunk(controller, chunk, isLastChunk = false) {
    try {
      let bytes = lazy.ArchiveUtils.stringToArray(chunk);

      if (this.#decryptor) {
        let plaintextBytes = await this.#decryptor.decrypt(bytes, isLastChunk);
        controller.enqueue(plaintextBytes);
      } else {
        controller.enqueue(bytes);
      }
    } catch (e) {
      throw e instanceof BackupError
        ? e
        : new BackupError("Corrupted archive", ERRORS.CORRUPTED_ARCHIVE);
    }
  }
}

export class FileWriterStream {
  #destPath = null;

  #outStream = null;

  #binStream = null;

  #decryptor = null;

  constructor(destPath, decryptor) {
    this.#destPath = destPath;
    this.#decryptor = decryptor;
  }

  async start() {
    let extractionDestFile = await IOUtils.getFile(this.#destPath);
    this.#outStream =
      lazy.FileUtils.openSafeFileOutputStream(extractionDestFile);
    this.#binStream = Cc["@mozilla.org/binaryoutputstream;1"].createInstance(
      Ci.nsIBinaryOutputStream
    );
    this.#binStream.setOutputStream(this.#outStream);
  }

  write(chunk) {
    this.#binStream.writeByteArray(chunk);
  }

  close(controller) {
    lazy.FileUtils.closeSafeFileOutputStream(this.#outStream);
    if (this.#decryptor && !this.#decryptor.isDone()) {
      lazy.logConsole.error(
        "Decryptor was not done when the stream was closed."
      );
      controller.error(
        new BackupError("Corrupted archive.", ERRORS.DECRYPTION_FAILED)
      );
    }
  }

  async abort(reason) {
    lazy.logConsole.error(`Writing to ${this.#destPath} failed: `, reason);
    lazy.FileUtils.closeSafeFileOutputStream(this.#outStream);
    await IOUtils.remove(this.#destPath, {
      ignoreAbsent: true,
      retryReadonly: true,
    });
  }
}

export class BackupService extends EventTarget {
  static #instance = null;

  #resources = new Map();

  static #backupFolderName = "Restore Firefox";

  static #backupFileName = null;

  static #errorRetries = 0;

  static backoffSeconds = () => Math.pow(2, BackupService.#errorRetries) * 60;


  get archiveEnabledStatus() {
    if (
      Services.prefs.getBoolPref(SQLITE_ENCRYPTION_ENABLED_PREF_NAME, false)
    ) {
      return {
        enabled: false,
        reason: "Archiving a profile disabled while SQLite encryption is on.",
        internalReason: "sqlite-encryption",
      };
    }

    const archiveKillswitchTriggered =
      lazy.NimbusFeatures.backupService.getVariable("archiveKillswitch");

    if (archiveKillswitchTriggered) {
      return {
        enabled: false,
        reason: "Archiving a profile disabled remotely.",
        internalReason: "nimbus",
      };
    }

    if (!Services.prefs.getBoolPref(BACKUP_ARCHIVE_ENABLED_PREF_NAME)) {
      if (Services.prefs.prefIsLocked(BACKUP_ARCHIVE_ENABLED_PREF_NAME)) {
        return {
          enabled: false,
          reason: "Archiving a profile disabled by policy.",
          internalReason: "policy",
        };
      }

      return {
        enabled: false,
        reason: "Archiving a profile disabled by user pref.",
        internalReason: "pref",
      };
    }

    return { enabled: true };
  }

  get restoreEnabledStatus() {
    if (
      Services.prefs.getBoolPref(SQLITE_ENCRYPTION_ENABLED_PREF_NAME, false)
    ) {
      return {
        enabled: false,
        reason: "Restoring a profile disabled while SQLite encryption is on.",
        internalReason: "sqlite-encryption",
      };
    }

    const restoreKillswitchTriggered =
      lazy.NimbusFeatures.backupService.getVariable("restoreKillswitch");

    if (restoreKillswitchTriggered) {
      return {
        enabled: false,
        reason: "Restore from backup disabled remotely.",
        internalReason: "nimbus",
      };
    }

    if (!Services.prefs.getBoolPref(BACKUP_RESTORE_ENABLED_PREF_NAME)) {
      if (Services.prefs.prefIsLocked(BACKUP_RESTORE_ENABLED_PREF_NAME)) {
        return {
          enabled: false,
          reason: "Restoring a profile disabled by policy.",
          internalReason: "policy",
        };
      }

      return {
        enabled: false,
        reason: "Restoring a profile disabled by user pref.",
        internalReason: "pref",
      };
    }

    return { enabled: true };
  }

  set #backupInProgress(val) {
    if (this.#_state.backupInProgress != val) {
      this.#_state.backupInProgress = val;
      this.stateUpdate();
    }
  }

  get #backupInProgress() {
    return this.#_state.backupInProgress;
  }

  stateUpdate() {
    this.dispatchEvent(new CustomEvent("BackupService:StateUpdate"));
  }

  setRecoveryError(errorCode) {
    this.#_state.recoveryErrorCode = errorCode;
    this.stateUpdate();
  }

  setEmbeddedComponentPersistentData(data) {
    this.#_state.embeddedComponentPersistentData = { ...data };
    this.stateUpdate();
  }

  #_state = {
    backupDirPath: lazy.backupDirPref,
    defaultParent: {},
    backupFileToRestore: null,
    backupFileInfo: null,
    backupInProgress: false,
    scheduledBackupsEnabled: lazy.scheduledBackupsPref,
    encryptionEnabled: false,
    lastBackupDate: null,
    lastBackupFileName: lazy.lastBackupFileName,
    supportBaseLink: Services.urlFormatter.formatURLPref("app.support.baseURL"),
    recoveryInProgress: false,
    embeddedComponentPersistentData: {},
    recoveryErrorCode: ERRORS.NONE,
    backupErrorCode: lazy.backupErrorCode,
    selectableProfilesAllowed: lazy.SelectableProfileService.isEnabled,
  };

  #postRecoveryPromise;

  #postRecoveryResolver;

  #encState = undefined;

  #placesObserver = null;

  #backupWriteAbortController = null;

  #regenerationDebouncer = null;

  #scheduledBackupsToggleSource = "unknown";

  #statusPrefObserver = null;

  #profileServiceStateObserver = null;

  static get DEFAULT_PARENT_DIR_PATH() {
    return (
      BackupService.oneDriveFolderPath?.path ||
      BackupService.docsDirFolderPath?.path ||
      ""
    );
  }

  static get BACKUP_DIR_NAME() {
    if (!BackupService.#backupFolderName) {
      BackupService.#backupFolderName = lazy.DownloadPaths.sanitize(
        lazy.gFluentStrings.formatValueSync("backup-folder-name")
      );
    }
    return BackupService.#backupFolderName;
  }

  static get BACKUP_DIR_PREF_NAME() {
    return BACKUP_DIR_PREF_NAME;
  }

  static get BACKUP_DIR_TRANSLATION() {
    let folderDesc = lazy.gFluentStrings.formatValueSync("backup-folder-name");
    folderDesc =
      folderDesc != "" ? folderDesc : BackupService.#backupFolderName;
    return folderDesc;
  }

  static get BACKUP_FILE_NAME() {
    if (!BackupService.#backupFileName) {
      BackupService.#backupFileName = lazy.DownloadPaths.sanitize(
        lazy.gFluentStrings.formatValueSync("backup-file-name")
      );
    }
    return BackupService.#backupFileName;
  }

  static get PROFILE_FOLDER_NAME() {
    return "backups";
  }

  static get SNAPSHOTS_FOLDER_NAME() {
    return "snapshots";
  }

  static get MANIFEST_FILE_NAME() {
    return "backup-manifest.json";
  }

  static #manifestSchemaPromise = null;

  static get MANIFEST_SCHEMA() {
    if (!BackupService.#manifestSchemaPromise) {
      BackupService.#manifestSchemaPromise = BackupService.getSchemaForVersion(
        SCHEMAS.BACKUP_MANIFEST,
        lazy.ArchiveUtils.SCHEMA_VERSION
      );
    }

    return BackupService.#manifestSchemaPromise;
  }

  static get POST_RECOVERY_FILE_NAME() {
    return "post-recovery.json";
  }

  static get ARCHIVE_ENCRYPTION_STATE_FILE() {
    return "enc-state.json";
  }

  static get SCHEMAS() {
    return SCHEMAS;
  }

  static get RECOVERY_ZIP_FILE_NAME() {
    return "recovery.zip";
  }

  static get STATUS_OBSERVER_PREFS() {
    return [
      BACKUP_ARCHIVE_ENABLED_PREF_NAME,
      BACKUP_RESTORE_ENABLED_PREF_NAME,
      SANITIZE_ON_SHUTDOWN_PREF_NAME,
      CREATED_MANAGED_PROFILES_PREF_NAME,
    ];
  }

  static async getSchemaForVersion(schemaType, version) {
    let schemaURL;

    if (schemaType == SCHEMAS.BACKUP_MANIFEST) {
      schemaURL = `chrome://browser/content/backup/BackupManifest.${version}.schema.json`;
    } else if (schemaType == SCHEMAS.ARCHIVE_JSON_BLOCK) {
      schemaURL = `chrome://browser/content/backup/ArchiveJSONBlock.${version}.schema.json`;
    } else {
      throw new BackupError(
        `Did not recognize SCHEMAS constant: ${schemaType}`,
        ERRORS.UNKNOWN
      );
    }

    let response = await fetch(schemaURL);
    return response.json();
  }

  static get COMPRESSION_LEVEL() {
    return Ci.nsIZipWriter.COMPRESSION_BEST;
  }

  static get ARCHIVE_TEMPLATE() {
    return "chrome://browser/content/backup/archive.template.html";
  }

  static get RECOVERY_OSKEYSTORE_LABEL() {
    return AppConstants.MOZ_APP_BASENAME + " Backup Recovery Storage";
  }

  static get WRITE_BACKUP_LOCK_NAME() {
    return "write-backup";
  }

  static get REGENERATION_DEBOUNCE_RATE_MS() {
    return 10000;
  }

  static get oneDriveFolderPath() {
    try {
      let oneDriveDir = Services.dirsvc.get("OneDrPD", Ci.nsIFile);
      return oneDriveDir.exists() ? oneDriveDir : null;
    } catch {
    }
    return null;
  }

  static get docsDirFolderPath() {
    try {
      return Services.dirsvc.get("Docs", Ci.nsIFile);
    } catch (e) {
      lazy.logConsole.warn(
        "There was an error while trying to get the Document's directory",
        e
      );
    }
    return null;
  }

  async probeDefaultDirAccess(path = BackupService.DEFAULT_PARENT_DIR_PATH) {
    if (!path) {
      return false;
    }
    try {
      await IOUtils.getChildren(path);
      return true;
    } catch {
      return false;
    }
  }

  static init(BackupResources = DefaultBackupResources) {
    if (this.#instance) {
      return this.#instance;
    }

    this.#instance = new BackupService(BackupResources);

    this.#instance.checkForPostRecovery();
    this.#instance.initBackupScheduler();
    this.#instance.initStatusObservers();
    return this.#instance;
  }

  static uninit() {
    if (this.#instance) {
      lazy.logConsole.debug("Uninitting the BackupService");

      this.#instance.uninitBackupScheduler();
      this.#instance.uninitStatusObservers();
      this.#instance = null;
    }
  }

  static get() {
    if (!this.#instance) {
      throw new BackupError(
        "BackupService not initialized",
        ERRORS.UNINITIALIZED
      );
    }
    return this.#instance;
  }

  constructor(backupResources = DefaultBackupResources) {
    super();
    lazy.logConsole.debug("Instantiated");

    for (const resourceName in backupResources) {
      let resource = backupResources[resourceName];
      this.#resources.set(resource.key, resource);
    }

    let { promise, resolve } = Promise.withResolvers();
    this.#postRecoveryPromise = promise;
    this.#postRecoveryResolver = resolve;
    this.#backupWriteAbortController = new AbortController();
    this.#regenerationDebouncer = new lazy.DeferredTask(async () => {
      if (
        !this.#backupWriteAbortController.signal.aborted &&
        this.archiveEnabledStatus.enabled
      ) {
        await this.createBackupOnIdleDispatch({
          reason: "user deleted some data",
        });
      }
    }, BackupService.REGENERATION_DEBOUNCE_RATE_MS);
    this.#lastSeenArchiveStatus = this.archiveEnabledStatus;
    this.#lastSeenRestoreStatus = this.restoreEnabledStatus;
  }

  #lastSeenArchiveStatus = false;
  #lastSeenRestoreStatus = false;

  get postRecoveryComplete() {
    return this.#postRecoveryPromise;
  }


  get state() {
    if (
      !Object.keys(this.#_state.defaultParent).length ||
      !this.#_state.defaultParent.path
    ) {
      let defaultPath = BackupService.DEFAULT_PARENT_DIR_PATH;
      this.#_state.defaultParent = {
        path: defaultPath,
        fileName: defaultPath ? PathUtils.filename(defaultPath) : "",
        iconURL: defaultPath ? this.getIconFromFilePath(defaultPath) : "",
      };
    }

    return Object.freeze(structuredClone(this.#_state));
  }

  async resolveArchiveDestFolderPath(configuredDestFolderPath) {
    try {
      await IOUtils.makeDirectory(configuredDestFolderPath, {
        createAncestors: true,
        ignoreExisting: true,
      });
      if (Services.sysinfo.getProperty("name") === "Windows_NT") {
        await this.#createDesktopIni(configuredDestFolderPath);
      }
      return configuredDestFolderPath;
    } catch (e) {
      lazy.logConsole.warn("Could not create configured destination path: ", e);
      throw new BackupError(
        "Could not resolve to a writable destination folder path.",
        ERRORS.FILE_SYSTEM_ERROR
      );
    }
  }

  async resolveDownloadLink(updateChannel) {
    const ULTIMATE_FALLBACK_DOWNLOAD_URL =
      "https://www.firefox.com/?utm_medium=firefox-desktop&utm_source=html-backup";
    const FALLBACK_DOWNLOAD_URL = Services.prefs.getStringPref(
      `browser.backup.template.fallback-download.${updateChannel}`,
      ULTIMATE_FALLBACK_DOWNLOAD_URL
    );


    return FALLBACK_DOWNLOAD_URL;
  }

  async createAndPopulateStagingFolder(profilePath, encState) {
    let currentStep, backupDirPath, renamedStagingPath, manifest;
    try {
      currentStep = BACKUP_STEPS.CREATE_BACKUP_CREATE_MANIFEST;
      manifest = await this.#createBackupManifest();

      currentStep = BACKUP_STEPS.CREATE_BACKUP_CREATE_BACKUPS_FOLDER;
      backupDirPath = PathUtils.join(
        profilePath,
        BackupService.PROFILE_FOLDER_NAME,
        BackupService.SNAPSHOTS_FOLDER_NAME
      );
      lazy.logConsole.debug("Creating backups folder");

      await IOUtils.makeDirectory(backupDirPath, {
        ignoreExisting: true,
        createAncestors: true,
      });

      currentStep = BACKUP_STEPS.CREATE_BACKUP_CREATE_STAGING_FOLDER;
      let stagingPath = await this.#prepareStagingFolder(backupDirPath);

      let sortedResources = Array.from(this.#resources.values()).sort(
        (a, b) => {
          return b.priority - a.priority;
        }
      );

      if (encState === undefined) {
        currentStep = BACKUP_STEPS.CREATE_BACKUP_LOAD_ENCSTATE;
        encState = await this.loadEncryptionState(profilePath);
      } else {
        lazy.logConsole.debug("Using encState param: ", encState);
      }

      let encryptionEnabled = !!encState;
      lazy.logConsole.debug("Encryption enabled: ", encryptionEnabled);

      currentStep = BACKUP_STEPS.CREATE_BACKUP_RUN_BACKUP;
      for (let resourceClass of sortedResources) {
        try {
          lazy.logConsole.debug(
            `Backing up resource with key ${resourceClass.key}. ` +
              `Requires encryption: ${resourceClass.requiresEncryption}`
          );

          if (resourceClass.requiresEncryption && !encryptionEnabled) {
            lazy.logConsole.debug(
              "Encryption is not currently enabled. Skipping."
            );
            continue;
          }

          if (!resourceClass.canBackupResource) {
            lazy.logConsole.debug(
              `We cannot backup ${resourceClass.key}. Skipping.`
            );
            continue;
          }

          let resourcePath = PathUtils.join(stagingPath, resourceClass.key);
          await IOUtils.makeDirectory(resourcePath);

          let manifestEntry = await new resourceClass().backup(
            resourcePath,
            profilePath,
            encryptionEnabled
          );

          if (manifestEntry === undefined) {
            lazy.logConsole.error(
              `Backup of resource with key ${resourceClass.key} returned undefined
                as its ManifestEntry instead of null or an object`
            );
          } else {
            lazy.logConsole.debug(
              `Backup of resource with key ${resourceClass.key} completed`,
              manifestEntry
            );
            manifest.resources[resourceClass.key] = manifestEntry;
          }
        } catch (e) {
          lazy.logConsole.error(
            `Failed to backup resource: ${resourceClass.key}`,
            e
          );
        }
      }

      currentStep = BACKUP_STEPS.CREATE_BACKUP_VERIFY_MANIFEST;
      let manifestSchema = await BackupService.MANIFEST_SCHEMA;
      let schemaValidationResult = lazy.JsonSchema.validate(
        manifest,
        manifestSchema
      );
      if (!schemaValidationResult.valid) {
        lazy.logConsole.error(
          "Backup manifest does not conform to schema:",
          manifest,
          manifestSchema,
          schemaValidationResult
        );
      }

      currentStep = BACKUP_STEPS.CREATE_BACKUP_WRITE_MANIFEST;
      let manifestPath = PathUtils.join(
        stagingPath,
        BackupService.MANIFEST_FILE_NAME
      );
      await IOUtils.writeJSON(manifestPath, manifest);

      currentStep = BACKUP_STEPS.CREATE_BACKUP_FINALIZE_STAGING;
      renamedStagingPath = await this.#finalizeStagingFolder(stagingPath);
      lazy.logConsole.log(
        "Wrote backup to staging directory at ",
        renamedStagingPath
      );

      let totalSizeKilobytes =
        await BackupResource.getDirectorySize(renamedStagingPath);
      let totalSizeBytesNearestMebibyte = MeasurementUtils.fuzzByteSize(
        totalSizeKilobytes * BYTES_IN_KILOBYTE,
        1 * BYTES_IN_MEBIBYTE
      );
      lazy.logConsole.debug(
        "total staging directory size in bytes: " +
          totalSizeBytesNearestMebibyte
      );

    } catch (e) {
      return { currentStep, error: e };
    }

    return {
      currentStep,
      backupDirPath,
      stagingPath: renamedStagingPath,
      manifest,
    };
  }


  async createBackup({
    profilePath = PathUtils.profileDir,
    reason = "unknown",
    encState,
  } = {}) {
    let status = this.archiveEnabledStatus;
    if (!status.enabled) {
      lazy.logConsole.debug(status.reason);
      return null;
    }

    if (this.#backupInProgress) {
      lazy.logConsole.warn("Backup attempt already in progress");
      return null;
    }


    if (encState === undefined) {
      encState = await this.loadEncryptionState(profilePath);
    }

    return locks.request(
      BackupService.WRITE_BACKUP_LOCK_NAME,
      { signal: this.#backupWriteAbortController.signal },
      async () => {
        let currentStep = BACKUP_STEPS.CREATE_BACKUP_ENTRYPOINT;
        this.#backupInProgress = true;
        Services.prefs.clearUserPref(BACKUP_DEBUG_INFO_PREF_NAME);
        Services.prefs.setIntPref(BACKUP_ERROR_CODE_PREF_NAME, ERRORS.NONE);
        Services.prefs.clearUserPref("browser.profiles.profile-copied");

        try {
          lazy.logConsole.debug(
            `Creating backup for profile at ${profilePath}`
          );

          currentStep = BACKUP_STEPS.CREATE_BACKUP_RESOLVE_DESTINATION;
          let archiveDestFolderPath = await this.resolveArchiveDestFolderPath(
            lazy.backupDirPref
          );
          lazy.logConsole.debug(
            `Destination for archive: ${archiveDestFolderPath}`
          );

          let result = await this.createAndPopulateStagingFolder(
            profilePath,
            encState
          );
          currentStep = result.currentStep;
          if (result.error) {
            throw result.error;
          }

          let { backupDirPath, stagingPath, manifest } = result;

          currentStep = BACKUP_STEPS.CREATE_BACKUP_COMPRESS_STAGING;
          let compressedStagingPath = await this.#compressStagingFolder(
            stagingPath,
            backupDirPath
          ).finally(async () => {
            await IOUtils.remove(stagingPath, {
              recursive: true,
              retryReadonly: true,
            });
          });

          currentStep = BACKUP_STEPS.CREATE_BACKUP_CREATE_ARCHIVE;
          let archiveTmpPath = PathUtils.join(backupDirPath, "archive.html");
          lazy.logConsole.log(
            "Exporting single-file archive to ",
            archiveTmpPath
          );
          await this.createArchive(
            archiveTmpPath,
            BackupService.ARCHIVE_TEMPLATE,
            compressedStagingPath,
            encState,
            manifest.meta
          ).finally(async () => {
            await IOUtils.remove(compressedStagingPath, {
              retryReadonly: true,
            });
          });

          let archiveSizeKilobytes =
            await BackupResource.getFileSize(archiveTmpPath);
          let archiveSizeBytesNearestMebibyte = MeasurementUtils.fuzzByteSize(
            archiveSizeKilobytes * BYTES_IN_KILOBYTE,
            1 * BYTES_IN_MEBIBYTE
          );
          lazy.logConsole.debug(
            "backup archive size in bytes: " + archiveSizeBytesNearestMebibyte
          );


          currentStep = BACKUP_STEPS.CREATE_BACKUP_FINALIZE_ARCHIVE;
          let archivePath = await this.finalizeSingleFileArchive(
            archiveTmpPath,
            archiveDestFolderPath,
            manifest.meta
          );

          let nowSeconds = Math.floor(Date.now() / 1000);
          Services.prefs.setIntPref(
            LAST_BACKUP_TIMESTAMP_PREF_NAME,
            nowSeconds
          );
          this.#_state.lastBackupDate = nowSeconds;


          BackupService.maybeAddToEnabledListPref();

          Services.prefs.clearUserPref(DISABLED_ON_IDLE_RETRY_PREF_NAME);
          BackupService.#errorRetries = 0;

          return { manifest, archivePath };
        } catch (e) {

          Services.prefs.setIntPref(
            BACKUP_ERROR_CODE_PREF_NAME,
            ERRORS.UNKNOWN
          );

          Services.prefs.setStringPref(
            BACKUP_DEBUG_INFO_PREF_NAME,
            JSON.stringify({
              lastBackupAttempt: Math.floor(Date.now() / 1000),
              errorCode: e instanceof BackupError ? e : ERRORS.UNKNOWN,
              lastRunStep: currentStep,
            })
          );

          this.stateUpdate();
          throw e;
        } finally {
          this.#backupInProgress = false;
        }
      }
    );
  }


  generateArchiveDateSuffix(date) {
    let year = date.getFullYear().toString();


    let month = `${date.getMonth() + 1}`.padStart(2, "0");

    let day = `${date.getDate()}`.padStart(2, "0");
    let hours = `${date.getHours()}`.padStart(2, "0");
    let minutes = `${date.getMinutes()}`.padStart(2, "0");
    let seconds = `${date.getSeconds()}`.padStart(2, "0");
    let millis = `${date.getMilliseconds()}`.padStart(3, "0");

    return `${year}${month}${day}-${hours}${minutes}${seconds}.${millis}`;
  }

  async finalizeSingleFileArchive(sourcePath, destFolder, metadata) {
    let archiveDateSuffix = this.generateArchiveDateSuffix(
      new Date(metadata.date)
    );

    let existingChildren = await IOUtils.getChildren(destFolder);

    let nameParts = [BackupService.BACKUP_FILE_NAME, metadata.profileName];
    let storeID = lazy.SelectableProfileService.storeID;
    if (storeID) {
      nameParts.push(storeID);
    }
    const FILENAME_PREFIX = nameParts.join("_");

    const FILENAME = `${FILENAME_PREFIX}_${archiveDateSuffix}.html`;
    let destPath = PathUtils.join(destFolder, FILENAME);

    lazy.logConsole.log("Moving single-file archive to ", destPath);
    await IOUtils.move(sourcePath, destPath);

    Services.prefs.setStringPref(LAST_BACKUP_FILE_NAME_PREF_NAME, FILENAME);

    for (let childFilePath of existingChildren) {
      let childFileName = PathUtils.filename(childFilePath);
      if (
        childFileName.startsWith(FILENAME_PREFIX) &&
        childFileName.endsWith(".html")
      ) {
        if (childFileName == FILENAME) {
          lazy.logConsole.warn(
            "Collided with a pre-existing archive name, so not clearing: ",
            FILENAME
          );
          continue;
        }
        lazy.logConsole.debug("Getting rid of ", childFilePath);
        await IOUtils.remove(childFilePath);
      }
    }

    return destPath;
  }

  async #prepareStagingFolder(backupDirPath) {
    lazy.logConsole.debug(`Clearing snapshot folder ${backupDirPath}`);
    let numUnremovableStagingItems = 0;
    let folderEntries = await IOUtils.getChildren(backupDirPath, {
      ignoreAbsent: true,
    });
    if (folderEntries) {
      let unremovableContents = [];
      for (let folderItem of folderEntries) {
        try {
          lazy.logConsole.debug(`Removing ${folderItem}`);
          await IOUtils.remove(folderItem, {
            recursive: true,
            retryReadonly: true,
          });
        } catch (e) {
          lazy.logConsole.warn(
            `Failed to remove stale snapshot item ${folderItem}.  Exception: ${e}`
          );
          numUnremovableStagingItems++;
          unremovableContents.push(folderItem);
          if (
            numUnremovableStagingItems >
            lazy.maximumNumberOfUnremovableStagingItems
          ) {
            let error = new BackupError(
              `Failed to remove ${numUnremovableStagingItems} items from ${backupDirPath}`,
              ERRORS.FILE_SYSTEM_ERROR
            );
            error.stack = e.stack;
            error.unremovableContents = unremovableContents;
            throw error;
          }
        }
      }
    }

    lazy.logConsole.debug(
      `${numUnremovableStagingItems} unremovable staging items found.  Proceeding with backup.  Determining staging folder.`
    );
    let stagingPath;
    for (let i = 0; i < lazy.maximumNumberOfUnremovableStagingItems + 1; i++) {
      let potentialStagingPath = PathUtils.join(backupDirPath, "staging-" + i);
      if (!(await IOUtils.exists(potentialStagingPath))) {
        stagingPath = potentialStagingPath;
        await IOUtils.makeDirectory(stagingPath);
        break;
      }
    }

    if (!stagingPath) {
      throw new BackupError(
        `Internal error in attempt to create staging folder`,
        ERRORS.FILE_SYSTEM_ERROR
      );
    }

    lazy.logConsole.debug(`Staging folder ${stagingPath} is prepared`);
    return stagingPath;
  }

  async #compressStagingFolder(stagingPath, destFolderPath) {
    const PR_RDWR = 0x04;
    const PR_CREATE_FILE = 0x08;
    const PR_TRUNCATE = 0x20;

    let archivePath = PathUtils.join(
      destFolderPath,
      `${PathUtils.filename(stagingPath)}.zip`
    );
    let archiveFile = await IOUtils.getFile(archivePath);

    let writer = new lazy.ZipWriter(
      archiveFile,
      PR_RDWR | PR_CREATE_FILE | PR_TRUNCATE
    );

    lazy.logConsole.log("Compressing staging folder to ", archivePath);
    let rootPathNSIFile = await IOUtils.getDirectory(stagingPath);
    await this.#compressChildren(rootPathNSIFile, stagingPath, writer);
    await new Promise(resolve => {
      let observer = {
        onStartRequest(_request) {
          lazy.logConsole.debug("Starting to write out archive file");
        },
        onStopRequest(_request, status) {
          lazy.logConsole.log("Done writing archive file");
          resolve(status);
        },
      };
      writer.processQueue(observer, null);
    });
    writer.close();

    return archivePath;
  }

  async #compressChildren(rootPathNSIFile, parentPath, writer) {
    let children = await IOUtils.getChildren(parentPath);
    for (let childPath of children) {
      let childState = await IOUtils.stat(childPath);
      if (childState.type == "directory") {
        await this.#compressChildren(rootPathNSIFile, childPath, writer);
      } else {
        let childFile = await IOUtils.getFile(childPath);
        let pathRelativeToRoot = childFile.getRelativePath(rootPathNSIFile);
        writer.addEntryFile(
          pathRelativeToRoot,
          BackupService.COMPRESSION_LEVEL,
          childFile,
          true
        );
      }
    }
  }

  async decompressRecoveryFile(recoveryFilePath, recoveryFolderDestPath) {
    let recoveryFile = await IOUtils.getFile(recoveryFilePath);
    let recoveryArchive = new lazy.ZipReader(recoveryFile);
    lazy.logConsole.log(
      "Decompressing recovery folder to ",
      recoveryFolderDestPath
    );
    try {
      recoveryArchive.test(null);
    } catch (e) {
      recoveryArchive.close();
      lazy.logConsole.error("Compressed recovery file was corrupt.");
      await IOUtils.remove(recoveryFilePath, {
        retryReadonly: true,
      });
      throw new BackupError("Corrupt archive.", ERRORS.CORRUPTED_ARCHIVE);
    }

    try {
      await this.#decompressChildren(
        recoveryFolderDestPath,
        "",
        recoveryArchive
      );
    } catch (e) {
      recoveryArchive.close();
      throw e instanceof BackupError
        ? e
        : new BackupError(
            `Failed to decompress recovery file: ${e.message}`,
            ERRORS.DECOMPRESSION_FAILED
          );
    }
    recoveryArchive.close();
  }

  async #decompressChildren(rootPath, parentEntryName, reader) {
    let childEntryNames = reader.findEntries(
      parentEntryName + "?*~" + parentEntryName + "?*/?*"
    );

    for (let childEntryName of childEntryNames) {
      let childEntry = reader.getEntry(childEntryName);
      if (childEntry.isDirectory) {
        await this.#decompressChildren(rootPath, childEntryName, reader);
      } else {
        let inputStream = reader.getInputStream(childEntryName);
        let fileNameParts = childEntryName.split("/");
        let outputFilePath = PathUtils.join(rootPath, ...fileNameParts);
        let outputFile = await IOUtils.getFile(outputFilePath);
        let outputStream = Cc[
          "@mozilla.org/network/file-output-stream;1"
        ].createInstance(Ci.nsIFileOutputStream);

        outputStream.init(
          outputFile,
          -1,
          -1,
          Ci.nsIFileOutputStream.DEFER_OPEN
        );

        await new Promise(resolve => {
          lazy.logConsole.debug("Writing ", outputFilePath);
          lazy.NetUtil.asyncCopy(inputStream, outputStream, () => {
            lazy.logConsole.debug("Done writing ", outputFilePath);
            outputStream.close();
            resolve();
          });
        });
      }
    }
  }

  async renderTemplate(templateURI, isEncrypted, backupMetadata) {
    const ARCHIVE_STYLES = "chrome://browser/content/backup/archive.css";
    const ARCHIVE_SCRIPT = "chrome://browser/content/backup/archive.js";
    const LOGO = "chrome://branding/content/icon128.png";

    let templateResponse = await fetch(templateURI);
    let templateString = await templateResponse.text();
    let templateDOM = new DOMParser().parseFromString(
      templateString,
      "text/html"
    );

    templateDOM.documentElement.setAttribute(
      "lang",
      Services.locale.appLocaleAsBCP47
    );

    let downloadLink = templateDOM.querySelector("#download-moz-browser");
    downloadLink.href = await this.resolveDownloadLink(
      AppConstants.MOZ_UPDATE_CHANNEL
    );

    let supportURI = new URL(
      "firefox-backup",
      Services.urlFormatter.formatURLPref("app.support.baseURL")
    );
    supportURI.searchParams.set("utm_medium", "firefox-desktop");
    supportURI.searchParams.set("utm_source", "html-backup");
    supportURI.searchParams.set("utm_campaign", "fx-backup-restore");

    let supportLink = templateDOM.querySelector("#support-link");
    supportLink.href = supportURI.href;

    let logoResponse = await fetch(LOGO);
    let logoBlob = await logoResponse.blob();
    let logoDataURL = await new Promise((resolve, reject) => {
      let reader = new FileReader();
      reader.addEventListener("load", () => resolve(reader.result));
      reader.addEventListener("error", reject);
      reader.readAsDataURL(logoBlob);
    });

    let logoNode = templateDOM.querySelector("#logo");
    logoNode.src = logoDataURL;

    let encStateNode = templateDOM.querySelector("#encryption-state-value");
    lazy.gDOMLocalization.setAttributes(
      encStateNode,
      isEncrypted
        ? "backup-file-encryption-state-value-encrypted"
        : "backup-file-encryption-state-value-not-encrypted"
    );

    let createdDateNode = templateDOM.querySelector("#creation-date-value");
    lazy.gDOMLocalization.setArgs(createdDateNode, {
      date: new Date(backupMetadata.date).getTime() || new Date().getTime(),
    });

    let creationDeviceNode = templateDOM.querySelector(
      "#creation-device-value"
    );
    creationDeviceNode.textContent = backupMetadata.machineName;

    try {
      await lazy.gDOMLocalization.translateFragment(
        templateDOM.documentElement
      );
    } catch (_) {
    }

    let stylesResponse = await fetch(ARCHIVE_STYLES);
    let scriptResponse = await fetch(ARCHIVE_SCRIPT);

    // can't ifdef out the MPL license header in styles before writing it into
    // the archive file. Instead, we'll ensure that the license header is there,
    let stylesText = await stylesResponse.text();
    const MPL_LICENSE = `/**
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */`;
    if (!stylesText.includes(MPL_LICENSE)) {
      throw new BackupError(
        "Expected the MPL license block within archive.css",
        ERRORS.UNKNOWN
      );
    }

    stylesText = stylesText.replace(MPL_LICENSE, "");

    let serializer = new XMLSerializer();
    return serializer
      .serializeToString(templateDOM)
      .replace("{{styles}}", stylesText)
      .replace("{{script}}", await scriptResponse.text());
  }

  async createArchive(
    archivePath,
    templateURI,
    compressedBackupSnapshotPath,
    encState,
    backupMetadata,
    options = {}
  ) {
    let markup = await this.renderTemplate(
      templateURI,
      !!encState,
      backupMetadata
    );

    let worker = new lazy.BasePromiseWorker(
      "resource:///modules/backup/Archive.worker.mjs",
      { type: "module" }
    );
    worker.ExceptionHandlers[BackupError.name] = BackupError.fromMsg;

    let chunkSize =
      options.chunkSize || lazy.ArchiveUtils.ARCHIVE_CHUNK_MAX_BYTES_SIZE;

    try {
      let encryptionArgs = encState
        ? {
            publicKey: encState.publicKey,
            salt: encState.salt,
            nonce: encState.nonce,
            backupAuthKey: encState.backupAuthKey,
            wrappedSecrets: encState.wrappedSecrets,
          }
        : null;

      await worker
        .post("constructArchive", [
          {
            archivePath,
            markup,
            backupMetadata,
            compressedBackupSnapshotPath,
            encryptionArgs,
            chunkSize,
          },
        ])
        .catch(e => {
          lazy.logConsole.error(e);
          if (!(e instanceof BackupError)) {
            throw new BackupError("Failed to create archive", ERRORS.UNKNOWN);
          }
          throw e;
        });
    } finally {
      worker.terminate();
    }
  }

  #createExtractionChannel(inputStream, contentType) {
    let uri = "http://localhost";
    let httpChan = lazy.NetUtil.newChannel({
      uri,
      loadUsingSystemPrincipal: true,
    });

    let channel = Cc["@mozilla.org/network/input-stream-channel;1"]
      .createInstance(Ci.nsIInputStreamChannel)
      .QueryInterface(Ci.nsIChannel);

    channel.setURI(httpChan.URI);
    channel.loadInfo = httpChan.loadInfo;

    channel.contentStream = inputStream;
    channel.contentType = contentType;
    return channel;
  }

  async #extractJSONFromArchive(archiveFile, startByteOffset, contentType) {
    let fileInputStream = Cc[
      "@mozilla.org/network/file-input-stream;1"
    ].createInstance(Ci.nsIFileInputStream);
    fileInputStream.init(
      archiveFile,
      -1,
      -1,
      Ci.nsIFileInputStream.CLOSE_ON_EOF
    );
    fileInputStream.seek(Ci.nsISeekableStream.NS_SEEK_SET, startByteOffset);

    const EXPECTED_CONTENT_TYPE = "application/json";

    let extractionChannel = this.#createExtractionChannel(
      fileInputStream,
      contentType
    );
    let textDecoder = new TextDecoder();
    return new Promise((resolve, reject) => {
      let streamConv = Cc["@mozilla.org/streamConverters;1"].getService(
        Ci.nsIStreamConverterService
      );
      let multipartListenerForJSON = {
        _enabled: false,

        _done: false,

        _buffer: "",

        QueryInterface: ChromeUtils.generateQI([
          "nsIStreamListener",
          "nsIRequestObserver",
          "nsIMultiPartChannelListener",
        ]),

        onStartRequest(request) {
          if (!(request instanceof Ci.nsIChannel)) {
            throw Components.Exception(
              "onStartRequest expected an nsIChannel request",
              Cr.NS_ERROR_UNEXPECTED
            );
          }
          this._enabled = request.contentType == EXPECTED_CONTENT_TYPE;
        },

        onDataAvailable(request, stream, offset, count) {
          if (this._done) {
            throw Components.Exception(
              "Got JSON block. Aborting further reads.",
              Cr.NS_BINDING_ABORTED
            );
          }
          if (!this._enabled) {
            return;
          }

          let binStream = new lazy.BinaryInputStream(stream);
          let arrBuffer = new ArrayBuffer(count);
          binStream.readArrayBuffer(count, arrBuffer);
          let jsonBytes = new Uint8Array(arrBuffer);
          this._buffer += textDecoder.decode(jsonBytes);
        },

        onStopRequest() {
          if (this._enabled && !this._done) {
            this._enabled = false;
            this._done = true;

            try {
              let archiveMetadata = JSON.parse(this._buffer);
              resolve(archiveMetadata);
            } catch (e) {
              reject(
                new BackupError(
                  "Could not parse archive metadata.",
                  ERRORS.CORRUPTED_ARCHIVE
                )
              );
            }
          }
        },

        onAfterLastPart() {
          if (!this._done) {
            reject(
              new BackupError(
                "Could not find JSON block.",
                ERRORS.CORRUPTED_ARCHIVE
              )
            );
          }
        },
      };
      let conv = streamConv.asyncConvertData(
        "multipart/mixed",
        "*/*",
        multipartListenerForJSON,
        null
      );

      extractionChannel.asyncOpen(conv);
    });
  }

  async createBinaryReadableStream(archiveFile, startByteOffset, contentType) {
    let fileInputStream = Cc[
      "@mozilla.org/network/file-input-stream;1"
    ].createInstance(Ci.nsIFileInputStream);
    fileInputStream.init(
      archiveFile,
      -1,
      -1,
      Ci.nsIFileInputStream.CLOSE_ON_EOF
    );
    fileInputStream.seek(Ci.nsISeekableStream.NS_SEEK_SET, startByteOffset);

    let extractionChannel = this.#createExtractionChannel(
      fileInputStream,
      contentType
    );

    return new ReadableStream(new BinaryReadableStream(extractionChannel));
  }


  async sampleArchive(archivePath) {
    let worker = new lazy.BasePromiseWorker(
      "resource:///modules/backup/Archive.worker.mjs",
      { type: "module" }
    );
    worker.ExceptionHandlers[BackupError.name] = BackupError.fromMsg;

    if (!(await IOUtils.exists(archivePath))) {
      throw new BackupError(
        "Archive file does not exist at path " + archivePath,
        ERRORS.UNKNOWN
      );
    }

    try {
      let { startByteOffset, contentType } = await worker
        .post("parseArchiveHeader", [archivePath])
        .catch(e => {
          lazy.logConsole.error(e);
          if (!(e instanceof BackupError)) {
            throw new BackupError(
              "Failed to parse archive header",
              ERRORS.CORRUPTED_ARCHIVE
            );
          }
          throw e;
        });
      let archiveFile = await IOUtils.getFile(archivePath);
      let archiveJSON;
      try {
        archiveJSON = await this.#extractJSONFromArchive(
          archiveFile,
          startByteOffset,
          contentType
        );

        if (!archiveJSON.version) {
          throw new BackupError(
            "Missing version in the archive JSON block.",
            ERRORS.CORRUPTED_ARCHIVE
          );
        }
        if (archiveJSON.version > lazy.ArchiveUtils.SCHEMA_VERSION) {
          throw new BackupError(
            `Archive JSON block is a version newer than we can interpret: ${archiveJSON.version}`,
            ERRORS.UNSUPPORTED_BACKUP_VERSION
          );
        }

        let archiveJSONSchema = await BackupService.getSchemaForVersion(
          SCHEMAS.ARCHIVE_JSON_BLOCK,
          archiveJSON.version
        );

        let manifestSchema = await BackupService.getSchemaForVersion(
          SCHEMAS.BACKUP_MANIFEST,
          archiveJSON.version
        );

        let validator = new lazy.JsonSchema.Validator(archiveJSONSchema);
        validator.addSchema(manifestSchema);

        let schemaValidationResult = validator.validate(archiveJSON);
        if (!schemaValidationResult.valid) {
          lazy.logConsole.error(
            "Archive JSON block does not conform to schema:",
            archiveJSON,
            archiveJSONSchema,
            schemaValidationResult
          );

          throw new BackupError(
            `Archive JSON block does not conform to schema version ${archiveJSON.version}`,
            ERRORS.CORRUPTED_ARCHIVE
          );
        }
      } catch (e) {
        lazy.logConsole.error(e);
        throw e;
      }

      lazy.logConsole.debug("Read out archive JSON: ", archiveJSON);

      return {
        isEncrypted: !!archiveJSON.encConfig,
        startByteOffset,
        contentType,
        archiveJSON,
      };
    } catch (e) {
      lazy.logConsole.error(e);
      throw e;
    } finally {
      worker.terminate();
    }
  }

  async extractCompressedSnapshotFromArchive(
    archivePath,
    extractionDestPath,
    recoveryCode = null
  ) {
    let { isEncrypted, startByteOffset, contentType, archiveJSON } =
      await this.sampleArchive(archivePath);

    let decryptor = null;
    if (isEncrypted) {
      if (!recoveryCode) {
        throw new BackupError(
          "A recovery code is required to decrypt this archive.",
          ERRORS.UNAUTHORIZED
        );
      }
      decryptor = await lazy.ArchiveDecryptor.initialize(
        recoveryCode,
        archiveJSON
      );
    }

    await IOUtils.remove(extractionDestPath, {
      ignoreAbsent: true,
      retryReadonly: true,
    });

    let archiveFile = await IOUtils.getFile(archivePath);
    let archiveStream = await this.createBinaryReadableStream(
      archiveFile,
      startByteOffset,
      contentType
    );

    let binaryDecoder = new TransformStream(
      new DecoderDecryptorTransformer(decryptor)
    );
    let fileWriter = new WritableStream(
      new FileWriterStream(extractionDestPath, decryptor)
    );
    try {
      await archiveStream.pipeThrough(binaryDecoder).pipeTo(fileWriter);
    } catch (e) {
      throw e instanceof BackupError
        ? e
        : new BackupError(
            `Failed to extract snapshot: ${e?.message || e}`,
            ERRORS.CORRUPTED_ARCHIVE
          );
    }

    if (decryptor) {
      await lazy.nativeOSKeyStore.asyncRecoverSecret(
        BackupService.RECOVERY_OSKEYSTORE_LABEL,
        decryptor.OSKeyStoreSecret
      );
    }
  }

  async #finalizeStagingFolder(stagingPath) {
    if (!(await IOUtils.exists(stagingPath))) {
      lazy.logConsole.error(
        `Failed to finalize staging folder. Cannot find ${stagingPath}.`
      );
      return null;
    }

    try {
      lazy.logConsole.debug("Finalizing and renaming staging folder");
      let currentDateISO = new Date().toISOString();
      let dateISOStripped = currentDateISO.replace(/\.\d+\Z$/, "Z");
      let dateISOFormatted = dateISOStripped.replaceAll(":", "-");

      let stagingPathParent = PathUtils.parent(stagingPath);
      let renamedBackupPath = PathUtils.join(
        stagingPathParent,
        dateISOFormatted
      );
      await IOUtils.move(stagingPath, renamedBackupPath);

      let existingBackups = await IOUtils.getChildren(stagingPathParent);

      let expectedFormatRegex = /\d{4}(-\d{2}){2}T(\d{2}-){2}\d{2}Z/;
      for (let existingBackupPath of existingBackups) {
        if (
          existingBackupPath !== renamedBackupPath &&
          existingBackupPath.match(expectedFormatRegex)
        ) {
          try {
            await IOUtils.remove(existingBackupPath, {
              recursive: true,
              retryReadonly: true,
            });
          } catch (e) {
            lazy.logConsole.debug(
              `Failed to remove staging item ${existingBackupPath}. Exception ${e}`
            );
          }
        }
      }
      return renamedBackupPath;
    } catch (e) {
      lazy.logConsole.error(
        `Something went wrong while finalizing the staging folder. ${e}`
      );
      throw new BackupError(
        "Failed to finalize staging folder",
        ERRORS.FILE_SYSTEM_ERROR
      );
    }
  }

  async #createBackupManifest() {
    let profileSvc = Cc["@mozilla.org/toolkit/profile-service;1"].getService(
      Ci.nsIToolkitProfileService
    );
    let profileName;
    if (!profileSvc.currentProfile) {
      let profileFolder = PathUtils.split(PathUtils.profileDir).at(-1);
      profileName = profileFolder.substring(profileFolder.indexOf(".") + 1);
    } else if (lazy.SelectableProfileService.currentProfile) {
      profileName = lazy.SelectableProfileService.currentProfile.name;
    } else {
      profileName = profileSvc.currentProfile.name;
    }

    let meta = {
      date: new Date().toISOString(),
      appName: AppConstants.MOZ_APP_NAME,
      appVersion: AppConstants.MOZ_APP_VERSION,
      buildID: AppConstants.MOZ_BUILDID,
      profileName,
      deviceName: Services.sysinfo.get("device") || Services.dns.myHostName,
      machineName: lazy.fxAccounts.device.getLocalName(),
      osName: Services.sysinfo.getProperty("name"),
      osVersion: Services.sysinfo.getProperty("version"),
      osBuildNumber: (() => {
        try {
          return Services.sysinfo.getProperty("build");
        } catch {
          return null;
        }
      })(),
      isSelectableProfile: !!lazy.SelectableProfileService.currentProfile,
    };

    let fxaState = lazy.UIState.get();
    if (fxaState.status == lazy.UIState.STATUS_SIGNED_IN) {
      meta.accountID = fxaState.uid;
      meta.accountEmail = fxaState.email;
    }

    return {
      version: lazy.ArchiveUtils.SCHEMA_VERSION,
      meta,
      resources: {},
    };
  }

  async recoverFromBackupArchive(
    archivePath,
    recoveryCode = null,
    shouldLaunchOrQuit = false,
    profilePath = PathUtils.profileDir,
    profileRootPath = null,
    replaceCurrentProfile = false,
    source = null
  ) {
    const status = this.restoreEnabledStatus;
    if (!status.enabled) {
      throw new Error(status.reason);
    }

    if (this.#_state.recoveryInProgress) {
      lazy.logConsole.warn("Recovery attempt already in progress");
      return null;
    }


    let currentStep = RESTORE_STEPS.RESTORE_ENTRYPOINT;
    try {
      this.#_state.recoveryInProgress = true;
      this.#_state.recoveryErrorCode = 0;

      try {
        let profileAge = await lazy.ProfileAge();
        this.#_state.intermediateProfileCreationDate = await profileAge.created;
      } catch (e) {
        lazy.logConsole.warn("Failed to get intermediate profile date", e);
        this.#_state.intermediateProfileCreationDate = null;
      }

      this.#_state.restoreSource = source;
      this.stateUpdate();
      const RECOVERY_FILE_DEST_PATH = PathUtils.join(
        profilePath,
        BackupService.PROFILE_FOLDER_NAME,
        BackupService.RECOVERY_ZIP_FILE_NAME
      );
      currentStep = RESTORE_STEPS.RESTORE_EXTRACT_SNAPSHOT;
      await this.extractCompressedSnapshotFromArchive(
        archivePath,
        RECOVERY_FILE_DEST_PATH,
        recoveryCode
      );

      const RECOVERY_FOLDER_DEST_PATH = PathUtils.join(
        profilePath,
        BackupService.PROFILE_FOLDER_NAME,
        "recovery"
      );
      currentStep = RESTORE_STEPS.RESTORE_DECOMPRESS;
      await this.decompressRecoveryFile(
        RECOVERY_FILE_DEST_PATH,
        RECOVERY_FOLDER_DEST_PATH
      );

      try {
        await IOUtils.remove(RECOVERY_FILE_DEST_PATH, { retryReadonly: true });
      } catch (_) {
        lazy.logConsole.warn("Could not remove ", RECOVERY_FILE_DEST_PATH);
      }

      try {

        currentStep = RESTORE_STEPS.RESTORE_READ_MANIFEST;
        let manifest = await this.#readAndValidateManifest(
          RECOVERY_FOLDER_DEST_PATH
        );

        const replacingLegacyWithLegacy =
          replaceCurrentProfile && manifest.meta?.isSelectableProfile === false;

        currentStep = RESTORE_STEPS.RESTORE_PROFILE_SETUP;
        if (!lazy.SelectableProfileService.isEnabled) {
          delete manifest.resources[
            DefaultBackupResources.SelectableProfileBackupResource.key
          ];
        } 
        else if (
          !lazy.SelectableProfileService.hasCreatedSelectableProfiles() &&
          !replacingLegacyWithLegacy
        ) {
          try {
            lazy.logConsole.debug(
              `Converting current legacy profile into a selectable profile`
            );
            await lazy.SelectableProfileService.maybeSetupDataStore();
          } catch (e) {
            throw new BackupError(
              `something went wrong when converting the current profile into a selectableProfile: ${e}`,
              ERRORS.PROFILE_CREATION_FAILED
            );
          }
        }

        currentStep = RESTORE_STEPS.RESTORE_CREATE_PROFILE;
        let newProfile;
        if (lazy.SelectableProfileService.currentProfile) {
          newProfile =
            await this.recoverFromSnapshotFolderIntoSelectableProfile(
              RECOVERY_FOLDER_DEST_PATH,
              shouldLaunchOrQuit,
              null,
              profileRootPath,
              manifest,
              replaceCurrentProfile
            );
        } else {
          newProfile = await this.recoverFromSnapshotFolder(
            RECOVERY_FOLDER_DEST_PATH,
            shouldLaunchOrQuit,
            profileRootPath,
            manifest
          );
        }

        currentStep = RESTORE_STEPS.RESTORE_FINALIZE;
        Services.obs.notifyObservers(null, "browser-backup-restore-complete");

        if (replaceCurrentProfile) {
          try {
            await this.deleteAndQuitCurrentSelectableProfile(
              shouldLaunchOrQuit
            );
          } catch (e) {
            lazy.logConsole.error(
              "Failed to delete and quit current profile after successful restore",
              e
            );
          }
        }

        return newProfile;
      } finally {
        if (recoveryCode) {
          await lazy.nativeOSKeyStore.asyncDeleteSecret(
            BackupService.RECOVERY_OSKEYSTORE_LABEL
          );
        }
      }
    } catch (ex) {
      let restoreStep = ex.resourceKey
        ? "RECOVER_RESOURCE:" + ex.resourceKey
        : ex.restoreStep || currentStep;
      throw ex;
    } finally {
      this.#_state.recoveryInProgress = false;
      this.stateUpdate();
    }
  }

  async deleteAndQuitCurrentSelectableProfile(shouldQuit = true) {
    if (!lazy.SelectableProfileService.hasCreatedSelectableProfiles()) {
      lazy.logConsole.warn(
        `We don't delete the running profile in the case of legacy backup to legacy recovery`
      );
      return null;
    }

    let cancelQuit = Cc["@mozilla.org/supports-PRBool;1"].createInstance(
      Ci.nsISupportsPRBool
    );
    Services.obs.notifyObservers(cancelQuit, "quit-application-requested");

    if (cancelQuit.data) {
      return null;
    }

    try {
      await lazy.SelectableProfileService.deleteCurrentProfile();

      if (shouldQuit) {
        Services.startup.quit(Ci.nsIAppStartup.eAttemptQuit);
      }
    } catch (e) {
      lazy.logConsole.error(`Errored while attempting delete and quit: ${e}`);
    }
    return null;
  }

  async #readAndValidateManifest(recoveryPath) {
    let manifestPath = PathUtils.join(
      recoveryPath,
      BackupService.MANIFEST_FILE_NAME
    );

    let manifest;
    try {
      manifest = await IOUtils.readJSON(manifestPath);
    } catch (e) {
      throw new BackupError(
        `Failed to read backup manifest: ${e.message}`,
        ERRORS.CORRUPTED_ARCHIVE
      );
    }
    if (!manifest.version) {
      throw new BackupError(
        "Backup manifest version not found",
        ERRORS.CORRUPTED_ARCHIVE
      );
    }

    if (manifest.version > lazy.ArchiveUtils.SCHEMA_VERSION) {
      throw new BackupError(
        "Cannot recover from a manifest newer than the current schema version",
        ERRORS.UNSUPPORTED_BACKUP_VERSION
      );
    }

    let manifestSchema = await BackupService.getSchemaForVersion(
      SCHEMAS.BACKUP_MANIFEST,
      manifest.version
    );
    let schemaValidationResult = lazy.JsonSchema.validate(
      manifest,
      manifestSchema
    );
    if (!schemaValidationResult.valid) {
      lazy.logConsole.error(
        "Backup manifest does not conform to schema:",
        manifest,
        manifestSchema,
        schemaValidationResult
      );
      throw new BackupError(
        "Cannot recover from an invalid backup manifest",
        ERRORS.CORRUPTED_ARCHIVE
      );
    }

    if (manifest.version < 2) {
      manifest.isSelectableProfile = false;
    }

    let meta = manifest.meta;

    if (meta.appName != AppConstants.MOZ_APP_NAME) {
      throw new BackupError(
        `Cannot recover a backup from ${meta.appName} in ${AppConstants.MOZ_APP_NAME}`,
        ERRORS.UNSUPPORTED_APPLICATION
      );
    }

    if (
      Services.vc.compare(AppConstants.MOZ_APP_VERSION, meta.appVersion) < 0
    ) {
      throw new BackupError(
        `Cannot recover a backup created on version ${meta.appVersion} in ${AppConstants.MOZ_APP_VERSION}`,
        ERRORS.UNSUPPORTED_BACKUP_VERSION
      );
    }

    return manifest;
  }

  async #getLegacyThemeId(recoveryPath) {
    const DEFAULT_THEME_ID = "default-theme@mozilla.org";
    let prefsPath = PathUtils.join(
      recoveryPath,
      DefaultBackupResources.PreferencesBackupResource.key,
      "prefs.js"
    );
    try {
      let prefsBuffer = await IOUtils.read(prefsPath);
      let prefs =
        DefaultBackupResources.PreferencesBackupResource.getPrefsFromBuffer(
          prefsBuffer,
          ["extensions.activeThemeID"]
        );
      return prefs.get("extensions.activeThemeID") || DEFAULT_THEME_ID;
    } catch (e) {
      lazy.logConsole.warn("Could not read legacy theme from prefs.js", e);
      return DEFAULT_THEME_ID;
    }
  }

  async #recoverResources(manifest, recoveryPath, profilePath) {
    let postRecovery = {};

    for (let resourceKey in manifest.resources) {
      let manifestEntry = manifest.resources[resourceKey];
      let resourceClass = this.#resources.get(resourceKey);
      if (!resourceClass) {
        lazy.logConsole.error(`No BackupResource found for key ${resourceKey}`);
        continue;
      }

      try {
        lazy.logConsole.debug(
          `Restoring resource with key ${resourceKey}. ` +
            `Requires encryption: ${resourceClass.requiresEncryption}`
        );
        let resourcePath = PathUtils.join(recoveryPath, resourceKey);
        let postRecoveryEntry = await new resourceClass().recover(
          manifestEntry,
          resourcePath,
          profilePath
        );
        postRecovery[resourceKey] = postRecoveryEntry;
      } catch (e) {
        lazy.logConsole.error(`Failed to recover resource: ${resourceKey}`, e);
        if (e instanceof BackupError) {
          e.resourceKey = resourceKey;
          throw e;
        }
        let err = new BackupError(
          `Failed to recover resource ${resourceKey}: ${e.message}`,
          ERRORS.RESOURCE_RECOVERY_FAILED
        );
        err.resourceKey = resourceKey;
        throw err;
      }
    }

    return postRecovery;
  }

  async #writePostRecoveryData(postRecoveryData, profilePath) {
    let postRecoveryPath = PathUtils.join(
      profilePath,
      BackupService.POST_RECOVERY_FILE_NAME
    );
    await IOUtils.writeJSON(postRecoveryPath, postRecoveryData);
  }

  async recoverFromSnapshotFolder(
    recoveryPath,
    shouldLaunch = false,
    profileRootPath = null,
    manifest = null
  ) {
    lazy.logConsole.debug("Recovering from backup at ", recoveryPath);

    let restoreStep = RESTORE_STEPS.RESTORE_CREATE_PROFILE;
    try {
      if (!manifest) {
        manifest = await this.#readAndValidateManifest(recoveryPath);
      }

      let profileSvc = Cc["@mozilla.org/toolkit/profile-service;1"].getService(
        Ci.nsIToolkitProfileService
      );
      let profile;
      try {
        profile = profileSvc.createUniqueProfile(
          profileRootPath ? await IOUtils.getDirectory(profileRootPath) : null,
          manifest.meta.profileName,
          "backup"
        );
      } catch (e) {
        throw e instanceof BackupError
          ? e
          : new BackupError(
              `Failed to create profile: ${e.message}`,
              ERRORS.PROFILE_CREATION_FAILED
            );
      }

      restoreStep = RESTORE_STEPS.RESTORE_RECOVER_RESOURCES;
      let postRecovery = await this.#recoverResources(
        manifest,
        recoveryPath,
        profile.rootDir.path
      );

      restoreStep = RESTORE_STEPS.RESTORE_WRITE_POST_RECOVERY;
      await this.#writePostRecoveryData(postRecovery, profile.rootDir.path);

      restoreStep = RESTORE_STEPS.RESTORE_CONFIGURE_PROFILE;
      if (profileSvc.currentProfile) {
        if (profileSvc.currentProfile === profileSvc.defaultProfile) {
          profileSvc.defaultProfile = profile;
        }

        if (!profileSvc.currentProfile.name.startsWith("old-")) {
          profileSvc.currentProfile.name = `old-${profileSvc.currentProfile.name}`;
        }
      }

      await profileSvc.asyncFlush();

      restoreStep = RESTORE_STEPS.RESTORE_LAUNCH_PROFILE;
      if (shouldLaunch) {
        Services.startup.createInstanceWithProfile(profile, [
          "--url",
          "about:home",
        ]);
      }

      return profile;
    } catch (e) {
      lazy.logConsole.error(
        "Failed to recover from backup at ",
        recoveryPath,
        e
      );
      let err =
        e instanceof BackupError
          ? e
          : new BackupError(
              `Recovery failed: ${e.message}`,
              ERRORS.RECOVERY_FAILED
            );
      err.restoreStep = err.restoreStep || restoreStep;
      throw err;
    }
  }

  async recoverFromSnapshotFolderIntoSelectableProfile(
    recoveryPath,
    shouldLaunch = false,
    copiedProfile = null,
    profileRootPath = null,
    manifest = null,
    replaceCurrentProfile = false
  ) {
    lazy.logConsole.debug(
      "Recovering SelectableProfile from backup at ",
      recoveryPath
    );

    let restoreStep = RESTORE_STEPS.RESTORE_CREATE_PROFILE;
    try {
      if (!manifest) {
        manifest = await this.#readAndValidateManifest(recoveryPath);
      }

      let profile;
      try {
        let existingProfilePath = null;
        if (profileRootPath) {
          let profileDirName = `recovered-${Date.now()}`;
          let profileDirPath = PathUtils.join(profileRootPath, profileDirName);
          await IOUtils.makeDirectory(profileDirPath, { permissions: 0o700 });
          existingProfilePath = await IOUtils.getDirectory(profileDirPath);
        }
        profile = await lazy.SelectableProfileService.createNewProfile(
          false,
          existingProfilePath,
          "backup"
        );
      } catch (e) {
        throw e instanceof BackupError
          ? e
          : new BackupError(
              `Failed to create selectable profile: ${e.message}`,
              ERRORS.PROFILE_CREATION_FAILED
            );
      }

      restoreStep = RESTORE_STEPS.RESTORE_RECOVER_RESOURCES;
      let postRecovery = await this.#recoverResources(
        manifest,
        recoveryPath,
        profile.path
      );

      if (copiedProfile) {
        let profileAge = await lazy.ProfileAge(profile.path);
        await profileAge.recordProfileCopied();
      }

      restoreStep = RESTORE_STEPS.RESTORE_CONFIGURE_PROFILE;
      let isLegacyBackup = manifest.meta?.isSelectableProfile === false;

      if (replaceCurrentProfile && isLegacyBackup) {
        let currentSelectableProfile =
          lazy.SelectableProfileService.currentProfile;

        profile.name = currentSelectableProfile.name;
        await profile.setAvatar(
          currentSelectableProfile.hasCustomAvatar
            ? await currentSelectableProfile.getAvatarFile()
            : currentSelectableProfile.avatar
        );

        let currentTheme = currentSelectableProfile.theme;
        await profile.setThemeAsync(currentTheme);

        postRecovery[
          DefaultBackupResources.SelectableProfileBackupResource.key
        ] = { themeId: currentTheme.themeId };
      } else if (!replaceCurrentProfile && isLegacyBackup) {
        let themeId = await this.#getLegacyThemeId(recoveryPath);
        let { themeFg, themeBg } =
          lazy.SelectableProfileService.getColorsForDefaultTheme();
        await profile.setThemeAsync({ themeId, themeFg, themeBg });

        postRecovery[
          DefaultBackupResources.SelectableProfileBackupResource.key
        ] = { themeId };
      }

      restoreStep = RESTORE_STEPS.RESTORE_WRITE_POST_RECOVERY;
      await this.#writePostRecoveryData(postRecovery, profile.path);

      restoreStep = RESTORE_STEPS.RESTORE_LAUNCH_PROFILE;
      if (shouldLaunch) {

        lazy.SelectableProfileService.launchInstance(
          profile,
          [
            "about:editprofile" +
              (copiedProfile
                ? `#copiedProfileName=${copiedProfile.name}`
                : "#restoredProfile"),
          ]
        );
      }

      return profile;
    } catch (e) {
      lazy.logConsole.error(
        "Failed to recover SelectableProfile from backup at ",
        recoveryPath,
        e
      );
      let err =
        e instanceof BackupError
          ? e
          : new BackupError(
              `Recovery failed: ${e.message}`,
              ERRORS.RECOVERY_FAILED
            );
      err.restoreStep = err.restoreStep || restoreStep;
      throw err;
    }
  }

  async checkForPostRecovery(profilePath = PathUtils.profileDir) {
    lazy.logConsole.debug(`Checking for post-recovery file in ${profilePath}`);
    let postRecoveryFile = PathUtils.join(
      profilePath,
      BackupService.POST_RECOVERY_FILE_NAME
    );

    if (!(await IOUtils.exists(postRecoveryFile))) {
      lazy.logConsole.debug("Did not find post-recovery file.");
      this.#postRecoveryResolver();
      return;
    }

    lazy.logConsole.debug("Found post-recovery file. Loading...");

    try {
      let postRecovery = await IOUtils.readJSON(postRecoveryFile);
      for (let resourceKey in postRecovery) {
        let postRecoveryEntry = postRecovery[resourceKey];
        let resourceClass = this.#resources.get(resourceKey);
        if (!resourceClass) {
          lazy.logConsole.error(
            `Invalid resource for post-recovery step: ${resourceKey}`
          );
          continue;
        }

        lazy.logConsole.debug(`Running post-recovery step for ${resourceKey}`);
        try {
          await new resourceClass().postRecovery(postRecoveryEntry);
        } catch (e) {
          lazy.logConsole.error(
            `Post-recovery step for ${resourceKey} failed`,
            e
          );
        }
        lazy.logConsole.debug(`Done post-recovery step for ${resourceKey}`);
      }
    } finally {
      await IOUtils.remove(postRecoveryFile, {
        ignoreAbsent: true,
        retryReadonly: true,
      });
      this.#postRecoveryResolver();
    }
  }

  #getDesktopIni(LocalizedResourceName) {
    return (
      `\r\n` +
      `[.ShellClassInfo]\r\n` +
      `LocalizedResourceName=${LocalizedResourceName}\r\n`
    );
  }

  async #createDesktopIni(fullPath) {
    let desktopIni = PathUtils.join(fullPath, "desktop.ini");
    try {
      lazy.logConsole.debug(`Creating desktop.ini file: ${desktopIni}`);
      await IOUtils.writeUTF8(
        desktopIni,
        this.#getDesktopIni(BackupService.BACKUP_DIR_TRANSLATION),
        { compress: false }
      );

      await IOUtils.setWindowsAttributes(
        desktopIni,
        { system: true, hidden: true },
        false
      );

      await IOUtils.setWindowsAttributes(fullPath, { system: true }, false);

      return true;
    } catch (e) {
      lazy.logConsole.warn(`Could not create ${desktopIni}: ${e}`);
    }
    return false;
  }

  async maybeCleanupDesktopIni(fullPath) {
    try {
      let desktopIni = PathUtils.join(fullPath, "desktop.ini");
      lazy.logConsole.debug(
        `Attempting to delete desktop.ini: '${desktopIni}'`
      );
      if (await IOUtils.exists(desktopIni)) {
        let expectedContents = this.#getDesktopIni(
          BackupService.BACKUP_DIR_TRANSLATION
        );

        let fileInfo = await IOUtils.stat(desktopIni);
        if (fileInfo && fileInfo.size == expectedContents.length) {
          let currentContents = await IOUtils.readUTF8(desktopIni);
          if (currentContents == expectedContents) {
            await IOUtils.remove(desktopIni, { retryReadonly: false });
          }
        } else {
          throw new BackupError(
            "The desktop.ini file has been modified and differs in size:" +
              ` ${fileInfo.size} != ${expectedContents.length}: ${desktopIni}`
          );
        }

        await IOUtils.setWindowsAttributes(fullPath, { system: false }, false);
      }
    } catch (e) {
      lazy.logConsole.warn(
        `Unable to remove a desktop.ini file from ${fullPath}: ${e}`
      );
    }
  }

  async setParentDirPath(parentDirPath) {
    try {
      let filename = parentDirPath ? PathUtils.filename(parentDirPath) : null;
      if (!filename) {
        throw new BackupError(
          "Parent directory path is invalid.",
          ERRORS.FILE_SYSTEM_ERROR
        );
      }

      let fullPath = parentDirPath;
      if (filename != BackupService.BACKUP_DIR_NAME) {
        fullPath = PathUtils.join(parentDirPath, BackupService.BACKUP_DIR_NAME);
      }

      Services.prefs.setStringPref(BACKUP_DIR_PREF_NAME, fullPath);
    } catch (e) {
      lazy.logConsole.error(
        `Failed to set parent directory ${parentDirPath}. ${e}`
      );
      throw e;
    }
  }

  async onUpdateLocationDirPath(newDirPath) {
    lazy.logConsole.debug(`Updating backup location to ${newDirPath}`);


    this.#_state.backupDirPath = newDirPath;
    this.stateUpdate();
  }

  onUpdateBackupErrorCode(newErrorCode) {
    lazy.logConsole.debug(`Updating backup error code to ${newErrorCode}`);

    this.#_state.backupErrorCode = newErrorCode;
    this.stateUpdate();
  }

  onUpdateLastBackupFileName(newLastBackupFileName) {
    lazy.logConsole.debug(
      `The last backup file name is being updated to ${newLastBackupFileName}`
    );

    this.#_state.lastBackupFileName = newLastBackupFileName;

    if (!newLastBackupFileName) {
      lazy.logConsole.debug(
        `Looks like we've cleared the last backup file name, let's also clear the last backup date`
      );

      this.#_state.lastBackupDate = null;
      Services.prefs.clearUserPref(LAST_BACKUP_TIMESTAMP_PREF_NAME);
    }

    this.stateUpdate();
  }

  onUpdateProfilesEnabledState() {
    lazy.logConsole.debug(
      `The profiles enabled state was updated to ${lazy.SelectableProfileService.isEnabled}`
    );

    this.#_state.selectableProfilesAllowed =
      lazy.SelectableProfileService.isEnabled;
    this.stateUpdate();
  }

  getIconFromFilePath(path) {
    if (!path) {
      return null;
    }

    try {
      let fileURI = PathUtils.toFileURI(path);
      return `moz-icon:${fileURI}?size=16`;
    } catch (e) {
      return null;
    }
  }

  setScheduledBackups(shouldEnableScheduledBackups, source = "unknown") {
    this.#scheduledBackupsToggleSource = source || "unknown";

    Services.prefs.setBoolPref(
      SCHEDULED_BACKUPS_ENABLED_PREF_NAME,
      shouldEnableScheduledBackups
    );

    if (shouldEnableScheduledBackups) {
      Services.prefs.setIntPref(BACKUP_ERROR_CODE_PREF_NAME, ERRORS.NONE);

      this.setEmbeddedComponentPersistentData({});

      BackupService.maybeAddToEnabledListPref();
    } else {
      Services.prefs.setBoolPref(
        "browser.backup.scheduled.user-disabled",
        true
      );

      BackupService.maybeRemoveFromEnabledListPref();
    }
  }

  onUpdateScheduledBackups(isScheduledBackupsEnabled) {
    if (this.#_state.scheduledBackupsEnabled != isScheduledBackupsEnabled) {
      if (isScheduledBackupsEnabled) {
      } else {
      }
      this.#scheduledBackupsToggleSource = "unknown";

      lazy.logConsole.debug(
        "Updating scheduled backups",
        isScheduledBackupsEnabled
      );
      this.#_state.scheduledBackupsEnabled = isScheduledBackupsEnabled;
      this.stateUpdate();
    }
  }


  #loadEncryptionStatePromise = null;

  loadEncryptionState(profilePath = PathUtils.profileDir) {
    if (this.#encState !== undefined) {
      return Promise.resolve(this.#encState);
    }

    if (!this.#loadEncryptionStatePromise) {
      this.#loadEncryptionStatePromise = (async () => {
        let encState = null;
        let encStateFile = PathUtils.join(
          profilePath,
          BackupService.PROFILE_FOLDER_NAME,
          BackupService.ARCHIVE_ENCRYPTION_STATE_FILE
        );

        try {
          if (await IOUtils.exists(encStateFile)) {
            let stateObject = await IOUtils.readJSON(encStateFile);
            ({ instance: encState } =
              await lazy.ArchiveEncryptionState.initialize(stateObject));
          }
        } catch (e) {
          lazy.logConsole.error(
            "Failed to read / deserialize archive encryption state file: ",
            e
          );
        }

        this.#_state.encryptionEnabled = !!encState;
        this.stateUpdate();

        this.#encState = encState;
        return encState;
      })();
    }

    return this.#loadEncryptionStatePromise;
  }

  async enableEncryption(password, profilePath = PathUtils.profileDir) {
    lazy.logConsole.debug("Enabling encryption.");
    if (!password) {
      throw new BackupError(
        "Cannot supply a blank password.",
        ERRORS.INVALID_PASSWORD
      );
    }

    if (password.length < 8) {
      throw new BackupError(
        "Password must be at least 8 characters.",
        ERRORS.INVALID_PASSWORD
      );
    }

    let { instance: encState } =
      await lazy.ArchiveEncryptionState.initialize(password);
    if (!encState) {
      throw new BackupError(
        "Failed to construct ArchiveEncryptionState",
        ERRORS.UNKNOWN
      );
    }

    this.#encState = encState;

    let encStateFile = PathUtils.join(
      profilePath,
      BackupService.PROFILE_FOLDER_NAME,
      BackupService.ARCHIVE_ENCRYPTION_STATE_FILE
    );

    let stateObj = await encState.serialize();
    await IOUtils.writeJSON(encStateFile, stateObj);

    this.#_state.encryptionEnabled = true;
    this.stateUpdate();
  }

  async disableEncryption(profilePath = PathUtils.profileDir) {
    lazy.logConsole.debug("Disabling encryption.");
    let encStateFile = PathUtils.join(
      profilePath,
      BackupService.PROFILE_FOLDER_NAME,
      BackupService.ARCHIVE_ENCRYPTION_STATE_FILE
    );
    await IOUtils.remove(encStateFile, {
      ignoreAbsent: true,
      retryReadonly: true,
    });

    this.#encState = null;
    this.#_state.encryptionEnabled = false;
    this.stateUpdate();
  }

  #idleThresholdSeconds = null;

  #observer = null;

  #backupSchedulerInitted = false;

  initBackupScheduler() {
    if (this.#backupSchedulerInitted) {
      lazy.logConsole.warn(
        "BackupService scheduler already initting or initted."
      );
      return;
    }

    this.#backupSchedulerInitted = true;

    let lastBackupPrefValue = Services.prefs.getIntPref(
      LAST_BACKUP_TIMESTAMP_PREF_NAME,
      0
    );

    this.#_state.lastBackupDate = lastBackupPrefValue || null;

    this.stateUpdate();

    const FIVE_MINUTES_IN_SECONDS = 5 * 60;

    this.#idleThresholdSeconds = Services.prefs.getIntPref(
      IDLE_THRESHOLD_SECONDS_PREF_NAME,
      FIVE_MINUTES_IN_SECONDS
    );
    this.#observer = (subject, topic, data) => {
      this.onObserve(subject, topic, data);
    };
    lazy.logConsole.debug(
      `Registering idle observer for ${
        this.#idleThresholdSeconds
      } seconds of idle time`
    );
    lazy.idleService.addIdleObserver(
      this.#observer,
      this.#idleThresholdSeconds
    );
    lazy.logConsole.debug("Idle observer registered.");

    lazy.logConsole.debug(`Registering Places observer`);

    this.#placesObserver = new PlacesWeakCallbackWrapper(
      this.onPlacesEvents.bind(this)
    );
    PlacesObservers.addListener(
      ["history-cleared", "page-removed", "bookmark-removed"],
      this.#placesObserver
    );

    Services.obs.addObserver(this.#observer, "passwordmgr-storage-changed");
    Services.obs.addObserver(this.#observer, "formautofill-storage-changed");
    Services.obs.addObserver(this.#observer, "sanitizer-sanitization-complete");
    Services.obs.addObserver(this.#observer, "perm-changed");
    Services.obs.addObserver(this.#observer, "cookie-changed");
    Services.obs.addObserver(this.#observer, "session-cookie-changed");
    Services.obs.addObserver(this.#observer, "newtab-linkBlocked");
    Services.obs.addObserver(this.#observer, "quit-application-granted");
    Services.prefs.addObserver(SANITIZE_ON_SHUTDOWN_PREF_NAME, this.#observer);
  }

  uninitBackupScheduler() {
    if (!this.#backupSchedulerInitted) {
      lazy.logConsole.warn(
        "Tried to uninitBackupScheduler when it wasn't yet enabled."
      );
      return;
    }

    lazy.idleService.removeIdleObserver(
      this.#observer,
      this.#idleThresholdSeconds
    );

    PlacesObservers.removeListener(
      ["history-cleared", "page-removed", "bookmark-removed"],
      this.#placesObserver
    );

    Services.obs.removeObserver(this.#observer, "passwordmgr-storage-changed");
    Services.obs.removeObserver(this.#observer, "formautofill-storage-changed");
    Services.obs.removeObserver(
      this.#observer,
      "sanitizer-sanitization-complete"
    );
    Services.obs.removeObserver(this.#observer, "perm-changed");
    Services.obs.removeObserver(this.#observer, "cookie-changed");
    Services.obs.removeObserver(this.#observer, "session-cookie-changed");
    Services.obs.removeObserver(this.#observer, "newtab-linkBlocked");
    Services.obs.removeObserver(this.#observer, "quit-application-granted");
    Services.prefs.removeObserver(
      SANITIZE_ON_SHUTDOWN_PREF_NAME,
      this.#observer
    );
    this.#observer = null;

    this.#regenerationDebouncer.disarm();
    this.#backupWriteAbortController.abort();
  }

  onObserve(subject, topic, data) {
    switch (topic) {
      case "idle": {
        this.onIdle();
        break;
      }
      case "quit-application-granted": {
        this.uninitBackupScheduler();
        this.uninitStatusObservers();
        break;
      }
      case "passwordmgr-storage-changed": {
        if (data == "removeLogin" || data == "removeAllLogins") {
          this.#debounceRegeneration();
        }
        break;
      }
      case "formautofill-storage-changed": {
        if (
          data == "remove" &&
          (subject.wrappedJSObject.collectionName == "creditCards" ||
            subject.wrappedJSObject.collectionName == "addresses")
        ) {
          this.#debounceRegeneration();
        }
        break;
      }
      case "newtab-linkBlocked":
      case "sanitizer-sanitization-complete": {
        this.#debounceRegeneration();
        break;
      }
      case "perm-changed": {
        if (data == "deleted") {
          this.#debounceRegeneration();
        }
        break;
      }
      case "cookie-changed":
      case "session-cookie-changed": {
        let notification = subject.QueryInterface(Ci.nsICookieNotification);
        if (
          (notification.action == Ci.nsICookieNotification.COOKIE_DELETED ||
            notification.action ==
              Ci.nsICookieNotification.ALL_COOKIES_CLEARED) &&
          !notification.browsingContextId
        ) {
          this.#debounceRegeneration();
        }
        break;
      }
      case "nsPref:changed": {
        if (data == SANITIZE_ON_SHUTDOWN_PREF_NAME) {
          this.#debounceRegeneration();
        }
      }
    }
  }

  initStatusObservers() {
    if (this.#statusPrefObserver != null) {
      return;
    }

    this.#statusPrefObserver = () => {
      this.#handleStatusChange();
    };

    for (let pref of BackupService.STATUS_OBSERVER_PREFS) {
      Services.prefs.addObserver(pref, this.#statusPrefObserver);
    }
    lazy.NimbusFeatures.backupService.onUpdate(this.#statusPrefObserver);
    this.#handleStatusChange();

    this.#profileServiceStateObserver = () =>
      this.onUpdateProfilesEnabledState();
    lazy.SelectableProfileService.on(
      "enableChanged",
      this.#profileServiceStateObserver
    );
  }

  uninitStatusObservers() {
    if (this.#statusPrefObserver == null) {
      return;
    }

    for (let pref of BackupService.STATUS_OBSERVER_PREFS) {
      Services.prefs.removeObserver(pref, this.#statusPrefObserver);
    }
    lazy.NimbusFeatures.backupService.offUpdate(this.#statusPrefObserver);
    this.#statusPrefObserver = null;

    lazy.SelectableProfileService.off(
      "enableChanged",
      this.#profileServiceStateObserver
    );
    this.#profileServiceStateObserver = null;
  }

  #handleStatusChange() {
    const archiveStatus = this.archiveEnabledStatus;
    const restoreStatus = this.restoreEnabledStatus;
    this.#_state.archiveEnabledStatus = this.archiveEnabledStatus.enabled;
    this.#_state.restoreEnabledStatus = this.restoreEnabledStatus.enabled;

    if (
      archiveStatus.enabled != this.#lastSeenArchiveStatus ||
      restoreStatus.enabled != this.#lastSeenRestoreStatus
    ) {
      this.#lastSeenArchiveStatus = archiveStatus.enabled;
      this.#lastSeenRestoreStatus = restoreStatus.enabled;
      this.#notifyStatusObservers();
    }
    if (!archiveStatus.enabled) {
      this.cleanupBackupFiles();
    }
  }

  #notifyStatusObservers() {
    lazy.logConsole.log(
      "Notifying observers about a BackupService state change"
    );

    Services.obs.notifyObservers(null, "backup-service-status-updated");
  }

  async cleanupBackupFiles() {
    lazy.logConsole.debug("Cleaning up backup data");
    try {
      if (this.state.encryptionEnabled) {
        await this.disableEncryption();
      }
      await this.deleteLastBackup();
    } catch (e) {
      lazy.logConsole.error(
        "There was an error when cleaning up backup files: ",
        e
      );
    }
  }

  #debounceRegeneration() {
    this.#regenerationDebouncer.disarm();
    this.#regenerationDebouncer.arm();
  }

  async onIdle() {
    lazy.logConsole.debug("Saw idle callback");
    if (lazy.scheduledBackupsPref && this.archiveEnabledStatus.enabled) {
      lazy.logConsole.debug("Scheduled backups enabled.");
      let now = Math.floor(Date.now() / 1000);
      let lastBackupDate = this.#_state.lastBackupDate;
      if (lastBackupDate && lastBackupDate > now) {
        lazy.logConsole.error(
          "Last backup was somehow in the future. Resetting the preference."
        );
        lastBackupDate = null;
        this.#_state.lastBackupDate = null;
        this.stateUpdate();
      }

      if (!lastBackupDate) {
        lazy.logConsole.debug("No last backup time recorded in prefs.");
      } else {
        lazy.logConsole.debug(
          "Last backup was: ",
          new Date(lastBackupDate * 1000)
        );
      }

      if (
        !lastBackupDate ||
        now - lastBackupDate > lazy.minimumTimeBetweenBackupsSeconds
      ) {
        lazy.logConsole.debug(
          "Last backup exceeded minimum time between backups. Queueing a " +
            "backup via idleDispatch."
        );

        let expectedBackupTime =
          lastBackupDate + lazy.minimumTimeBetweenBackupsSeconds;
        try {
          await this.createBackupOnIdleDispatch({
            reason:
              expectedBackupTime < this._startupTimeUnixSeconds
                ? "missed"
                : "idle",
          });
        } catch (e) {
          lazy.logConsole.error(
            "createBackupOnIdleDispatch promise rejected",
            e
          );
        }
      } else {
        lazy.logConsole.debug(
          "Last backup was too recent. Not creating one for now."
        );
      }
    }
  }

  get _startupTimeUnixSeconds() {
    let startupTimeMs = Services.startup.getStartupInfo().process.getTime();
    return Math.floor(startupTimeMs / 1000);
  }

  shouldAttemptBackup() {
    let now = Math.floor(Date.now() / 1000);
    const debugInfoStr = Services.prefs.getStringPref(
      BACKUP_DEBUG_INFO_PREF_NAME,
      ""
    );

    let parsed = null;
    if (debugInfoStr) {
      try {
        parsed = JSON.parse(debugInfoStr);
      } catch (e) {
        lazy.logConsole.warn(
          "Invalid backup debug-info pref; ignoring and allowing backup attempt.",
          e
        );
        parsed = null;
      }
    }

    const lastBackupAttempt = parsed?.lastBackupAttempt;
    const hasErroredLastAttempt = Number.isFinite(lastBackupAttempt);

    if (!hasErroredLastAttempt) {
      lazy.logConsole.debug(
        `There have been no errored last attempts, let's do a backup`
      );
      return true;
    }

    const secondsSinceLastAttempt = now - lastBackupAttempt;

    if (lazy.isRetryDisabledOnIdle) {
      if (secondsSinceLastAttempt < lazy.minimumTimeBetweenBackupsSeconds / 2) {
        lazy.logConsole.debug(
          `Retrying is disabled, we have to wait for ${lazy.minimumTimeBetweenBackupsSeconds / 2}s to retry`
        );
        return false;
      }
      BackupService.#errorRetries = 0;
      Services.prefs.clearUserPref(DISABLED_ON_IDLE_RETRY_PREF_NAME);

      return true;
    }

    if (secondsSinceLastAttempt < BackupService.backoffSeconds()) {
      lazy.logConsole.debug(
        `backoff: elapsed ${secondsSinceLastAttempt}s < backoff ${BackupService.backoffSeconds()}s`
      );
      return false;
    }

    return true;
  }

  createBackupOnIdleDispatch({ deletePreviousBackup = true, reason }) {
    if (!this.shouldAttemptBackup()) {
      return Promise.resolve();
    }

    const oldBackupFile = this.#_state.lastBackupFileName;
    const isScheduledBackupsEnabled = lazy.scheduledBackupsPref;

    let { promise, resolve } = Promise.withResolvers();
    ChromeUtils.idleDispatch(async () => {
      lazy.logConsole.debug(
        "idleDispatch fired. Attempting to create a backup."
      );
      let oldBackupFilePath;
      if (await this.#infalliblePathExists(lazy.backupDirPref)) {
        oldBackupFilePath = PathUtils.join(lazy.backupDirPref, oldBackupFile);
      }

      let possibleArchivePath = "";

      try {
        if (isScheduledBackupsEnabled) {
          ({ archivePath: possibleArchivePath } = await this.createBackup({
            reason,
          }));
        }
      } catch (e) {
        lazy.logConsole.debug(
          `There was an error creating backup on idle dispatch: ${e}`
        );

        BackupService.#errorRetries += 1;
        if (BackupService.#errorRetries > lazy.backupRetryLimit) {
          Services.prefs.setBoolPref(DISABLED_ON_IDLE_RETRY_PREF_NAME, true);
          BackupService.#errorRetries = 0;
        }
      } finally {
        if (
          deletePreviousBackup &&
          oldBackupFilePath &&
          oldBackupFilePath != possibleArchivePath
        ) {
          lazy.logConsole.log(
            "Attempting to delete last backup file at ",
            oldBackupFilePath
          );
          await this.maybeCleanupDesktopIni(lazy.backupDirPref);
          await IOUtils.remove(oldBackupFilePath, {
            ignoreAbsent: true,
            retryReadonly: true,
          });
        }
        resolve();
      }
    });
    return promise;
  }

  onPlacesEvents(placesEvents) {
    for (let event of placesEvents) {
      switch (event.type) {
        case "page-removed": {
          if (event.reason == PlacesVisitRemoved.REASON_DELETED) {
            this.#debounceRegeneration();
            return;
          }
          break;
        }
        case "bookmark-removed":
        case "history-cleared": {
          this.#debounceRegeneration();
          return;
        }
      }
    }
  }

  async getBackupFileInfo(backupFilePath) {
    lazy.logConsole.debug(`Getting info from backup file at ${backupFilePath}`);

    this.#_state.backupFileInfo = null;
    this.#_state.backupFileToRestore = backupFilePath;

    try {
      let { archiveJSON, isEncrypted } =
        await this.sampleArchive(backupFilePath);
      this.#_state.backupFileInfo = {
        isEncrypted,
        date: archiveJSON?.meta?.date,
        deviceName: archiveJSON?.meta?.deviceName,
        appName: archiveJSON?.meta?.appName,
        appVersion: archiveJSON?.meta?.appVersion,
        buildID: archiveJSON?.meta?.buildID,
        osName: archiveJSON?.meta?.osName,
        osVersion: archiveJSON?.meta?.osVersion,
        osBuildNumber: archiveJSON?.meta?.osBuildNumber,
        profileName: archiveJSON?.meta?.profileName,
      };

      this.setRecoveryError(ERRORS.NONE);
    } catch (error) {
      this.#_state.backupFileInfo = null;

      this.setRecoveryError(error.cause);
    }
  }

  resetLastBackupInternalState() {
    this.#_state.backupFileToRestore = null;
    this.#_state.backupFileInfo = null;
    this.#_state.lastBackupFileName = "";
    this.#_state.lastBackupDate = null;
    this.stateUpdate();
  }

  resetDefaultParentInternalState() {
    this.#_state.defaultParent = {};
    this.stateUpdate();
  }

  async showBackupLocation() {
    let backupFilePath = PathUtils.join(
      lazy.backupDirPref,
      lazy.lastBackupFileName
    );
    if (await IOUtils.exists(backupFilePath)) {
      new lazy.nsLocalFile(backupFilePath).reveal();
    } else {
      let archiveDestFolderPath = await this.resolveArchiveDestFolderPath(
        lazy.backupDirPref
      );
      new lazy.nsLocalFile(archiveDestFolderPath).reveal();
    }
  }

  async findIfABackupFileExists({
    validateFile = true,
    multipleFiles = false,
    speedUpHeuristic = false,
  } = {}) {
    if (lazy.lastBackupFileName && !multipleFiles) {
      return {
        multipleBackupsFound: false,
        count: 1,
      };
    }

    try {
      let backupPaths = new Set();

      if (this.#_state.backupDirPath) {
        backupPaths.add(this.#_state.backupDirPath);
      }

      for (let dirPath of [
        BackupService.docsDirFolderPath?.path,
        BackupService.oneDriveFolderPath?.path,
      ]) {
        if (dirPath) {
          backupPaths.add(
            PathUtils.join(dirPath, BackupService.BACKUP_DIR_NAME)
          );
        }
      }

      let files = [];
      let anyPathSucceeded = false;

      for (let backupPath of backupPaths) {
        try {
          files.push(
            ...(await IOUtils.getChildren(backupPath, { ignoreAbsent: true }))
          );
          anyPathSucceeded = true;
        } catch (e) {
          lazy.logConsole.error(
            "Could not read backup directory: ",
            backupPath,
            e
          );
        }
      }

      if (!anyPathSucceeded && backupPaths.size) {
        this.#_state.backupFileToRestore = null;
        this.#_state.backupFileInfo = null;
        this.stateUpdate();
        return { multipleBackupsFound: false, count: 0 };
      }

      if (speedUpHeuristic && files && files.length > 1000) {
        return {
          multipleBackupsFound: false,
          count: 0,
        };
      }

      let maybeBackupFiles = files.filter(f => {
        let name = PathUtils.filename(f);

        return /^FirefoxBackup_.*\.html$/.test(name);
      });

      if (!multipleFiles && maybeBackupFiles.length > 1 && !validateFile) {
        return { multipleBackupsFound: true, count: maybeBackupFiles.length };
      }

      if (multipleFiles && maybeBackupFiles.length > 1 && validateFile) {
        maybeBackupFiles.sort((a, b) => {
          let nameA = PathUtils.filename(a);
          let nameB = PathUtils.filename(b);
          const match = /_(\d{8}-\d{6}\.\d{3})\.html$/;
          let timestampA = nameA.match(match)?.[1];
          let timestampB = nameB.match(match)?.[1];

          if (!timestampA || !timestampB) {
            return 0;
          }

          return timestampB.localeCompare(timestampA);
        });
      }

      for (const file of maybeBackupFiles) {
        if (validateFile) {
          try {
            await this.getBackupFileInfo(file);
          } catch (e) {
            lazy.logConsole.log(
              "Not a valid backup file in the default folder",
              file,
              e
            );

            if (this.#_state.backupFileToRestore === file) {
              this.#_state.backupFileToRestore = null;
              this.#_state.backupFileInfo = null;
              this.stateUpdate();
            }

            continue;
          }
        }

        this.#_state.backupFileToRestore = file;
        this.stateUpdate();

        if (multipleFiles && maybeBackupFiles.length > 1 && validateFile) {
          return {
            multipleBackupsFound: true,
            count: maybeBackupFiles.length,
          };
        }

        return { multipleBackupsFound: false, count: maybeBackupFiles.length };
      }
    } catch (e) {
      lazy.logConsole.error(
        "There was an error while looking for backups: ",
        e
      );
    }

    return { multipleBackupsFound: false, count: 0 };
  }

  async findBackupsInWellKnownLocations({
    validateFile = true,
    multipleFiles = true,
  } = {}) {
    this.#_state.backupFileToRestore = null;

    let { multipleBackupsFound } = await this.findIfABackupFileExists({
      validateFile,
      multipleFiles,
    });

    let found = !!this.#_state.backupFileToRestore;

    if (found) {
      return {
        found: true,
        backupFileToRestore: this.#_state.backupFileToRestore,
        multipleBackupsFound,
      };
    }
    return { found: false, backupFileToRestore: null, multipleBackupsFound };
  }

  async editBackupLocation(path) {
    try {
      await this.deleteLastBackup();
    } catch {
      lazy.logConsole.error(
        "Error deleting last backup while editing the backup location."
      );
    }
    await this.setParentDirPath(path);
  }

  async deleteLastBackup() {
    if (!lazy.scheduledBackupsPref) {
      lazy.logConsole.debug(
        "Not deleting last backup, as scheduled backups are disabled."
      );
      return undefined;
    }

    return locks.request(
      BackupService.WRITE_BACKUP_LOCK_NAME,
      { signal: this.#backupWriteAbortController.signal },
      async () => {
        if (lazy.lastBackupFileName) {
          if (await this.#infalliblePathExists(lazy.backupDirPref)) {
            let backupFilePath = PathUtils.join(
              lazy.backupDirPref,
              lazy.lastBackupFileName
            );

            lazy.logConsole.log(
              "Attempting to delete last backup file at ",
              backupFilePath
            );
            await IOUtils.remove(backupFilePath, {
              ignoreAbsent: true,
              retryReadonly: true,
            });
          }

          Services.prefs.clearUserPref(LAST_BACKUP_FILE_NAME_PREF_NAME);
        } else {
          lazy.logConsole.log(
            "Not deleting last backup file, since none is known about."
          );
        }

        if (await this.#infalliblePathExists(lazy.backupDirPref)) {
          await this.maybeCleanupDesktopIni(lazy.backupDirPref);

          let children = await IOUtils.getChildren(lazy.backupDirPref);
          if (!children.length) {
            await IOUtils.remove(lazy.backupDirPref, { retryReadony: true });
          }
        }
      }
    );
  }

  async #infalliblePathExists(path) {
    if (!path) {
      return false;
    }
    let exists = false;
    try {
      exists = await IOUtils.exists(path);
    } catch (e) {
      lazy.logConsole.warn("Path failed existence check :", path);
      return false;
    }
    return exists;
  }

  static maybeAddToEnabledListPref(
    profileID = lazy.SelectableProfileService.currentProfile?.id
  ) {
    if (!lazy.SelectableProfileService.currentProfile) {
      lazy.logConsole.warn(
        "The enabled pref is only to be used for selectable profiles"
      );
      return;
    }

    let profilesEnabledOn = [...lazy.enabledOnProfilesPref];

    if (!profilesEnabledOn.includes(profileID)) {
      profilesEnabledOn.push(profileID);
    }

    Services.prefs.setStringPref(
      BACKUP_ENABLED_ON_PROFILES_PREF_NAME,
      JSON.stringify(profilesEnabledOn)
    );
  }

  static async maybeRemoveFromEnabledListPref(
    profileID = lazy.SelectableProfileService.currentProfile?.id
  ) {
    if (!lazy.SelectableProfileService.currentProfile) {
      lazy.logConsole.warn(
        "The enabled pref is only to be used for selectable profiles"
      );
      return;
    }

    let profilesEnabledOn = lazy.enabledOnProfilesPref.filter(
      id => id !== profileID
    );
    Services.prefs.setStringPref(
      BACKUP_ENABLED_ON_PROFILES_PREF_NAME,
      JSON.stringify(profilesEnabledOn)
    );

    await lazy.SelectableProfileService.flushSharedPrefToDatabase(
      BACKUP_ENABLED_ON_PROFILES_PREF_NAME
    );
  }
}
