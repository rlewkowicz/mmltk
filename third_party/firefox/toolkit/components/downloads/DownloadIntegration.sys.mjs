/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

import { Downloads } from "resource://gre/modules/Downloads.sys.mjs";
import { Integration } from "resource://gre/modules/Integration.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AsyncShutdown: "resource://gre/modules/AsyncShutdown.sys.mjs",
  DeferredTask: "resource://gre/modules/DeferredTask.sys.mjs",
  DownloadStore: "resource://gre/modules/DownloadStore.sys.mjs",
  DownloadUIHelper: "resource://gre/modules/DownloadUIHelper.sys.mjs",
  FileUtils: "resource://gre/modules/FileUtils.sys.mjs",
  NetUtil: "resource://gre/modules/NetUtil.sys.mjs",
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "gDownloadPlatform",
  "@mozilla.org/toolkit/download-platform;1",
  Ci.mozIDownloadPlatform
);
XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "gMIMEService",
  "@mozilla.org/mime;1",
  Ci.nsIMIMEService
);
XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "gExternalProtocolService",
  "@mozilla.org/uriloader/external-protocol-service;1",
  Ci.nsIExternalProtocolService
);

Integration.downloads.defineESModuleGetter(
  lazy,
  "DownloadIntegration",
  "resource://gre/modules/DownloadIntegration.sys.mjs"
);
ChromeUtils.defineLazyGetter(lazy, "gCombinedDownloadIntegration", () => {
  return lazy.DownloadIntegration;
});

ChromeUtils.defineLazyGetter(lazy, "stringBundle", () =>
  Services.strings.createBundle(
    "chrome://mozapps/locale/downloads/downloads.properties"
  )
);

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "gExternalAppLauncher",
  "@mozilla.org/uriloader/external-helper-app-service;1",
  Ci.nsPIExternalAppLauncher
);

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "powerManager",
  "@mozilla.org/power/powermanagerservice;1",
  Ci.nsIPowerManagerService
);

const Timer = Components.Constructor(
  "@mozilla.org/timer;1",
  "nsITimer",
  "initWithCallback"
);

const kSaveDelayMs = 1500;

const kObserverTopics = [
  "quit-application-requested",
  "quit-application-granted",
  "offline-requested",
  "last-pb-context-exiting",
  "last-pb-context-exited",
  "sleep_notification",
  "suspend_process_notification",
  "wake_notification",
  "resume_process_notification",
  "network:offline-about-to-go-offline",
  "network:offline-status-changed",
  "xpcom-will-shutdown",
  "blocked-automatic-download",
];

