/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { Integration } from "resource://gre/modules/Integration.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  DownloadHistory: "resource://gre/modules/DownloadHistory.sys.mjs",
  DownloadPaths: "resource://gre/modules/DownloadPaths.sys.mjs",
  E10SUtils: "resource://gre/modules/E10SUtils.sys.mjs",
  FileUtils: "resource://gre/modules/FileUtils.sys.mjs",
  NetUtil: "resource://gre/modules/NetUtil.sys.mjs",
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "gExternalAppLauncher",
  "@mozilla.org/uriloader/external-helper-app-service;1",
  Ci.nsPIExternalAppLauncher
);
XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "gExternalHelperAppService",
  "@mozilla.org/uriloader/external-helper-app-service;1",
  Ci.nsIExternalHelperAppService
);

Integration.downloads.defineESModuleGetter(
  lazy,
  "DownloadIntegration",
  "resource://gre/modules/DownloadIntegration.sys.mjs"
);

const BackgroundFileSaverStreamListener = Components.Constructor(
  "@mozilla.org/network/background-file-saver;1?mode=streamlistener",
  "nsIBackgroundFileSaver"
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "enableDeletePrivateFeature",
  "browser.download.enableDeletePrivate",
  false
);

function isString(aValue) {
  return (
    typeof aValue == "string" ||
    (typeof aValue == "object" && "charAt" in aValue)
  );
}

function serializeUnknownProperties(aObject, aSerializable) {
  if (aObject._unknownProperties) {
    for (let property in aObject._unknownProperties) {
      aSerializable[property] = aObject._unknownProperties[property];
    }
  }
}

function deserializeUnknownProperties(aObject, aSerializable, aFilterFn) {
  for (let property in aSerializable) {
    if (aFilterFn(property)) {
      if (!aObject._unknownProperties) {
        aObject._unknownProperties = {};
      }

      aObject._unknownProperties[property] = aSerializable[property];
    }
  }
}

async function isPlaceholder(path) {
  try {
    if ((await IOUtils.stat(path)).size == 0) {
      return true;
    }
  } catch (ex) {
    if (ex.name != "NotFoundError") {
      console.error(ex);
    }
  }
  return false;
}

const kProgressUpdateIntervalMs = 400;

const gPublicBatch = new Set(),
  gPrivateBatch = new Set();

export var Download = function () {
  this._deferSucceeded = Promise.withResolvers();
};

