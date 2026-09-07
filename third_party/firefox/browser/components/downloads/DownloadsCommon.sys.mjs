/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */



import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  DownloadHistory: "resource://gre/modules/DownloadHistory.sys.mjs",
  DownloadUtils: "resource://gre/modules/DownloadUtils.sys.mjs",
  Downloads: "resource://gre/modules/Downloads.sys.mjs",
  NetUtil: "resource://gre/modules/NetUtil.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

XPCOMUtils.defineLazyServiceGetters(lazy, {
  gClipboardHelper: [
    "@mozilla.org/widget/clipboardhelper;1",
    Ci.nsIClipboardHelper,
  ],
  gMIMEService: ["@mozilla.org/mime;1", Ci.nsIMIMEService],
});

ChromeUtils.defineLazyGetter(lazy, "DownloadsLogger", () => {
  let { ConsoleAPI } = ChromeUtils.importESModule(
    "resource://gre/modules/Console.sys.mjs"
  );
  let consoleOptions = {
    maxLogLevelPref: "toolkit.download.loglevel",
    prefix: "Downloads",
  };
  return new ConsoleAPI(consoleOptions);
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "gAlwaysOpenPanel",
  "browser.download.alwaysOpenPanel",
  true
);

const kDownloadsStringBundleUrl =
  "chrome://browser/locale/downloads/downloads.properties";

const kDownloadsFluentStrings = new Localization(
  ["browser/downloads.ftl"],
  true
);

const kDownloadsStringsRequiringFormatting = {
  sizeWithUnits: true,
  statusSeparator: true,
  statusSeparatorBeforeNumber: true,
};

const kMaxHistoryResultsForLimitedView = 42;

const kLargeDataUriLengthThreshold = 10 * 1024 * 1024;

const kPrefBranch = Services.prefs.getBranch("browser.download.");

const kGenericContentTypes = [
  "application/octet-stream",
  "binary/octet-stream",
  "application/unknown",
];

var PrefObserver = {
  QueryInterface: ChromeUtils.generateQI(["nsIObserver"]),
  getPref(name) {
    try {
      switch (typeof this.prefs[name]) {
        case "boolean":
          return kPrefBranch.getBoolPref(name);
      }
    } catch (ex) {}
    return this.prefs[name];
  },
  observe(aSubject, aTopic, aData) {
    if (this.prefs.hasOwnProperty(aData)) {
      delete this[aData];
      this[aData] = this.getPref(aData);
    }
  },
  register(prefs) {
    this.prefs = prefs;
    kPrefBranch.addObserver("", this);
    for (let key in prefs) {
      let name = key;
      ChromeUtils.defineLazyGetter(this, name, function () {
        return PrefObserver.getPref(name);
      });
    }
  },
};

PrefObserver.register({
  openInSystemViewerContextMenuItem: true,
  alwaysOpenInSystemViewerContextMenuItem: true,
});


export var DownloadsCommon = {
  DOWNLOAD_NOTSTARTED: -1,
  DOWNLOAD_DOWNLOADING: 0,
  DOWNLOAD_FINISHED: 1,
  DOWNLOAD_FAILED: 2,
  DOWNLOAD_CANCELED: 3,
  DOWNLOAD_PAUSED: 4,
  DOWNLOAD_BLOCKED_POLICY: 9,

  ATTENTION_NONE: "",
  ATTENTION_SUCCESS: "success",
  ATTENTION_INFO: "info",
  ATTENTION_WARNING: "warning",
  ATTENTION_SEVERE: "severe",

  SUPPRESS_NONE: 0,
  SUPPRESS_PANEL_OPEN: 1,
  SUPPRESS_ALL_DOWNLOADS_OPEN: 2,
  SUPPRESS_CONTENT_AREA_DOWNLOADS_OPEN: 4,

  get strings() {
    let strings = {};
    let sb = Services.strings.createBundle(kDownloadsStringBundleUrl);
    for (let string of sb.getSimpleEnumeration()) {
      let stringName = string.key;
      if (stringName in kDownloadsStringsRequiringFormatting) {
        strings[stringName] = function () {
          return sb.formatStringFromName(stringName, Array.from(arguments));
        };
      } else {
        strings[stringName] = string.value;
      }
    }
    delete this.strings;
    return (this.strings = strings);
  },

  get openInSystemViewerItemEnabled() {
    return PrefObserver.openInSystemViewerContextMenuItem;
  },

  get alwaysOpenInSystemViewerItemEnabled() {
    return PrefObserver.alwaysOpenInSystemViewerContextMenuItem;
  },

  getData(window, history = false, privateAll = false, limited = false) {
    let isPrivate =
      window && lazy.PrivateBrowsingUtils.isContentWindowPrivate(window);
    if (isPrivate && !privateAll) {
      return lazy.PrivateDownloadsData;
    }
    if (history) {
      if (isPrivate && privateAll) {
        return lazy.LimitedPrivateHistoryDownloadData;
      }
      return limited
        ? lazy.LimitedHistoryDownloadsData
        : lazy.HistoryDownloadsData;
    }
    return lazy.DownloadsData;
  },

  initializeAllDataLinks() {
    lazy.DownloadsData.initializeDataLink();
    lazy.PrivateDownloadsData.initializeDataLink();
  },

  getIndicatorData(aWindow) {
    if (lazy.PrivateBrowsingUtils.isContentWindowPrivate(aWindow)) {
      return lazy.PrivateDownloadsIndicatorData;
    }
    return lazy.DownloadsIndicatorData;
  },

  getSummary(aWindow, aNumToExclude) {
    if (lazy.PrivateBrowsingUtils.isContentWindowPrivate(aWindow)) {
      if (this._privateSummary) {
        return this._privateSummary;
      }
      return (this._privateSummary = new DownloadsSummaryData(
        true,
        aNumToExclude
      ));
    }
    if (this._summary) {
      return this._summary;
    }
    return (this._summary = new DownloadsSummaryData(false, aNumToExclude));
  },
  _summary: null,
  _privateSummary: null,

  stateOfDownload(download) {
    if (!download.stopped) {
      return DownloadsCommon.DOWNLOAD_DOWNLOADING;
    }
    if (download.succeeded) {
      return DownloadsCommon.DOWNLOAD_FINISHED;
    }
    if (download.error) {
      return DownloadsCommon.DOWNLOAD_FAILED;
    }
    if (download.canceled) {
      if (download.hasPartialData) {
        return DownloadsCommon.DOWNLOAD_PAUSED;
      }
      return DownloadsCommon.DOWNLOAD_CANCELED;
    }
    return DownloadsCommon.DOWNLOAD_NOTSTARTED;
  },

  async deleteDownload(download) {
    let sourceURI = URL.parse(download.source.url)?.URI;
    if (
      AppConstants.MOZ_PLACES &&
      sourceURI &&
      lazy.PlacesUtils.history.canAddURI(sourceURI)
    ) {
      await lazy.PlacesUtils.history.remove(sourceURI).catch(console.error);
    }
    let list = await lazy.Downloads.getList(lazy.Downloads.ALL);
    await list.remove(download);
    await download.finalize(true);
  },

  async deleteDownloadFiles(download, clearHistoryOnDelete = 0) {
    if (clearHistoryOnDelete > 1) {
      let sourceURI = URL.parse(download.source.url)?.URI;
      if (
        AppConstants.MOZ_PLACES &&
        sourceURI &&
        lazy.PlacesUtils.history.canAddURI(sourceURI)
      ) {
        await lazy.PlacesUtils.history.remove(sourceURI).catch(console.error);
      }
    }
    if (clearHistoryOnDelete > 0) {
      let list = await lazy.Downloads.getList(lazy.Downloads.ALL);
      await list.remove(download);
    }
    await download.manuallyRemoveData();
    if (AppConstants.MOZ_PLACES && clearHistoryOnDelete < 2) {
      lazy.DownloadHistory.updateMetaData(download).catch(console.error);
    }
  },

  getMimeInfo(download) {
    if (!download.succeeded) {
      return null;
    }
    let contentType = download.contentType;
    let url = Cc["@mozilla.org/network/standard-url-mutator;1"]
      .createInstance(Ci.nsIURIMutator)
      .setSpec("http://example.com") 
      .setFilePath(download.target.path)
      .finalize()
      .QueryInterface(Ci.nsIURL);
    let fileExtension = url.fileExtension;

    if (!contentType || kGenericContentTypes.includes(contentType)) {
      try {
        contentType = lazy.gMIMEService.getTypeFromExtension(fileExtension);
      } catch (ex) {
        DownloadsCommon.log(
          "Cant get mimeType from file extension: ",
          fileExtension
        );
      }
    }
    if (!(contentType || fileExtension)) {
      return null;
    }
    let mimeInfo = null;
    try {
      mimeInfo = lazy.gMIMEService.getFromTypeAndExtension(
        contentType || "",
        fileExtension || ""
      );
    } catch (ex) {
      DownloadsCommon.log(
        "Can't get nsIMIMEInfo for contentType: ",
        contentType,
        "and fileExtension:",
        fileExtension
      );
    }
    return mimeInfo;
  },

  isFileOfType(download, mimeType) {
    if (!(download.succeeded && download.target?.exists)) {
      DownloadsCommon.log(
        `isFileOfType returning false for mimeType: ${mimeType}, succeeded: ${download.succeeded}, exists: ${download.target?.exists}`
      );
      return false;
    }
    let mimeInfo = DownloadsCommon.getMimeInfo(download);
    return mimeInfo?.type === mimeType.toLowerCase();
  },

  copyDownloadLink(download) {
    lazy.gClipboardHelper.copyString(
      download.source.originalUrl || download.source.url
    );
  },

  summarizeDownloads(downloads) {
    let summary = {
      numActive: 0,
      numPaused: 0,
      numDownloading: 0,
      totalSize: 0,
      totalTransferred: 0,
      slowestSpeed: Infinity,
      rawTimeLeft: -1,
      percentComplete: -1,
    };

    for (let download of downloads) {
      summary.numActive++;

      if (!download.stopped) {
        summary.numDownloading++;
        if (download.hasProgress && download.speed > 0) {
          let sizeLeft = download.totalBytes - download.currentBytes;
          summary.rawTimeLeft = Math.max(
            summary.rawTimeLeft,
            sizeLeft / download.speed
          );
          summary.slowestSpeed = Math.min(summary.slowestSpeed, download.speed);
        }
      } else if (download.canceled && download.hasPartialData) {
        summary.numPaused++;
      }

      if (download.succeeded) {
        summary.totalSize += download.target.size;
        summary.totalTransferred += download.target.size;
      } else if (download.hasProgress) {
        summary.totalSize += download.totalBytes;
        summary.totalTransferred += download.currentBytes;
      }
    }

    if (summary.totalSize != 0) {
      summary.percentComplete = Math.floor(
        (summary.totalTransferred / summary.totalSize) * 100
      );
    }

    if (summary.slowestSpeed == Infinity) {
      summary.slowestSpeed = 0;
    }

    return summary;
  },

  smoothSeconds(aSeconds, aLastSeconds) {
    let shouldApplySmoothing = aLastSeconds >= 0 && aSeconds > aLastSeconds / 2;
    if (shouldApplySmoothing) {
      let diff = aSeconds - aLastSeconds;
      aSeconds = aLastSeconds + (diff < 0 ? 0.3 : 0.1) * diff;

      diff = aSeconds - aLastSeconds;
      let diffPercent = (diff / aLastSeconds) * 100;
      if (Math.abs(diff) < 5 || Math.abs(diffPercent) < 5) {
        aSeconds = aLastSeconds - (diff < 0 ? 0.4 : 0.2);
      }
    }

    return (aLastSeconds = Math.max(aSeconds, 1));
  },

  async openDownload(download, options) {
    if (typeof download.launch !== "function") {
      download = await lazy.Downloads.createDownload(download);
    }
    return download.launch(options).catch(ex => console.error(ex));
  },

  showDownloadedFile(aFile) {
    if (!(aFile instanceof Ci.nsIFile)) {
      throw new Error("aFile must be a nsIFile object");
    }
    try {
      aFile.reveal();
    } catch (ex) {
      let parent = aFile.parent;
      if (parent) {
        this.showDirectory(parent);
      }
    }
  },

  showDirectory(aDirectory) {
    if (!(aDirectory instanceof Ci.nsIFile)) {
      throw new Error("aDirectory must be a nsIFile object");
    }
    try {
      aDirectory.launch();
    } catch (ex) {
      Cc["@mozilla.org/uriloader/external-protocol-service;1"]
        .getService(Ci.nsIExternalProtocolService)
        .loadURI(
          lazy.NetUtil.newURI(aDirectory),
          Services.scriptSecurityManager.getSystemPrincipal()
        );
    }
  },


};

ChromeUtils.defineLazyGetter(DownloadsCommon, "log", () => {
  return lazy.DownloadsLogger.log.bind(lazy.DownloadsLogger);
});
ChromeUtils.defineLazyGetter(DownloadsCommon, "error", () => {
  return lazy.DownloadsLogger.error.bind(lazy.DownloadsLogger);
});


function DownloadsDataCtor({ isPrivate, isHistory, maxHistoryResults } = {}) {
  this._isPrivate = !!isPrivate;

  this._oldDownloadStates = new WeakMap();

  if (AppConstants.MOZ_PLACES && isHistory) {
    if (isPrivate) {
      lazy.PrivateDownloadsData.initializeDataLink();
    }
    lazy.DownloadsData.initializeDataLink();
    this._promiseList = lazy.DownloadsData._promiseList.then(() => {
      return lazy.DownloadHistory.getList({
        type: isPrivate ? lazy.Downloads.ALL : lazy.Downloads.PUBLIC,
        maxHistoryResults,
      });
    });
    return;
  }

  this._promiseList = (async () => {
    await new Promise(resolve => (this.initializeDataLink = resolve));
    let list = await lazy.Downloads.getList(
      isPrivate ? lazy.Downloads.PRIVATE : lazy.Downloads.PUBLIC
    );
    list.addView(this);
    return list;
  })();
}

DownloadsDataCtor.prototype = {
  initializeDataLink() {},

  _promiseList: null,

  get _downloads() {
    return ChromeUtils.nondeterministicGetWeakMapKeys(this._oldDownloadStates);
  },

  get canRemoveFinished() {
    for (let download of this._downloads) {
      if (download.stopped && !(download.canceled && download.hasPartialData)) {
        return true;
      }
    }
    return false;
  },

  removeFinished() {
    lazy.Downloads.getList(
      this._isPrivate ? lazy.Downloads.PRIVATE : lazy.Downloads.PUBLIC
    )
      .then(list => list.removeFinished())
      .catch(console.error);
  },


  onDownloadAdded(download) {
    download.endTime = Date.now();

    this._oldDownloadStates.set(
      download,
      DownloadsCommon.stateOfDownload(download)
    );
  },

  onDownloadChanged(download) {
    let oldState = this._oldDownloadStates.get(download);
    let newState = DownloadsCommon.stateOfDownload(download);
    this._oldDownloadStates.set(download, newState);

    if (oldState != newState) {
      if (
        download.succeeded ||
        (download.canceled && !download.hasPartialData) ||
        download.error
      ) {
        download.endTime = Date.now();

        if (AppConstants.MOZ_PLACES) {
          lazy.DownloadHistory.updateMetaData(download).catch(console.error);
        }

        if (
          download.succeeded &&
          download.source.url?.startsWith("data:") &&
          download.source.url.length > kLargeDataUriLengthThreshold
        ) {
          const commaIndex = download.source.url.indexOf(",");
          download.source.url = download.source.url.slice(0, commaIndex + 1);
          download.source.isDataURICleared = true;
        }
      }

      if (
        download.succeeded ||
        (download.error && download.error.becauseBlocked)
      ) {
        this._notifyDownloadEvent("finish");
      }
    }

    if (!download.newDownloadNotified) {
      download.newDownloadNotified = true;
      this._notifyDownloadEvent("start", {
        openDownloadsListOnStart: download.openDownloadsListOnStart,
      });
    }
  },

  onDownloadRemoved(download) {
    this._oldDownloadStates.delete(download);
  },


  addView(aView) {
    this._promiseList.then(list => list.addView(aView)).catch(console.error);
  },

  removeView(aView) {
    this._promiseList.then(list => list.removeView(aView)).catch(console.error);
  },


  get panelHasShownBefore() {
    try {
      return Services.prefs.getBoolPref("browser.download.panel.shown");
    } catch (ex) {}
    return false;
  },

  set panelHasShownBefore(aValue) {
    Services.prefs.setBoolPref("browser.download.panel.shown", aValue);
  },

  _notifyDownloadEvent(aType, { openDownloadsListOnStart = true } = {}) {
    DownloadsCommon.log(
      "Attempting to notify that a new download has started or finished."
    );

    let browserWin = lazy.BrowserWindowTracker.getTopWindow({
      private: this._isPrivate,
      allowFromInactiveWorkspace: true,
    });
    if (!browserWin) {
      return;
    }

    let shouldOpenDownloadsPanel =
      aType == "start" &&
      DownloadsCommon.summarizeDownloads(this._downloads).numDownloading <= 1 &&
      lazy.gAlwaysOpenPanel;

    if (
      aType != "error" &&
      ((this.panelHasShownBefore && !shouldOpenDownloadsPanel) ||
        !openDownloadsListOnStart ||
        browserWin != Services.focus.activeWindow)
    ) {
      DownloadsCommon.log("Showing new download notification.");
      browserWin.DownloadsIndicatorView.showEventNotification(aType);
      return;
    }
    this.panelHasShownBefore = true;
    browserWin.DownloadsPanel.showPanel();
  },
};

ChromeUtils.defineLazyGetter(lazy, "HistoryDownloadsData", function () {
  return new DownloadsDataCtor({ isHistory: true });
});

ChromeUtils.defineLazyGetter(lazy, "LimitedHistoryDownloadsData", function () {
  return new DownloadsDataCtor({
    isHistory: true,
    maxHistoryResults: kMaxHistoryResultsForLimitedView,
  });
});

ChromeUtils.defineLazyGetter(
  lazy,
  "LimitedPrivateHistoryDownloadData",
  function () {
    return new DownloadsDataCtor({
      isPrivate: true,
      isHistory: true,
      maxHistoryResults: kMaxHistoryResultsForLimitedView,
    });
  }
);

ChromeUtils.defineLazyGetter(lazy, "PrivateDownloadsData", function () {
  return new DownloadsDataCtor({ isPrivate: true });
});

ChromeUtils.defineLazyGetter(lazy, "DownloadsData", function () {
  return new DownloadsDataCtor();
});


const DownloadsViewPrototype = {
  _oldDownloadStates: null,


  _views: null,

  _isPrivate: false,

  addView(aView) {
    if (!this._views.length) {
      if (this._isPrivate) {
        lazy.PrivateDownloadsData.addView(this);
      } else {
        lazy.DownloadsData.addView(this);
      }
    }

    this._views.push(aView);
    this.refreshView(aView);
  },

  refreshView(aView) {
    this._refreshProperties();
    this._updateView(aView);
  },

  removeView(aView) {
    let index = this._views.indexOf(aView);
    if (index != -1) {
      this._views.splice(index, 1);
    }

    if (!this._views.length) {
      if (this._isPrivate) {
        lazy.PrivateDownloadsData.removeView(this);
      } else {
        lazy.DownloadsData.removeView(this);
      }
    }
  },


  _loading: false,

  onDownloadBatchStarting() {
    this._loading = true;
  },

  onDownloadBatchEnded() {
    this._loading = false;
    this._updateViews();
  },

  onDownloadAdded(download) {
    this._oldDownloadStates.set(
      download,
      DownloadsCommon.stateOfDownload(download)
    );
  },

  onDownloadStateChanged() {
    throw Components.Exception("", Cr.NS_ERROR_NOT_IMPLEMENTED);
  },

  onDownloadChanged(download) {
    let oldState = this._oldDownloadStates.get(download);
    let newState = DownloadsCommon.stateOfDownload(download);
    this._oldDownloadStates.set(download, newState);

    if (oldState != newState) {
      this.onDownloadStateChanged(download);
    }
  },

  onDownloadRemoved(download) {
    this._oldDownloadStates.delete(download);
  },

  _refreshProperties() {
    throw Components.Exception("", Cr.NS_ERROR_NOT_IMPLEMENTED);
  },

  _updateView() {
    throw Components.Exception("", Cr.NS_ERROR_NOT_IMPLEMENTED);
  },

  _updateViews() {
    if (this._loading) {
      return;
    }

    this._refreshProperties();
    this._views.forEach(this._updateView, this);
  },
};


function DownloadsIndicatorDataCtor(aPrivate) {
  this._oldDownloadStates = new WeakMap();
  this._isPrivate = aPrivate;
  this._views = [];
}
DownloadsIndicatorDataCtor.prototype = {
  _attentionPriority: new Map([
    [DownloadsCommon.ATTENTION_NONE, 0],
    [DownloadsCommon.ATTENTION_SUCCESS, 1],
    [DownloadsCommon.ATTENTION_INFO, 2],
    [DownloadsCommon.ATTENTION_WARNING, 3],
    [DownloadsCommon.ATTENTION_SEVERE, 4],
  ]),

  get _downloads() {
    return ChromeUtils.nondeterministicGetWeakMapKeys(this._oldDownloadStates);
  },

  removeView(aView) {
    DownloadsViewPrototype.removeView.call(this, aView);

    if (!this._views.length) {
      this._itemCount = 0;
    }
  },

  onDownloadAdded(download) {
    DownloadsViewPrototype.onDownloadAdded.call(this, download);
    this._itemCount++;
    this._updateViews();
  },

  onDownloadStateChanged(download) {
    if (this._attentionSuppressed !== DownloadsCommon.SUPPRESS_NONE) {
      return;
    }
    let attention;
    if (download.succeeded) {
      attention = DownloadsCommon.ATTENTION_SUCCESS;
    } else if (download.error) {
      attention = DownloadsCommon.ATTENTION_WARNING;
    }
    download.attention = attention;
    this.updateAttention();
  },

  onDownloadChanged(download) {
    DownloadsViewPrototype.onDownloadChanged.call(this, download);
    this._updateViews();
  },

  onDownloadRemoved(download) {
    DownloadsViewPrototype.onDownloadRemoved.call(this, download);
    this._itemCount--;
    this.updateAttention();
    this._updateViews();
  },


  _hasDownloads: false,
  _percentComplete: -1,

  set attention(aValue) {
    this._attention = aValue;
    this._updateViews();
  },
  _attention: DownloadsCommon.ATTENTION_NONE,

  set attentionSuppressed(aFlags) {
    this._attentionSuppressed = aFlags;
    if (aFlags !== DownloadsCommon.SUPPRESS_NONE) {
      for (let download of this._downloads) {
        download.attention = DownloadsCommon.ATTENTION_NONE;
      }
      this.attention = DownloadsCommon.ATTENTION_NONE;
    }
  },
  get attentionSuppressed() {
    return this._attentionSuppressed;
  },
  _attentionSuppressed: DownloadsCommon.SUPPRESS_NONE,

  updateAttention() {
    let currentAttention = DownloadsCommon.ATTENTION_NONE;
    let currentPriority = 0;
    for (let download of this._downloads) {
      let { attention } = download;
      let priority = this._attentionPriority.get(attention);
      if (priority > currentPriority) {
        currentPriority = priority;
        currentAttention = attention;
      }
    }
    this.attention = currentAttention;
  },

  _updateView(aView) {
    aView.hasDownloads = this._hasDownloads;
    aView.percentComplete = this._percentComplete;
    aView.attention =
      this.attentionSuppressed !== DownloadsCommon.SUPPRESS_NONE
        ? DownloadsCommon.ATTENTION_NONE
        : this._attention;
  },


  _itemCount: 0,

  *_activeDownloads() {
    let downloads = this._isPrivate
      ? lazy.PrivateDownloadsData._downloads
      : lazy.DownloadsData._downloads;
    for (let download of downloads) {
      if (
        download.isInCurrentBatch ||
        (download.canceled && download.hasPartialData)
      ) {
        yield download;
      }
    }
  },

  _refreshProperties() {
    let summary = DownloadsCommon.summarizeDownloads(this._activeDownloads());

    this._hasDownloads = this._itemCount > 0;

    if (summary.percentComplete >= 0) {
      this._percentComplete = summary.percentComplete;
    } else if (summary.numDownloading > 0) {
      this._percentComplete = 0;
    } else {
      this._percentComplete = -1;
    }
  },
};
Object.setPrototypeOf(
  DownloadsIndicatorDataCtor.prototype,
  DownloadsViewPrototype
);

ChromeUtils.defineLazyGetter(
  lazy,
  "PrivateDownloadsIndicatorData",
  function () {
    return new DownloadsIndicatorDataCtor(true);
  }
);

ChromeUtils.defineLazyGetter(lazy, "DownloadsIndicatorData", function () {
  return new DownloadsIndicatorDataCtor(false);
});


function DownloadsSummaryData(aIsPrivate, aNumToExclude) {
  this._numToExclude = aNumToExclude;
  this._loading = false;

  this._downloads = [];

  this._lastRawTimeLeft = -1;

  this._lastTimeLeft = -1;

  this._showingProgress = false;
  this._details = "";
  this._description = "";
  this._numActive = 0;
  this._percentComplete = -1;

  this._oldDownloadStates = new WeakMap();
  this._isPrivate = aIsPrivate;
  this._views = [];
}

DownloadsSummaryData.prototype = {
  removeView(aView) {
    DownloadsViewPrototype.removeView.call(this, aView);

    if (!this._views.length) {
      this._downloads = [];
    }
  },

  onDownloadAdded(download) {
    DownloadsViewPrototype.onDownloadAdded.call(this, download);
    this._downloads.unshift(download);
    this._updateViews();
  },

  onDownloadStateChanged() {
    this._lastRawTimeLeft = -1;
    this._lastTimeLeft = -1;
  },

  onDownloadChanged(download) {
    DownloadsViewPrototype.onDownloadChanged.call(this, download);
    this._updateViews();
  },

  onDownloadRemoved(download) {
    DownloadsViewPrototype.onDownloadRemoved.call(this, download);
    let itemIndex = this._downloads.indexOf(download);
    this._downloads.splice(itemIndex, 1);
    this._updateViews();
  },


  _updateView(aView) {
    aView.showingProgress = this._showingProgress;
    aView.percentComplete = this._percentComplete;
    aView.description = this._description;
    aView.details = this._details;
  },


  *_downloadsForSummary() {
    if (this._downloads.length) {
      for (let i = this._numToExclude; i < this._downloads.length; ++i) {
        yield this._downloads[i];
      }
    }
  },

  _refreshProperties() {
    let summary = DownloadsCommon.summarizeDownloads(
      this._downloadsForSummary()
    );

    this._description = kDownloadsFluentStrings.formatValueSync(
      "downloads-more-downloading",
      {
        count: summary.numDownloading,
      }
    );
    this._percentComplete = summary.percentComplete;

    this._showingProgress = summary.numDownloading > 0;

    if (summary.rawTimeLeft == -1) {
      this._lastRawTimeLeft = -1;
      this._lastTimeLeft = -1;
      this._details = "";
    } else {
      if (this._lastRawTimeLeft != summary.rawTimeLeft) {
        this._lastRawTimeLeft = summary.rawTimeLeft;
        this._lastTimeLeft = DownloadsCommon.smoothSeconds(
          summary.rawTimeLeft,
          this._lastTimeLeft
        );
      }
      [this._details] = lazy.DownloadUtils.getDownloadStatusNoRate(
        summary.totalTransferred,
        summary.totalSize,
        summary.slowestSpeed,
        this._lastTimeLeft
      );
    }
  },
};
Object.setPrototypeOf(DownloadsSummaryData.prototype, DownloadsViewPrototype);