export var DownloadIntegration = {
  _store: null,

  async initializePublicDownloadList(list) {
    try {
      await this.loadPublicDownloadListFromStore(list);
    } catch (ex) {
      console.error(ex);
    }

    if (AppConstants.MOZ_PLACES) {
      new DownloadHistoryObserver(list);
    }
  },

  async loadPublicDownloadListFromStore(list) {
    if (this._store) {
      throw new Error("Initialization may be performed only once.");
    }

    this._store = new lazy.DownloadStore(
      list,
      PathUtils.join(PathUtils.profileDir, "downloads.json")
    );
    this._store.onsaveitem = this.shouldPersistDownload.bind(this);

    try {
      await this._store.load();
    } catch (ex) {
      console.error(ex);
    }

    new DownloadAutoSaveView(list, this._store).initialize();
  },

  shouldPersistDownload(aDownload) {
    return (
      !aDownload.stopped ||
      aDownload.hasPartialData ||
      AppConstants.platform == "android"
    );
  },

  async getSystemDownloadsDirectory() {
    if (this._downloadsDirectory) {
      return this._downloadsDirectory;
    }

    if (AppConstants.platform == "android") {
      this._downloadsDirectory = Services.env.get("DOWNLOADS_DIRECTORY");
      if (!this._downloadsDirectory) {
        throw new Components.Exception(
          "DOWNLOADS_DIRECTORY is not set.",
          Cr.NS_ERROR_FILE_UNRECOGNIZED_PATH
        );
      }
    } else {
      try {
        this._downloadsDirectory = this._getDirectory("DfltDwnld");
      } catch (e) {
        this._downloadsDirectory = await this._createDownloadsDirectory("Home");
      }
    }

    return this._downloadsDirectory;
  },
  _downloadsDirectory: null,

  async _getCustomDirectoryOrDownloads(pref) {
    let directoryPath = null;
    try {
      let directory = Services.prefs.getComplexValue(pref, Ci.nsIFile);
      directoryPath = directory.path;
      await IOUtils.makeDirectory(directoryPath, {
        createAncestors: false,
      });
    } catch (ex) {
      console.error(ex);
      directoryPath = await this.getSystemDownloadsDirectory();
    }
    return directoryPath;
  },

  async getPreferredDownloadsDirectory() {
    let directoryPath = null;
    let prefValue = Services.prefs.getIntPref("browser.download.folderList", 1);

    switch (prefValue) {
      case 0: 
        directoryPath = this._getDirectory("Desk");
        break;
      case 1: 
        directoryPath = await this.getSystemDownloadsDirectory();
        break;
      case 2: 
        directoryPath = await this._getCustomDirectoryOrDownloads(
          "browser.download.dir"
        );
        break;
      default:
        directoryPath = await this.getSystemDownloadsDirectory();
    }
    return directoryPath;
  },

  async getPreferredScreenshotsDirectory() {
    let directoryPath = null;
    let prefValue = Services.prefs.getIntPref(
      "browser.screenshots.folderList",
      4
    );

    switch (prefValue) {
      case 0: 
        directoryPath = this._getDirectory("Desk");
        break;
      case 1: 
        directoryPath = await this.getSystemDownloadsDirectory();
        break;
      case 2: 
        directoryPath = await this._getCustomDirectoryOrDownloads(
          "browser.screenshots.dir"
        );
        break;
      case 3: 
        try {
          directoryPath = this._getDirectory("Scrnshts");
        } catch {
          directoryPath = await this.getSystemDownloadsDirectory();
        }
        break;
      case 4: 
        directoryPath = this.getPreferredDownloadsDirectory();
        break;
      default:
        directoryPath = await this.getSystemDownloadsDirectory();
    }
    return directoryPath;
  },

  async getTemporaryDownloadsDirectory() {
    let directoryPath = null;
    if (AppConstants.platform == "macosx") {
      directoryPath = await this.getPreferredDownloadsDirectory();
    } else if (AppConstants.platform == "android") {
      directoryPath = await this.getSystemDownloadsDirectory();
    } else {
      directoryPath = this._getDirectory("TmpD");
    }
    return directoryPath;
  },

  async downloadDone(aDownload) {
    try {
      await lazy.gDownloadPlatform.maybeWriteDownloadOriginInformation(
        new lazy.FileUtils.File(aDownload.target.path),
        lazy.NetUtil.newURI(aDownload.source.url),
        aDownload.source.referrerInfo,
        aDownload.source.isPrivate
      );
    } catch (ex) {
      console.error(ex);
    }

    try {
      let isTemporaryDownload =
        aDownload.launchWhenSucceeded && aDownload.source.isPrivate;
      let unixMode;
      if (isTemporaryDownload) {
        unixMode = 0o400;
      } else {
        unixMode = 0o666;
      }
      await IOUtils.setPermissions(aDownload.target.path, unixMode);
    } catch (ex) {
      if (!DOMException.isInstance(ex)) {
        console.error(ex);
      }
    }

    let aReferrer = null;
    if (aDownload.source.referrerInfo?.computedReferrerSpec) {
      aReferrer = lazy.NetUtil.newURI(
        aDownload.source.referrerInfo.computedReferrerSpec
      );
    }

    await lazy.gDownloadPlatform.downloadDone(
      lazy.NetUtil.newURI(aDownload.source.url),
      aReferrer,
      new lazy.FileUtils.File(aDownload.target.path),
      aDownload.contentType,
      aDownload.source.isPrivate
    );
  },

  shouldViewDownloadInternally() {
    return false;
  },

  async launchDownload(aDownload, { openWhere, useSystemDefault = null }) {
    let file = new lazy.FileUtils.File(aDownload.target.path);

    let fileExtension = null,
      mimeInfo = null;
    let match = file.leafName.match(/\.([^.]+)$/);
    if (match) {
      fileExtension = match[1];
    }

    let isWindowsExe =
      AppConstants.platform == "win" &&
      fileExtension &&
      fileExtension.toLowerCase() == "exe";

    if (
      file.isExecutable() &&
      !isWindowsExe &&
      !(await this.confirmLaunchExecutable(file.path))
    ) {
      return;
    }

    try {
      mimeInfo = lazy.gMIMEService.getFromTypeAndExtension(
        aDownload.contentType,
        fileExtension
      );
    } catch (e) {}

    let launchWhenSucceeded = aDownload.launchWhenSucceeded;
    aDownload.launchWhenSucceeded = false;

    if (aDownload.launcherPath || aDownload.launcherId) {
      if (!mimeInfo) {
        throw new Error(
          "Unable to create nsIMIMEInfo to launch a custom application"
        );
      }
      let localHandlerApp = null;

      if (aDownload.launcherId) {
        if (!Cc["@mozilla.org/gio-service;1"]) {
          throw new Error(
            "The launcherId is set but missing gio-service to create nsGIOHandlerApp"
          );
        }
        localHandlerApp = Cc["@mozilla.org/gio-service;1"]
          .getService(Ci.nsIGIOService)
          .createHandlerAppFromAppId(aDownload.launcherId);
      } else {
        localHandlerApp = Cc[
          "@mozilla.org/uriloader/local-handler-app;1"
        ].createInstance(Ci.nsILocalHandlerApp);
        localHandlerApp.executable = new lazy.FileUtils.File(
          aDownload.launcherPath
        );
      }

      mimeInfo.preferredApplicationHandler = localHandlerApp;
      mimeInfo.preferredAction = Ci.nsIMIMEInfo.useHelperApp;

      this.launchFile(file, mimeInfo);
      return;
    }

    if (!useSystemDefault && mimeInfo) {
      useSystemDefault = mimeInfo.preferredAction == mimeInfo.useSystemDefault;
    }
    if (!useSystemDefault) {
      if (
        aDownload.handleInternally ||
        (mimeInfo &&
          this.shouldViewDownloadInternally(mimeInfo.type, fileExtension) &&
          !mimeInfo.alwaysAskBeforeHandling &&
          (mimeInfo.preferredAction === Ci.nsIHandlerInfo.handleInternally ||
            (["image/svg+xml", "text/xml", "application/xml"].includes(
              mimeInfo.type
            ) &&
              mimeInfo.preferredAction === Ci.nsIHandlerInfo.saveToDisk)) &&
          !launchWhenSucceeded)
      ) {
        lazy.DownloadUIHelper.loadFileIn(file, {
          browsingContextId: aDownload.source.browsingContextId,
          isPrivate: aDownload.source.isPrivate,
          openWhere,
          userContextId: aDownload.source.userContextId,
          openInBackgroundIfSwitchedBrowsingContext: launchWhenSucceeded,
        });
        return;
      }
    }

    if (!fileExtension && AppConstants.platform == "win") {
      this.showContainingDirectory(aDownload.target.path);
      return;
    }

    if (mimeInfo) {
      mimeInfo.preferredAction = Ci.nsIMIMEInfo.useSystemDefault;
      try {
        this.launchFile(file, mimeInfo);
        return;
      } catch (ex) {}
    }

    try {
      this.launchFile(file);
      return;
    } catch (ex) {}

    lazy.gExternalProtocolService.loadURI(
      lazy.NetUtil.newURI(file),
      Services.scriptSecurityManager.getSystemPrincipal()
    );
  },

  async confirmLaunchExecutable(path) {
    return lazy.DownloadUIHelper.getPrompter().confirmLaunchExecutable(path);
  },

  launchFile(file, mimeInfo) {
    if (mimeInfo) {
      mimeInfo.launchWithFile(file);
    } else {
      file.launch();
    }
  },

  async showContainingDirectory(aFilePath) {
    let file = new lazy.FileUtils.File(aFilePath);

    try {
      file.reveal();
      return;
    } catch (ex) {}

    let parent = file.parent;
    if (!parent) {
      throw new Error(
        "Unexpected reference to a top-level directory instead of a file"
      );
    }

    try {
      parent.launch();
      return;
    } catch (ex) {}

    lazy.gExternalProtocolService.loadURI(
      lazy.NetUtil.newURI(parent),
      Services.scriptSecurityManager.getSystemPrincipal()
    );
  },

  _createDownloadsDirectory(aName) {
    let directoryPath = PathUtils.join(
      this._getDirectory(aName),
      lazy.stringBundle.GetStringFromName("downloadsFolder")
    );

    return IOUtils.makeDirectory(directoryPath, {
      createAncestors: false,
    }).then(() => directoryPath);
  },

  _getDirectory(name) {
    return Services.dirsvc.get(name, Ci.nsIFile).path;
  },

  addListObservers(aList, aIsPrivate) {
    DownloadObserver.registerView(aList, aIsPrivate);
    if (!DownloadObserver.observersAdded) {
      DownloadObserver.observersAdded = true;
      for (let topic of kObserverTopics) {
        Services.obs.addObserver(DownloadObserver, topic);
      }
    }

    Services.prefs.addObserver(
      "browser.download.deletePrivate",
      async function () {
        let privateDownloadsList = await Downloads.getList(Downloads.PRIVATE);
        let privateDownloads = await privateDownloadsList.getAll();
        for (let download of privateDownloads) {
          lazy.gExternalAppLauncher.deletePrivateFileWhenPossible(
            new lazy.FileUtils.File(download.target.path)
          );
        }
      }
    );
    return Promise.resolve();
  },

  forceSave() {
    if (this._store) {
      return this._store.save();
    }
    return Promise.resolve();
  },

  get _testGetDownloadObserver() {
    return DownloadObserver;
  },
};