Download.prototype = {
  source: null,

  target: null,

  saver: null,

  stopped: true,

  succeeded: false,

  canceled: false,

  deleted: false,

  error: null,

  startTime: null,

  hasProgress: false,

  progress: 0,

  totalBytes: 0,

  currentBytes: 0,

  speed: 0,

  hasPartialData: false,

  onchange: null,

  launchWhenSucceeded: false,

  openDownloadsListOnStart: true,

  contentType: null,

  launcherPath: null,

  launcherId: null,

  get isInCurrentBatch() {
    return this._batch !== null;
  },

  _batch: null,

  _notifyChange: function D_notifyChange() {
    try {
      if (this.onchange) {
        this.onchange();
      }
    } catch (ex) {
      console.error(ex);
    }
  },

  _currentAttempt: null,

  _launchedFromPanel: false,

  start: function D_start() {
    if (this.succeeded) {
      return Promise.resolve();
    }

    if (this._currentAttempt) {
      return this._currentAttempt;
    }

    if (this._finalized) {
      return Promise.reject(
        new DownloadError({
          message: "Cannot start after finalization.",
        })
      );
    }

    this.stopped = false;
    this.canceled = false;
    if (!this._batch) {
      this._batch = this.source.isPrivate ? gPrivateBatch : gPublicBatch;
      this._batch.add(this);
    }
    this.error = null;
    delete this._unknownProperties?.errorObj;
    this.hasProgress = false;
    this.progress = 0;
    this.totalBytes = 0;
    this.currentBytes = 0;
    this.startTime = new Date();

    let deferAttempt = Promise.withResolvers();
    let currentAttempt = deferAttempt.promise;
    this._currentAttempt = currentAttempt;

    this._lastProgressTimeMs = 0;

    function DS_setProgressBytes(aCurrentBytes, aTotalBytes, aHasPartialData) {
      if (this._currentAttempt == currentAttempt) {
        this._setBytes(aCurrentBytes, aTotalBytes, aHasPartialData);
      }
    }

    function DS_setProperties(aOptions) {
      if (this._currentAttempt != currentAttempt) {
        return;
      }

      let changeMade = false;

      for (let property of [
        "contentType",
        "progress",
        "hasPartialData",
      ]) {
        if (property in aOptions && this[property] != aOptions[property]) {
          this[property] = aOptions[property];
          changeMade = true;
        }
      }

      if (changeMade) {
        this._notifyChange();
      }
    }

    deferAttempt.resolve(
      (async () => {
        if (this._promiseCanceled) {
          await this._promiseCanceled;
        }
        if (this._promiseRemovePartialData) {
          try {
            await this._promiseRemovePartialData;
          } catch (ex) {
          }
        }

        if (this.succeeded) {
          return;
        }

        try {
          if (this.downloadingToSameFile()) {
            throw new DownloadError({
              message: "Can't overwrite the source file.",
              becauseTargetFailed: true,
            });
          }

          if (this._promiseCanceled) {
            throw new Error(undefined);
          }

          this._saverExecuting = true;
          try {
            await this.saver.execute(
              DS_setProgressBytes.bind(this),
              DS_setProperties.bind(this)
            );
          } catch (ex) {
            if (!this.hasPartialData) {
              await this.saver.removeData(true);
            }
            throw ex;
          }

          await this.target.refresh();

          if (this._promiseCanceled) {
            await this.saver.removeData(true);

            throw new DownloadError();
          }

          this.progress = 100;
          this.succeeded = true;
          this.hasPartialData = false;
        } catch (originalEx) {
          let ex = originalEx;

          if (this._promiseCanceled) {
            throw new DownloadError({ message: "Download canceled." });
          }

          if (this._currentAttempt == currentAttempt || !this._currentAttempt) {
            if (!(ex instanceof DownloadError)) {
              let properties = { innerException: ex };

              if (ex.message) {
                properties.message = ex.message;
              }

              ex = new DownloadError(properties);
            }
            if (
              originalEx.result != Cr.NS_ERROR_ABORT ||
              !Services.startup.isInOrBeyondShutdownPhase(
                Ci.nsIAppStartup.SHUTDOWN_PHASE_APPSHUTDOWNCONFIRMED
              )
            ) {
              this.error = ex;
            }
          }
          throw ex;
        } finally {
          this._saverExecuting = false;
          this._promiseCanceled = null;

          if (this._currentAttempt == currentAttempt || !this._currentAttempt) {
            this._currentAttempt = null;
            this.stopped = true;
            this.speed = 0;
            if (!this._batch || Download._updateBatch(this._batch)) {
              this._notifyChange();
            }
            if (this.succeeded) {
              await this._succeed();
            }
          }
        }
      })()
    );

    this._notifyChange();
    return currentAttempt;
  },

  async _succeed() {
    await lazy.DownloadIntegration.downloadDone(this);

    this._deferSucceeded.resolve();

    if (this.launchWhenSucceeded) {
      this.launch().catch(console.error);

      if (!lazy.enableDeletePrivateFeature && this.source.isPrivate) {
        lazy.gExternalAppLauncher.deleteTemporaryPrivateFileWhenPossible(
          new lazy.FileUtils.File(this.target.path)
        );
      }
      if (
        !this.source.isPrivate &&
        Services.prefs.getBoolPref("browser.helperApps.deleteTempFileOnExit") &&
        Services.prefs.getBoolPref(
          "browser.download.start_downloads_in_tmp_dir",
          false
        )
      ) {
        lazy.gExternalAppLauncher.deleteTemporaryFileOnExit(
          new lazy.FileUtils.File(this.target.path)
        );
      }
    }

    if (
      lazy.enableDeletePrivateFeature &&
      Services.prefs.getBoolPref("browser.download.deletePrivate", false) &&
      this.source.isPrivate
    ) {
      lazy.gExternalAppLauncher.deletePrivateFileWhenPossible(
        new lazy.FileUtils.File(this.target.path)
      );
    }
  },


  launch(options = {}) {
    if (!this.succeeded) {
      return Promise.reject(
        new Error("launch can only be called if the download succeeded")
      );
    }

    if (this._launchedFromPanel) {
    }

    return lazy.DownloadIntegration.launchDownload(this, options);
  },

  showContainingDirectory: function D_showContainingDirectory() {
    return lazy.DownloadIntegration.showContainingDirectory(this.target.path);
  },

  _promiseCanceled: null,

  _saverExecuting: false,

  cancel: function D_cancel() {
    if (this.stopped) {
      return Promise.resolve();
    }

    if (!this._promiseCanceled) {
      this._promiseCanceled = new Promise(resolve => {
        this._currentAttempt.then(resolve, resolve);
      });

      this._currentAttempt = null;

      this.canceled = true;
      let batch = this._batch;
      this._batch = null;
      batch.delete(this);
      Download._updateBatch(batch);
      this._notifyChange();

      if (this._saverExecuting) {
        this.saver.cancel();
      }
    }

    return this._promiseCanceled;
  },

  tryToKeepPartialData: false,

  _promiseRemovePartialData: null,

  removePartialData() {
    if (!this.canceled && !this.error) {
      return Promise.resolve();
    }

    if (!this._promiseRemovePartialData) {
      this._promiseRemovePartialData = (async () => {
        try {
          if (this._promiseCanceled) {
            await this._promiseCanceled;
          }
          await this.saver.removeData();
          if (this.currentBytes != 0 || this.hasPartialData) {
            this.currentBytes = 0;
            this.hasPartialData = false;
            this.target.refreshPartFileState();
            this._notifyChange();
          }
        } finally {
          this._promiseRemovePartialData = null;
        }
      })();
    }

    return this._promiseRemovePartialData;
  },

  downloadingToSameFile() {
    if (!this.source.url || !this.source.url.startsWith("file:")) {
      return false;
    }

    try {
      let sourceUri = lazy.NetUtil.newURI(this.source.url);
      let targetUri = lazy.NetUtil.newURI(
        new lazy.FileUtils.File(this.target.path)
      );
      return sourceUri.equals(targetUri);
    } catch (ex) {
      return false;
    }
  },

  _deferSucceeded: null,

  whenSucceeded: function D_whenSucceeded() {
    return this._deferSucceeded.promise;
  },

  refresh() {
    return (async () => {
      if (!this.stopped || this._finalized) {
        return;
      }

      if (this.succeeded) {
        let oldExists = this.target.exists;
        let oldSize = this.target.size;
        await this.target.refresh();
        if (oldExists != this.target.exists || oldSize != this.target.size) {
          this._notifyChange();
        }
        return;
      }

      if (this.hasPartialData && this.target.partFilePath) {
        try {
          let stat = await IOUtils.stat(this.target.partFilePath);

          if (!this.stopped || this._finalized) {
            return;
          }

          this.currentBytes = stat.size;
          if (this.totalBytes > 0) {
            this.hasProgress = true;
            this.progress = Math.floor(
              (this.currentBytes / this.totalBytes) * 100
            );
          }
        } catch (ex) {
          if (ex.name != "NotFoundError") {
            throw ex;
          }
          if (!this.stopped || this._finalized) {
            return;
          }
          this.hasPartialData = false;
        }

        this._notifyChange();
      }
    })().catch(console.error);
  },

  _finalized: false,

  _finalizeExecuted: false,

  finalize(aRemovePartialData) {
    this._finalized = true;
    let promise;

    if (aRemovePartialData) {
      this.cancel();
      promise = this.removePartialData();
    } else {
      promise = this.cancel();
    }
    promise.then(() => {
      this._finalizeExecuted = true;
    });

    return promise;
  },

  async manuallyRemoveData() {
    let { path } = this.target;
    if (this.succeeded) {
      await IOUtils.setPermissions(path, 0o660);
      await IOUtils.remove(path, { ignoreAbsent: true });
    }
    this.deleted = true;
    await this.cancel();
    await this.removePartialData();
    await this.target.refreshPartFileState();
    await this.refresh();
    this._notifyChange();
  },

  _lastProgressTimeMs: 0,

  _setBytes: function D_setBytes(
    aCurrentBytes,
    aTotalBytes,
    aHasPartialData = false
  ) {
    let changeMade = this.hasPartialData != aHasPartialData;
    this.hasPartialData = aHasPartialData;

    if (
      aTotalBytes != -1 &&
      (!this.hasProgress || this.totalBytes != aTotalBytes)
    ) {
      this.hasProgress = true;
      this.totalBytes = aTotalBytes;
      changeMade = true;
    }

    let currentTimeMs = Date.now();
    let intervalMs = currentTimeMs - this._lastProgressTimeMs;
    if (intervalMs >= kProgressUpdateIntervalMs) {
      if (this._lastProgressTimeMs != 0) {
        let rawSpeed =
          ((aCurrentBytes - this.currentBytes) / intervalMs) * 1000;
        if (this.speed == 0) {
          this.speed = rawSpeed;
        } else {
          this.speed = rawSpeed * 0.1 + this.speed * 0.9;
        }
      }

      if (aCurrentBytes > 0) {
        this._lastProgressTimeMs = currentTimeMs;

        this.currentBytes = aCurrentBytes;
        if (this.totalBytes > 0) {
          this.progress = Math.floor(
            (this.currentBytes / this.totalBytes) * 100
          );
        }
        changeMade = true;
      }

      if (this.hasProgress && this.target && !this.target.partFileExists) {
        this.target.refreshPartFileState();
      }
    }

    if (changeMade) {
      this._notifyChange();
    }
  },

  toSerializable() {
    let serializable = {
      source: this.source.toSerializable(),
      target: this.target.toSerializable(),
    };

    let saver = this.saver.toSerializable();
    if (!serializable.source || !saver) {
      return null;
    }

    if (saver !== "copy") {
      serializable.saver = saver;
    }

    if (this.error) {
      serializable.errorObj = this.error.toSerializable();
    }

    if (this.startTime) {
      serializable.startTime = this.startTime.toJSON();
    }

    for (let property of kPlainSerializableDownloadProperties) {
      if (this[property]) {
        serializable[property] = this[property];
      }
    }

    serializeUnknownProperties(this, serializable);

    return serializable;
  },

  getSerializationHash() {
    return (
      this.stopped +
      "," +
      this.totalBytes +
      "," +
      this.hasPartialData +
      "," +
      this.contentType
    );
  },
};

const kPlainSerializableDownloadProperties = [
  "succeeded",
  "canceled",
  "deleted",
  "totalBytes",
  "hasPartialData",
  "tryToKeepPartialData",
  "launcherPath",
  "launcherId",
  "launchWhenSucceeded",
  "contentType",
  "handleInternally",
  "openDownloadsListOnStart",
];

Download.fromSerializable = function (aSerializable) {
  let download = new Download();
  if (aSerializable.source instanceof DownloadSource) {
    download.source = aSerializable.source;
  } else {
    download.source = DownloadSource.fromSerializable(aSerializable.source);
  }
  if (aSerializable.target instanceof DownloadTarget) {
    download.target = aSerializable.target;
  } else {
    download.target = DownloadTarget.fromSerializable(aSerializable.target);
  }
  if ("saver" in aSerializable) {
    download.saver = DownloadSaver.fromSerializable(aSerializable.saver);
  } else {
    download.saver = DownloadSaver.fromSerializable("copy");
  }
  download.saver.download = download;

  if ("startTime" in aSerializable) {
    let time = aSerializable.startTime.getTime
      ? aSerializable.startTime.getTime()
      : aSerializable.startTime;
    download.startTime = new Date(time);
  }

  if ("errorObj" in aSerializable) {
    download.error = DownloadError.fromSerializable(aSerializable.errorObj);
  } else if ("error" in aSerializable) {
    download.error = aSerializable.error;
  }

  for (let property of kPlainSerializableDownloadProperties) {
    if (property in aSerializable) {
      download[property] = aSerializable[property];
    }
  }

  deserializeUnknownProperties(
    download,
    aSerializable,
    property =>
      !kPlainSerializableDownloadProperties.includes(property) &&
      property != "startTime" &&
      property != "source" &&
      property != "target" &&
      property != "error" &&
      property != "saver"
  );

  return download;
};