var DownloadObserver = {
  observersAdded: false,

  _wakeTimer: null,

  _downloadWakeLock: null,

  _publicInProgressDownloads: new Set(),

  _privateInProgressDownloads: new Set(),

  _canceledOfflineDownloads: new Set(),

  get _hasInProgressDownloads() {
    return (
      this._publicInProgressDownloads.size > 0 ||
      this._privateInProgressDownloads.size > 0
    );
  },

  _acquireDownloadWakeLock() {
    if (!this._downloadWakeLock) {
      this._downloadWakeLock = lazy.powerManager.newWakeLock(
        "download-in-progress",
        null
      );
    }
  },

  _releaseDownloadWakeLock() {
    if (this._downloadWakeLock) {
      try {
        this._downloadWakeLock.unlock();
      } catch (e) {
      }
      this._downloadWakeLock = null;
    }
  },

  registerView: function DO_registerView(aList, aIsPrivate) {
    let downloadsSet = aIsPrivate
      ? this._privateInProgressDownloads
      : this._publicInProgressDownloads;
    let downloadsView = {
      onDownloadAdded: aDownload => {
        if (!aDownload.stopped) {
          downloadsSet.add(aDownload);
          if (this._hasInProgressDownloads) {
            this._acquireDownloadWakeLock();
          }
        }
      },
      onDownloadChanged: aDownload => {
        if (aDownload.stopped) {
          downloadsSet.delete(aDownload);
          if (!this._hasInProgressDownloads) {
            this._releaseDownloadWakeLock();
          }
        } else {
          downloadsSet.add(aDownload);
          if (this._hasInProgressDownloads) {
            this._acquireDownloadWakeLock();
          }
        }
      },
      onDownloadRemoved: aDownload => {
        downloadsSet.delete(aDownload);
        this._canceledOfflineDownloads.delete(aDownload);
        if (!this._hasInProgressDownloads) {
          this._releaseDownloadWakeLock();
        }
      },
    };

    aList.addView(downloadsView);
  },

  _confirmCancelDownloads: function DO_confirmCancelDownload(
    aCancel,
    aDownloadsCount,
    aPromptType
  ) {
    if (lazy.gCombinedDownloadIntegration._testPromptDownloads) {
      lazy.gCombinedDownloadIntegration._testPromptDownloads = aDownloadsCount;
      return;
    }

    if (!aDownloadsCount) {
      return;
    }

    const isPromptGranted = Cc["@mozilla.org/supports-PRBool;1"].createInstance(
      Ci.nsISupportsPRBool
    );
    isPromptGranted.data = true;
    Services.obs.notifyObservers(
      isPromptGranted,
      "before-cancel-download-prompt"
    );

    if (!isPromptGranted.data) {
      aCancel.data = false;
      return;
    }

    if (aCancel instanceof Ci.nsISupportsPRBool && aCancel.data) {
      return;
    }

    let prompter = lazy.DownloadUIHelper.getPrompter();
    aCancel.data = prompter.confirmCancelDownloads(
      aDownloadsCount,
      prompter[aPromptType]
    );
  },

  _resumeOfflineDownloads: function DO_resumeOfflineDownloads() {
    this._wakeTimer = null;

    for (let download of this._canceledOfflineDownloads) {
      download.start().catch(() => {});
    }
    this._canceledOfflineDownloads.clear();
  },

  observe: function DO_observe(aSubject, aTopic, aData) {
    let downloadsCount;
    switch (aTopic) {
      case "quit-application-requested": {
        downloadsCount =
          this._publicInProgressDownloads.size +
          this._privateInProgressDownloads.size;
        this._confirmCancelDownloads(aSubject, downloadsCount, "ON_QUIT");
        break;
      }
      case "quit-application-granted": {
        break;
      }
      case "offline-requested":
        downloadsCount =
          this._publicInProgressDownloads.size +
          this._privateInProgressDownloads.size;
        this._confirmCancelDownloads(aSubject, downloadsCount, "ON_OFFLINE");
        break;
      case "last-pb-context-exiting":
        downloadsCount = this._privateInProgressDownloads.size;
        this._confirmCancelDownloads(
          aSubject,
          downloadsCount,
          "ON_LEAVE_PRIVATE_BROWSING"
        );
        break;
      case "last-pb-context-exited": {
        let collector;
        try {
          collector = aSubject?.QueryInterface(Ci.nsIPBMCleanupCollector);
        } catch (e) {}
        let cb = collector?.addPendingCleanup();

        let promise = (async function () {
          let list = await Downloads.getList(Downloads.PRIVATE);
          let downloads = await list.getAll();

          downloads.forEach(d => list.remove(d));
          await Promise.all(
            downloads.map(d => d.finalize(true).catch(console.error))
          );
        })();

        promise.then(
          () => cb?.complete(Cr.NS_OK),
          () => cb?.complete(Cr.NS_ERROR_FAILURE)
        );

        if (lazy.gCombinedDownloadIntegration._testResolveClearPrivateList) {
          lazy.gCombinedDownloadIntegration._testResolveClearPrivateList(
            promise
          );
        } else {
          promise.catch(ex => console.error(ex));
        }
        break;
      }
      case "sleep_notification":
      case "suspend_process_notification":
      case "network:offline-about-to-go-offline":
        if (
          Services.startup.isInOrBeyondShutdownPhase(
            Ci.nsIAppStartup.SHUTDOWN_PHASE_APPSHUTDOWNCONFIRMED
          )
        ) {
          break;
        }
        for (let download of this._publicInProgressDownloads) {
          download.cancel();
          this._canceledOfflineDownloads.add(download);
        }
        for (let download of this._privateInProgressDownloads) {
          download.cancel();
          this._canceledOfflineDownloads.add(download);
        }
        break;
      case "wake_notification":
      case "resume_process_notification": {
        let wakeDelay = Services.prefs.getIntPref(
          "browser.download.manager.resumeOnWakeDelay",
          10000
        );

        if (wakeDelay >= 0) {
          this._wakeTimer = new Timer(
            this._resumeOfflineDownloads.bind(this),
            wakeDelay,
            Ci.nsITimer.TYPE_ONE_SHOT
          );
        }
        break;
      }
      case "network:offline-status-changed":
        if (aData == "online") {
          this._resumeOfflineDownloads();
        }
        break;
      case "xpcom-will-shutdown":
        for (let topic of kObserverTopics) {
          Services.obs.removeObserver(this, topic);
        }
        break;
    }
  },

  QueryInterface: ChromeUtils.generateQI(["nsIObserver"]),
};