Download._updateBatch = function (batch) {
  const batchArray = Array.from(batch);
  for (let download of batchArray) {
    if (!download.stopped) {
      return true;
    }
  }
  batch.clear();
  for (let download of batchArray) {
    download._batch = null;
    download._notifyChange();
  }
  return false;
};

export var DownloadSource = function () {};

DownloadSource.prototype = {
  url: null,

  isDataURICleared: false,

  originalUrl: null,

  triggeredByContentDispositionHeader: false,

  isPrivate: false,

  referrerInfo: null,

  adjustChannel: null,

  allowHttpStatus: null,

  loadingPrincipal: null,

  cookieJarSettings: null,

  authHeader: null,
  toSerializable() {
    if (this.isDataURICleared) {
      return null;
    }

    if (this.adjustChannel) {
      return null;
    }

    if (this.allowHttpStatus) {
      return null;
    }

    let serializable = { url: this.url };
    if (this.isPrivate) {
      serializable.isPrivate = true;
    }

    if (this.referrerInfo && isString(this.referrerInfo)) {
      serializable.referrerInfo = this.referrerInfo;
    } else if (this.referrerInfo) {
      serializable.referrerInfo = lazy.E10SUtils.serializeReferrerInfo(
        this.referrerInfo
      );
    }

    if (this.loadingPrincipal) {
      serializable.loadingPrincipal = isString(this.loadingPrincipal)
        ? this.loadingPrincipal
        : lazy.E10SUtils.serializePrincipal(this.loadingPrincipal);
    }

    if (this.cookieJarSettings) {
      serializable.cookieJarSettings = isString(this.cookieJarSettings)
        ? this.cookieJarSettings
        : lazy.E10SUtils.serializeCookieJarSettings(this.cookieJarSettings);
    }

    if (this.triggeredByContentDispositionHeader) {
      serializable.triggeredByContentDispositionHeader = true;
    }

    serializeUnknownProperties(this, serializable);

    if (Object.keys(serializable).length === 1) {
      return this.url;
    }
    return serializable;
  },
};

DownloadSource.fromSerializable = function (aSerializable) {
  let source = new DownloadSource();
  if (isString(aSerializable)) {
    source.url = aSerializable.toString();
  } else if (aSerializable instanceof Ci.nsIURI) {
    source.url = aSerializable.spec;
  } else {
    source.url = aSerializable.url.toString();
    for (let propName of [
      "isPrivate",
      "userContextId",
      "browsingContextId",
      "triggeredByContentDispositionHeader",
    ]) {
      if (propName in aSerializable) {
        source[propName] = aSerializable[propName];
      }
    }
    if ("originalUrl" in aSerializable) {
      source.originalUrl = aSerializable.originalUrl;
    }
    if ("referrerInfo" in aSerializable) {
      if (aSerializable.referrerInfo instanceof Ci.nsIReferrerInfo) {
        source.referrerInfo = aSerializable.referrerInfo;
      } else {
        source.referrerInfo = lazy.E10SUtils.deserializeReferrerInfo(
          aSerializable.referrerInfo
        );
      }
    }
    if ("loadingPrincipal" in aSerializable) {
      if (aSerializable.loadingPrincipal instanceof Ci.nsIPrincipal) {
        source.loadingPrincipal = aSerializable.loadingPrincipal;
      } else {
        source.loadingPrincipal = lazy.E10SUtils.deserializePrincipal(
          aSerializable.loadingPrincipal
        );
      }
    }
    if ("adjustChannel" in aSerializable) {
      source.adjustChannel = aSerializable.adjustChannel;
    }

    if ("allowHttpStatus" in aSerializable) {
      source.allowHttpStatus = aSerializable.allowHttpStatus;
    }

    if ("cookieJarSettings" in aSerializable) {
      if (aSerializable.cookieJarSettings instanceof Ci.nsICookieJarSettings) {
        source.cookieJarSettings = aSerializable.cookieJarSettings;
      } else {
        source.cookieJarSettings = lazy.E10SUtils.deserializeCookieJarSettings(
          aSerializable.cookieJarSettings
        );
      }
    }

    if ("authHeader" in aSerializable) {
      source.authHeader = aSerializable.authHeader;
    }

    deserializeUnknownProperties(
      source,
      aSerializable,
      property =>
        property != "url" &&
        property != "originalUrl" &&
        property != "isPrivate" &&
        property != "referrerInfo" &&
        property != "cookieJarSettings" &&
        property != "authHeader"
    );
  }

  return source;
};

export var DownloadTarget = function () {};

DownloadTarget.prototype = {
  path: null,

  partFilePath: null,

  exists: false,

  partFileExists: false,

  size: 0,

  async refresh() {
    try {
      this.size = (await IOUtils.stat(this.path)).size;
      this.exists = true;
    } catch (ex) {
      if (ex.name != "NotFoundError") {
        console.error(ex);
      }
      this.exists = false;
    }
    this.refreshPartFileState();
  },

  async refreshPartFileState() {
    if (!this.partFilePath) {
      this.partFileExists = false;
      return;
    }
    try {
      this.partFileExists = (await IOUtils.stat(this.partFilePath)).size > 0;
    } catch (ex) {
      if (ex.name != "NotFoundError") {
        console.error(ex);
      }
      this.partFileExists = false;
    }
  },

  toSerializable() {
    if (!this.partFilePath && !this._unknownProperties) {
      return this.path;
    }

    let serializable = { path: this.path, partFilePath: this.partFilePath };
    serializeUnknownProperties(this, serializable);
    return serializable;
  },
};

DownloadTarget.fromSerializable = function (aSerializable) {
  let target = new DownloadTarget();
  if (isString(aSerializable)) {
    target.path = aSerializable.toString();
  } else if (aSerializable instanceof Ci.nsIFile) {
    target.path = aSerializable.path;
  } else {
    target.path = aSerializable.path.toString();
    if ("partFilePath" in aSerializable) {
      target.partFilePath = aSerializable.partFilePath;
    }

    deserializeUnknownProperties(
      target,
      aSerializable,
      property => property != "path" && property != "partFilePath"
    );
  }
  return target;
};

export var DownloadError = function (aProperties) {
  const NS_ERROR_MODULE_BASE_OFFSET = 0x45;
  const NS_ERROR_MODULE_NETWORK = 6;
  const NS_ERROR_MODULE_FILES = 13;

  this.name = "DownloadError";
  this.result = aProperties.result || Cr.NS_ERROR_FAILURE;
  this.localizedReason = aProperties.localizedReason;
  if (aProperties.message) {
    this.message = aProperties.message;
  } else if (aProperties.becauseBlocked) {
    this.message = "Download blocked.";
  } else {
    let exception = new Components.Exception("", this.result);
    this.message = exception.toString();
  }
  if (aProperties.inferCause) {
    let module =
      ((this.result & 0x7fff0000) >> 16) - NS_ERROR_MODULE_BASE_OFFSET;
    this.becauseSourceFailed = module == NS_ERROR_MODULE_NETWORK;
    this.becauseTargetFailed = module == NS_ERROR_MODULE_FILES;
  } else {
    if (aProperties.becauseSourceFailed) {
      this.becauseSourceFailed = true;
    }
    if (aProperties.becauseTargetFailed) {
      this.becauseTargetFailed = true;
    }
  }

  if (aProperties.becauseBlocked) {
    this.becauseBlocked = true;
  }

  if (aProperties.innerException) {
    this.innerException = aProperties.innerException;
  }

  this.stack = new Error().stack;
};

DownloadError.prototype = {
  result: false,

  becauseSourceFailed: false,

  becauseTargetFailed: false,

  becauseBlocked: false,

  innerException: null,

  toSerializable() {
    let serializable = {
      result: this.result,
      localizedReason: this.localizedReason,
      message: this.message,
      becauseSourceFailed: this.becauseSourceFailed,
      becauseTargetFailed: this.becauseTargetFailed,
      becauseBlocked: this.becauseBlocked,
    };

    serializeUnknownProperties(this, serializable);
    return serializable;
  },
};
Object.setPrototypeOf(DownloadError.prototype, Error.prototype);

DownloadError.fromSerializable = function (aSerializable) {
  let e = new DownloadError(aSerializable);
  deserializeUnknownProperties(
    e,
    aSerializable,
    property =>
      property != "result" &&
      property != "message" &&
      property != "becauseSourceFailed" &&
      property != "becauseTargetFailed" &&
      property != "becauseBlocked"
  );

  return e;
};

export var DownloadSaver = function () {};

DownloadSaver.prototype = {
  download: null,

  async execute() {
    throw new Error("Not implemented.");
  },

  cancel: function DS_cancel() {
    throw new Error("Not implemented.");
  },

  async removeData() {},

  addToHistory() {
    if (AppConstants.MOZ_PLACES) {
      lazy.DownloadHistory.addDownloadToHistory(this.download).catch(
        console.error
      );
    }
  },

  toSerializable() {
    throw new Error("Not implemented.");
  },

}; 