var DownloadHistoryObserver = function (aList) {
  this._list = aList;

  this._placesObserver = new PlacesWeakCallbackWrapper(
    this.handlePlacesEvents.bind(this)
  );
  PlacesObservers.addListener(
    ["history-cleared", "page-removed"],
    this._placesObserver
  );
};

DownloadHistoryObserver.prototype = {
  _list: null,

  handlePlacesEvents(events) {
    for (const event of events) {
      switch (event.type) {
        case "history-cleared": {
          this._list.removeFinished();
          break;
        }
        case "page-removed": {
          if (event.isRemovedFromStore) {
            this._list.removeFinished(
              download => event.url === download.source.url
            );
          }
          break;
        }
      }
    }
  },
};

var DownloadAutoSaveView = function (aList, aStore) {
  this._list = aList;
  this._store = aStore;
  this._downloadsMap = new Map();
  this._writer = new lazy.DeferredTask(() => this._store.save(), kSaveDelayMs);
  lazy.AsyncShutdown.profileBeforeChange.addBlocker(
    "DownloadAutoSaveView: writing data",
    () => this._writer.finalize()
  );
};

DownloadAutoSaveView.prototype = {
  _list: null,

  _store: null,

  _initialized: false,

  initialize() {
    this._list.addView(this);
    this._initialized = true;
  },

  _downloadsMap: null,

  _writer: null,

  saveSoon() {
    this._writer.arm();
  },

  onDownloadAdded(aDownload) {
    if (lazy.gCombinedDownloadIntegration.shouldPersistDownload(aDownload)) {
      this._downloadsMap.set(aDownload, aDownload.getSerializationHash());
      if (this._initialized) {
        this.saveSoon();
      }
    }
  },

  onDownloadChanged(aDownload) {
    if (!lazy.gCombinedDownloadIntegration.shouldPersistDownload(aDownload)) {
      if (this._downloadsMap.has(aDownload)) {
        this._downloadsMap.delete(aDownload);
        this.saveSoon();
      }
      return;
    }

    let hash = aDownload.getSerializationHash();
    if (this._downloadsMap.get(aDownload) != hash) {
      this._downloadsMap.set(aDownload, hash);
      this.saveSoon();
    }
  },

  onDownloadRemoved(aDownload) {
    if (this._downloadsMap.has(aDownload)) {
      this._downloadsMap.delete(aDownload);
      this.saveSoon();
    }
  },
};