DownloadSaver.fromSerializable = function (aSerializable) {
  let serializable = isString(aSerializable)
    ? { type: aSerializable }
    : aSerializable;
  let saver;
  switch (serializable.type) {
    case "copy":
      saver = DownloadCopySaver.fromSerializable(serializable);
      break;
    case "legacy":
      saver = DownloadLegacySaver.fromSerializable(serializable);
      break;
    default:
      throw new Error("Unrecoginzed download saver type.");
  }
  return saver;
};

export var DownloadCopySaver = function () {};

DownloadCopySaver.prototype = {
  _backgroundFileSaver: null,

  _canceled: false,

  _redirects: null,

  alreadyAddedToHistory: false,

  entityID: null,

  async execute(aSetProgressBytesFn, aSetPropertiesFn) {
    this._canceled = false;

    let download = this.download;
    let targetPath = download.target.path;
    let partFilePath = download.target.partFilePath;
    let keepPartialData = download.tryToKeepPartialData;

    if (!this.alreadyAddedToHistory) {
      this.addToHistory();
      this.alreadyAddedToHistory = true;
    }

    try {
      await IOUtils.writeUTF8(targetPath, "", { mode: "appendOrCreate" });
    } catch (ex) {
      if (!DOMException.isInstance(ex)) {
        throw ex;
      }
      let error = new DownloadError({ message: ex.message });
      error.becauseTargetFailed = true;
      throw error;
    }

    let deferSaveComplete = Promise.withResolvers();

    if (this._canceled) {
      throw new DownloadError({ message: "Saver canceled." });
    }

    let backgroundFileSaver = new BackgroundFileSaverStreamListener();
    backgroundFileSaver.QueryInterface(Ci.nsIStreamListener);

    try {
      backgroundFileSaver.observer = {
        onTargetChange() {},
        onSaveComplete: (aSaver, aStatus) => {
          if (Components.isSuccessCode(aStatus)) {
            this._redirects = aSaver.redirects;
            deferSaveComplete.resolve();
          } else {
            let properties = { result: aStatus, inferCause: true };
            deferSaveComplete.reject(new DownloadError(properties));
          }
          backgroundFileSaver.observer = null;
          this._backgroundFileSaver = null;
        },
      };

      let resumeAttempted = false;
      let resumeFromBytes = 0;

      const notificationCallbacks = {
        QueryInterface: ChromeUtils.generateQI(["nsIInterfaceRequestor"]),
        getInterface: ChromeUtils.generateQI(["nsIProgressEventSink"]),
        onProgress: function DCSE_onProgress(
          aRequest,
          aProgress,
          aProgressMax
        ) {
          let currentBytes = resumeFromBytes + aProgress;
          let totalBytes =
            aProgressMax == -1 ? -1 : resumeFromBytes + aProgressMax;
          aSetProgressBytesFn(
            currentBytes,
            totalBytes,
            aProgress > 0 && partFilePath && keepPartialData
          );
        },
        onStatus() {},
      };

      const streamListener = {
        onStartRequest: function (aRequest) {
          backgroundFileSaver.onStartRequest(aRequest);

          if (aRequest instanceof Ci.nsIHttpChannel) {
            if (
              download.source.allowHttpStatus &&
              !download.source.allowHttpStatus(
                download,
                aRequest.responseStatus
              )
            ) {
              aRequest.cancel(Cr.NS_BINDING_ABORTED);
              return;
            }
          }

          if (aRequest instanceof Ci.nsIChannel) {
            aSetPropertiesFn({ contentType: aRequest.contentType });

            if (aRequest.contentLength >= 0) {
              aSetProgressBytesFn(0, aRequest.contentLength);
            }
          }

          if (
            aRequest instanceof Ci.nsIEncodedChannel &&
            aRequest.contentEncodings
          ) {
            let uri = aRequest.URI;
            if (uri instanceof Ci.nsIURL && uri.fileExtension) {
              let encoding = aRequest.contentEncodings.getNext();
              if (encoding) {
                aRequest.applyConversion =
                  lazy.gExternalHelperAppService.applyDecodingForExtension(
                    uri.fileExtension,
                    encoding
                  );
              }
            }
          }

          if (keepPartialData) {
            if (aRequest instanceof Ci.nsIResumableChannel) {
              try {
                this.entityID = aRequest.entityID;
              } catch (ex) {
                if (
                  !(ex instanceof Components.Exception) ||
                  ex.result != Cr.NS_ERROR_NOT_RESUMABLE
                ) {
                  throw ex;
                }
                keepPartialData = false;
              }
            } else {
              keepPartialData = false;
            }
          }

          if (partFilePath) {
            if (resumeAttempted) {
              backgroundFileSaver.enableAppend();
            }

            backgroundFileSaver.setTarget(
              new lazy.FileUtils.File(partFilePath),
              keepPartialData
            );
          } else {
            backgroundFileSaver.setTarget(
              new lazy.FileUtils.File(targetPath),
              false
            );
          }
        }.bind(this),

        onStopRequest(aRequest, aStatusCode) {
          try {
            backgroundFileSaver.onStopRequest(aRequest, aStatusCode);
          } finally {
            if (Components.isSuccessCode(aStatusCode)) {
              backgroundFileSaver.finish(Cr.NS_OK);
            }
          }
        },

        onDataAvailable: (aRequest, aInputStream, aOffset, aCount) => {
          if (this._canceled) {
            aRequest.cancel(Cr.NS_BINDING_ABORTED);
            return;
          }
          backgroundFileSaver.onDataAvailable(
            aRequest,
            aInputStream,
            aOffset,
            aCount
          );
        },
      };

      const open = async () => {
        let channel;
        if (download.source.loadingPrincipal) {
          channel = lazy.NetUtil.newChannel({
            uri: download.source.url,
            contentPolicyType: Ci.nsIContentPolicy.TYPE_SAVEAS_DOWNLOAD,
            loadingPrincipal: download.source.loadingPrincipal,
            triggeringPrincipal:
              Services.scriptSecurityManager.getSystemPrincipal(),
            securityFlags:
              Ci.nsILoadInfo.SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL,
          });
        } else {
          channel = lazy.NetUtil.newChannel({
            uri: download.source.url,
            contentPolicyType: Ci.nsIContentPolicy.TYPE_SAVEAS_DOWNLOAD,
            loadUsingSystemPrincipal: true,
          });
        }
        if (channel instanceof Ci.nsIPrivateBrowsingChannel) {
          channel.setPrivate(download.source.isPrivate);
        }
        if (
          channel instanceof Ci.nsIHttpChannel &&
          download.source.referrerInfo
        ) {
          channel.referrerInfo = download.source.referrerInfo;
          download.source.referrerInfo = channel.referrerInfo;
        }
        if (
          channel instanceof Ci.nsIHttpChannel &&
          download.source.cookieJarSettings
        ) {
          channel.loadInfo.cookieJarSettings =
            download.source.cookieJarSettings;
        }
        if (
          channel instanceof Ci.nsIHttpChannel &&
          download.source.authHeader
        ) {
          try {
            channel.setRequestHeader(
              "Authorization",
              download.source.authHeader,
              true
            );
          } catch (e) {}
        }

        if (download.source.userContextId) {
          channel.loadInfo.originAttributes = {
            ...channel.loadInfo.originAttributes,
            userContextId: download.source.userContextId,
          };
        }

        if (channel instanceof Ci.nsIHttpChannelInternal) {
          channel.channelIsForDownload = true;

          channel.forceAllowThirdPartyCookie = true;
        }

        if (
          channel instanceof Ci.nsIResumableChannel &&
          this.entityID &&
          partFilePath &&
          keepPartialData
        ) {
          try {
            let stat = await IOUtils.stat(partFilePath);
            channel.resumeAt(stat.size, this.entityID);
            resumeAttempted = true;
            resumeFromBytes = stat.size;
          } catch (ex) {
            if (ex.name != "NotFoundError") {
              throw ex;
            }
          }
        }

        channel.notificationCallbacks = notificationCallbacks;

        if (download.source.adjustChannel) {
          await download.source.adjustChannel(channel);
        }
        channel.asyncOpen(streamListener);
      };

      await open();

      if (this._canceled) {
        throw new DownloadError({ message: "Saver canceled." });
      }

      this._backgroundFileSaver = backgroundFileSaver;
    } catch (ex) {
      backgroundFileSaver.finish(Cr.NS_ERROR_FAILURE);
      deferSaveComplete.promise.catch(() => {});
      throw ex;
    }

    await deferSaveComplete.promise;

    await this._moveCompletedDownload();
  },

  async _moveCompletedDownload() {
    let targetPath = this.download.target.path;
    let partFilePath = this.download.target.partFilePath;
    if (partFilePath) {
      try {
        await IOUtils.move(partFilePath, targetPath);
      } catch (e) {
        if (e.name === "NotAllowedError") {
          let uniquePath = lazy.DownloadPaths.createNiceUniqueFile(
            new lazy.FileUtils.File(targetPath)
          ).path;
          await IOUtils.move(partFilePath, uniquePath);
          this.download.target.path = uniquePath;
        } else {
          throw e;
        }
      }
    }
  },

  cancel: function DCS_cancel() {
    this._canceled = true;
    if (this._backgroundFileSaver) {
      this._backgroundFileSaver.finish(Cr.NS_ERROR_FAILURE);
      this._backgroundFileSaver = null;
    }
  },

  async removeData(canRemoveFinalTarget = false) {
    async function _tryToRemoveFile(path) {
      try {
        await IOUtils.remove(path);
      } catch (ex) {
        if (!["NotFoundError", "NotAllowedError"].includes(ex.name)) {
          console.error(ex);
        }
      }
    }

    if (this.download.target.partFilePath) {
      await _tryToRemoveFile(this.download.target.partFilePath);
    }

    if (this.download.target.path) {
      if (
        canRemoveFinalTarget ||
        (await isPlaceholder(this.download.target.path))
      ) {
        await _tryToRemoveFile(this.download.target.path);
      }
      this.download.target.exists = false;
      this.download.target.size = 0;
    }
  },

  toSerializable() {
    if (!this.entityID && !this._unknownProperties) {
      return "copy";
    }

    let serializable = { type: "copy", entityID: this.entityID };
    serializeUnknownProperties(this, serializable);
    return serializable;
  },

  getRedirects() {
    return this._redirects;
  },
};
Object.setPrototypeOf(DownloadCopySaver.prototype, DownloadSaver.prototype);

DownloadCopySaver.fromSerializable = function (aSerializable) {
  let saver = new DownloadCopySaver();
  if ("entityID" in aSerializable) {
    saver.entityID = aSerializable.entityID;
  }

  deserializeUnknownProperties(
    saver,
    aSerializable,
    property => property != "entityID" && property != "type"
  );

  return saver;
};

export var DownloadLegacySaver = function () {
  this.deferExecuted = Promise.withResolvers();
  this.deferCanceled = Promise.withResolvers();
};

DownloadLegacySaver.prototype = {
  _redirects: null,

  request: null,

  deferExecuted: null,

  deferCanceled: null,

  setProgressBytesFn: null,

  onProgressBytes: function DLS_onProgressBytes(aCurrentBytes, aTotalBytes) {
    this.progressWasNotified = true;

    if (!this.setProgressBytesFn) {
      this.currentBytes = aCurrentBytes;
      this.totalBytes = aTotalBytes;
      return;
    }

    let hasPartFile = !!this.download.target.partFilePath;

    this.setProgressBytesFn(
      aCurrentBytes,
      aTotalBytes,
      aCurrentBytes > 0 && hasPartFile
    );
  },

  progressWasNotified: false,

  onTransferStarted(aRequest) {
    this.request = aRequest;

    if (
      this.download.tryToKeepPartialData &&
      aRequest instanceof Ci.nsIResumableChannel
    ) {
      try {
        this.entityID = aRequest.entityID;
      } catch (ex) {
        if (
          !(ex instanceof Components.Exception) ||
          ex.result != Cr.NS_ERROR_NOT_RESUMABLE
        ) {
          throw ex;
        }
      }
    }

    if (aRequest instanceof Ci.nsIHttpChannel) {
      this.download.source.referrerInfo = aRequest.referrerInfo;
    }

    if (
      aRequest instanceof Ci.nsIChannel &&
      aRequest.loadInfo.isUserTriggeredSave
    ) {
      this.download.openDownloadsListOnStart = false;
    }

    this.addToHistory();
  },

  onTransferFinished(status, localizedReason) {
    if (Components.isSuccessCode(status)) {
      this.deferExecuted.resolve();
    } else {
      let properties = {
        result: status,
        inferCause: true,
        localizedReason,
      };
      this.deferExecuted.reject(new DownloadError(properties));
    }
  },

  firstExecutionFinished: false,

  copySaver: null,

  entityID: null,

  async execute(aSetProgressBytesFn, aSetPropertiesFn) {
    if (this.firstExecutionFinished) {
      if (!this.copySaver) {
        this.copySaver = new DownloadCopySaver();
        this.copySaver.download = this.download;
        this.copySaver.entityID = this.entityID;
        this.copySaver.alreadyAddedToHistory = true;
      }
      await this.copySaver.execute.apply(this.copySaver, arguments);
      return;
    }

    this.setProgressBytesFn = aSetProgressBytesFn;
    if (this.progressWasNotified) {
      this.onProgressBytes(this.currentBytes, this.totalBytes);
    }

    try {
      await this.deferExecuted.promise;

      if (
        !this.progressWasNotified &&
        this.request instanceof Ci.nsIChannel &&
        this.request.contentLength >= 0
      ) {
        aSetProgressBytesFn(0, this.request.contentLength);
      }

      if (!this.download.target.partFilePath) {
        try {
          await IOUtils.writeUTF8(this.download.target.path, "", {
            mode: "create",
          });
        } catch (ex) {
          if (
            !DOMException.isInstance(ex) ||
            ex.name !== "NoModificationAllowedError"
          ) {
            throw ex;
          }
        }
      }

      await this._moveCompletedDownload();
    } catch (ex) {
      this.deferCanceled.resolve();
      throw ex;
    } finally {
      this.request = null;
      this.deferCanceled = null;
      this.firstExecutionFinished = true;
    }
  },

  _moveCompletedDownload() {
    return DownloadCopySaver.prototype._moveCompletedDownload.apply(this);
  },

  cancel: function DLS_cancel() {
    if (this.copySaver) {
      return this.copySaver.cancel.apply(this.copySaver, arguments);
    }

    if (this.deferCanceled) {
      this.deferCanceled.resolve();
    }
  },

  removeData(canRemoveFinalTarget) {
    return DownloadCopySaver.prototype.removeData.call(
      this,
      canRemoveFinalTarget
    );
  },

  toSerializable() {
    return DownloadCopySaver.prototype.toSerializable.call(this);
  },

  getRedirects() {
    if (this.copySaver) {
      return this.copySaver.getRedirects();
    }
    return this._redirects;
  },

  setRedirects(redirects) {
    this._redirects = redirects;
  },
};
Object.setPrototypeOf(DownloadLegacySaver.prototype, DownloadSaver.prototype);

DownloadLegacySaver.fromSerializable = function () {
  return new DownloadLegacySaver();
};
