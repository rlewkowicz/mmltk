/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const PREF_STORAGE_VERSION = "browser.pagethumbnails.storage_version";
const LATEST_STORAGE_VERSION = 3;

const EXPIRATION_MIN_CHUNK_SIZE = 50;
const EXPIRATION_INTERVAL_SECS = 3600;

const MAX_THUMBNAIL_AGE_SECS = 172800; 

const THUMBNAIL_DIRECTORY = "thumbnails";

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

import { BasePromiseWorker } from "resource://gre/modules/PromiseWorker.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PageThumbUtils: "resource://gre/modules/PageThumbUtils.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "gUpdateTimerManager",
  "@mozilla.org/updates/timer-manager;1",
  Ci.nsIUpdateTimerManager
);

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "PageThumbsStorageService",
  "@mozilla.org/thumbnails/pagethumbs-service;1",
  Ci.nsIPageThumbsStorageService
);

const TaskUtils = {
  readBlob: function readBlob(blob) {
    return new Promise((resolve, reject) => {
      let reader = new FileReader();
      reader.onloadend = function onloadend() {
        if (reader.readyState != FileReader.DONE) {
          reject(reader.error);
        } else {
          resolve(reader.result);
        }
      };
      reader.readAsArrayBuffer(blob);
    });
  },
};

export var PageThumbs = {
  _initialized: false,

  _thumbnailWidth: 0,
  _thumbnailHeight: 0,

  get scheme() {
    return "moz-page-thumb";
  },

  get staticHost() {
    return "thumbnails";
  },

  get contentType() {
    return "image/png";
  },

  init: function PageThumbs_init() {
    if (!this._initialized) {
      this._initialized = true;

      this._placesObserver = new PlacesWeakCallbackWrapper(
        this.handlePlacesEvents.bind(this)
      );
      PlacesObservers.addListener(
        ["history-cleared", "page-removed"],
        this._placesObserver
      );

      PageThumbsStorageMigrator.migrate();
      PageThumbsExpiration.init();
    }
  },

  handlePlacesEvents(events) {
    for (const event of events) {
      switch (event.type) {
        case "history-cleared": {
          PageThumbsStorage.wipe();
          break;
        }
        case "page-removed": {
          if (event.isRemovedFromStore) {
            PageThumbsStorage.remove(event.url);
          }
          break;
        }
      }
    }
  },

  uninit: function PageThumbs_uninit() {
    if (this._initialized) {
      this._initialized = false;
    }
  },

  getThumbnailURL: function PageThumbs_getThumbnailURL(aUrl) {
    return (
      this.scheme +
      "://" +
      this.staticHost +
      "/?url=" +
      encodeURIComponent(aUrl) +
      "&revision=" +
      PageThumbsStorage.getRevision(aUrl)
    );
  },

  getThumbnailPath: function PageThumbs_getThumbnailPath(aUrl) {
    return lazy.PageThumbsStorageService.getFilePathForURL(aUrl);
  },

  captureToBlob: function PageThumbs_captureToBlob(aBrowser, aArgs) {
    if (!this._prefEnabled()) {
      return null;
    }

    return new Promise(resolve => {
      let canvas = this.createCanvas(aBrowser.documentGlobal);
      this.captureToCanvas(aBrowser, canvas, aArgs)
        .then(() => {
          canvas.toBlob(blob => {
            resolve(blob, this.contentType);
          });
        })
        .catch(e => console.error(e));
    });
  },

  async captureToCanvas(aBrowser, aCanvas, aArgs, aSkipTelemetry = false) {
    let telemetryCaptureTime = new Date();
    let args = {
      fullScale: aArgs ? aArgs.fullScale : false,
      isImage: aArgs ? aArgs.isImage : false,
      backgroundColor:
        aArgs?.backgroundColor ?? lazy.PageThumbUtils.THUMBNAIL_BG_COLOR,
      targetWidth:
        aArgs?.targetWidth ?? lazy.PageThumbUtils.THUMBNAIL_DEFAULT_SIZE,
      preserveAspectRatio: aArgs?.preserveAspectRatio ?? false,
      isBackgroundThumb: aArgs ? aArgs.isBackgroundThumb : false,
      fullViewport: aArgs?.fullViewport ?? false,
    };

    return this._captureToCanvas(aBrowser, aCanvas, args).then(() => {
      if (!aSkipTelemetry) {
      }
      return aCanvas;
    });
  },

  async shouldStoreThumbnail(aBrowser) {
    if (lazy.PrivateBrowsingUtils.isBrowserPrivate(aBrowser)) {
      return false;
    }
    if (aBrowser.isRemoteBrowser) {
      if (aBrowser.browsingContext.currentWindowGlobal) {
        let thumbnailsActor =
          aBrowser.browsingContext.currentWindowGlobal.getActor("Thumbnails");
        return thumbnailsActor
          .sendQuery("Browser:Thumbnail:CheckState")
          .catch(() => {
            return false;
          });
      }
      return false;
    }
    return lazy.PageThumbUtils.shouldStoreContentThumbnail(
      aBrowser.contentDocument,
      aBrowser.docShell
    );
  },

  async _captureToCanvas(aBrowser, aCanvas, aArgs) {
    if (aBrowser.isRemoteBrowser) {
      let thumbnail = await this._captureRemoteThumbnail(
        aBrowser,
        aCanvas.width,
        aCanvas.height,
        aArgs
      );

      if (thumbnail) {
        let ctx = thumbnail.getContext("2d");
        let imgData = ctx.getImageData(0, 0, thumbnail.width, thumbnail.height);
        aCanvas.width = thumbnail.width;
        aCanvas.height = thumbnail.height;
        aCanvas.getContext("2d").putImageData(imgData, 0, 0);
      }

      return aCanvas;
    }
    await lazy.PageThumbUtils.createSnapshotThumbnail(aBrowser, aCanvas, aArgs);
    return aCanvas;
  },

  async _captureRemoteThumbnail(aBrowser, aWidth, aHeight, aArgs) {
    if (!aBrowser.browsingContext || !aBrowser.isConnected) {
      return null;
    }

    let thumbnailsActor = aBrowser.browsingContext.currentWindowGlobal.getActor(
      aArgs.isBackgroundThumb ? "BackgroundThumbnails" : "Thumbnails"
    );
    let contentInfo = await thumbnailsActor.sendQuery(
      "Browser:Thumbnail:ContentInfo",
      {
        isImage: aArgs.isImage,
        targetWidth: aArgs.targetWidth,
        backgroundColor: aArgs.backgroundColor,
      }
    );

    let contentWidth = contentInfo.width;
    let contentHeight = contentInfo.height;
    if (contentWidth == 0 || contentHeight == 0) {
      throw new Error("IMAGE_ZERO_DIMENSION");
    }
    let aspectRatio = contentWidth / contentHeight;

    if (!aBrowser.isConnected) {
      return null;
    }
    let doc = aBrowser.ownerDocument;
    let thumbnail = doc.createElementNS(
      lazy.PageThumbUtils.HTML_NAMESPACE,
      "canvas"
    );

    let ctx = thumbnail.getContext("2d");
    if (contentInfo.imageData) {
      thumbnail.width = contentWidth;
      thumbnail.height = contentHeight;

      let imageData = new aBrowser.documentGlobal.ImageData(
        contentInfo.imageData,
        contentWidth,
        contentHeight
      );
      ctx.putImageData(imageData, 0, 0);
    } else {
      let fullScale = aArgs ? aArgs.fullScale : false;
      let targetWidth = aArgs.targetWidth ? aArgs.targetWidth : aWidth;
      let preserveAspectRatio = aArgs ? aArgs.preserveAspectRatio : false;
      let scale = 1;
      if (!fullScale) {
        let targetScale;
        if (preserveAspectRatio) {
          targetScale = targetWidth / contentWidth;
        } else {
          targetScale = Math.max(
            aWidth / contentWidth,
            aHeight / contentHeight
          );
        }
        scale = Math.min(targetScale, 1);
      }

      let image = await aBrowser.drawSnapshot(
        0,
        0,
        contentWidth,
        contentHeight,
        scale,
        aArgs.backgroundColor,
        aArgs.fullViewport
      );
      if (!image) {
        return null;
      }

      if (preserveAspectRatio) {
        thumbnail.width = targetWidth;
        thumbnail.height = targetWidth / aspectRatio;
      } else {
        thumbnail.width = fullScale ? contentWidth : aWidth;
        thumbnail.height = fullScale ? contentHeight : aHeight;
      }
      ctx.drawImage(image, 0, 0);
    }

    return thumbnail;
  },

  captureAndStore: async function PageThumbs_captureAndStore(aBrowser) {
    if (!this._prefEnabled()) {
      return;
    }

    let url = aBrowser.currentURI.spec;
    let originalURL;
    let channelError = false;

    if (!aBrowser.isRemoteBrowser) {
      let channel = aBrowser.docShell.currentDocumentChannel;
      originalURL = channel.originalURI.spec;
      channelError = lazy.PageThumbUtils.isChannelErrorResponse(channel);
    } else {
      let thumbnailsActor =
        aBrowser.browsingContext.currentWindowGlobal.getActor("Thumbnails");
      let resp = await thumbnailsActor.sendQuery(
        "Browser:Thumbnail:GetOriginalURL"
      );

      originalURL = resp.originalURL || url;
      channelError = resp.channelError;
    }

    try {
      let blob = await this.captureToBlob(aBrowser);
      let buffer = await TaskUtils.readBlob(blob);
      await this._store(originalURL, url, buffer, channelError);
    } catch (ex) {
      console.error("Exception thrown during thumbnail capture:", ex);
    }
  },

  captureAndStoreIfStale: async function PageThumbs_captureAndStoreIfStale(
    aBrowser
  ) {
    if (!aBrowser.currentURI) {
      return false;
    }
    let url = aBrowser.currentURI.spec;
    let recent;
    try {
      recent = await PageThumbsStorage.isFileRecentForURL(url);
    } catch {
      return false;
    }
    if (
      !recent &&
      aBrowser.currentURI &&
      aBrowser.currentURI.spec == url
    ) {
      await this.captureAndStore(aBrowser);
    }
    return true;
  },

  async captureTabPreviewThumbnail(aBrowser, aCanvas) {
    let desiredAspectRatio = aCanvas.width / aCanvas.height;

    let thumbnailsActor =
      aBrowser.browsingContext.currentWindowGlobal.getActor("Thumbnails");
    let contentInfo = await thumbnailsActor.sendQuery(
      "Browser:Thumbnail:ContentInfo"
    );

    let captureX = 0;
    let captureY = contentInfo.scrollY;
    let captureWidth = contentInfo.width;
    let captureHeight = captureWidth / desiredAspectRatio;
    let captureScale = aCanvas.width / captureWidth;

    let renderX = 0;
    let renderY = 0;
    let renderWidth = aCanvas.width;
    let renderHeight = aCanvas.height;

    if (contentInfo.documentHeight < captureHeight) {
      captureY = 0;
      captureHeight = contentInfo.documentHeight;
      captureScale = aCanvas.height / captureHeight;

      renderWidth = captureWidth * captureScale;
      renderHeight = aCanvas.height;


      renderX = (aCanvas.width - renderWidth) / 2;
      renderY = (aCanvas.height - renderHeight) / 2;
    }
    else if (contentInfo.documentHeight - captureY < captureHeight) {
      captureY = contentInfo.documentHeight - captureHeight;
    }
    let snapshotResult = await aBrowser.drawSnapshot(
      captureX,
      captureY,
      captureWidth,
      captureHeight,
      captureScale * 2,
      "transparent",
      false
    );
    aCanvas
      .getContext("2d")
      .drawImage(snapshotResult, renderX, renderY, renderWidth, renderHeight);
  },

  _store: async function PageThumbs__store(
    aOriginalURL,
    aFinalURL,
    aData,
    aNoOverwrite
  ) {
    let telemetryStoreTime = new Date();
    await PageThumbsStorage.writeData(aFinalURL, aData, aNoOverwrite);

    Services.obs.notifyObservers(null, "page-thumbnail:create", aFinalURL);
    if (aFinalURL != aOriginalURL) {
      await PageThumbsStorage.copy(aFinalURL, aOriginalURL, aNoOverwrite);
      Services.obs.notifyObservers(null, "page-thumbnail:create", aOriginalURL);
    }
  },

  addExpirationFilter: function PageThumbs_addExpirationFilter(aFilter) {
    PageThumbsExpiration.addFilter(aFilter);
  },

  removeExpirationFilter: function PageThumbs_removeExpirationFilter(aFilter) {
    PageThumbsExpiration.removeFilter(aFilter);
  },

  createCanvas: function PageThumbs_createCanvas(aWindow) {
    return lazy.PageThumbUtils.createCanvas(aWindow);
  },

  _prefEnabled: function PageThumbs_prefEnabled() {
    try {
      return !Services.prefs.getBoolPref(
        "browser.pagethumbnails.capturing_disabled"
      );
    } catch (e) {
      return true;
    }
  },
};

export var PageThumbsStorage = {
  ensurePath: function Storage_ensurePath() {
    return PageThumbsWorker.post("makeDir", [
      lazy.PageThumbsStorageService.path,
      { ignoreExisting: true },
    ]).catch(function onError(aReason) {
      console.error("Could not create thumbnails directory", aReason);
    });
  },

  _revisionTable: {},

  updateRevision(aURL) {
    let rev = this._revisionTable[aURL];
    if (rev == null) {
      rev = Math.floor(Math.random() * this._revisionRange);
    }
    this._revisionTable[aURL] = (rev + 1) % this._revisionRange;
  },

  _revisionRange: 8192,

  getRevision(aURL) {
    let rev = this._revisionTable[aURL];
    if (rev == null) {
      this.updateRevision(aURL);
      rev = this._revisionTable[aURL];
    }
    return rev;
  },

  writeData: function Storage_writeData(aURL, aData, aNoOverwrite) {
    let path = lazy.PageThumbsStorageService.getFilePathForURL(aURL);
    this.ensurePath();
    aData = new Uint8Array(aData);
    let msg = [
      path,
      aData,
      {
        tmpPath: path + ".tmp",
        mode: aNoOverwrite ? "create" : "overwrite",
      },
    ];
    return PageThumbsWorker.post(
      "writeAtomic",
      msg,
      msg 
    ).then(
      () => this.updateRevision(aURL),
      this._eatNoOverwriteError(aNoOverwrite)
    );
  },

  copy: function Storage_copy(aSourceURL, aTargetURL, aNoOverwrite) {
    this.ensurePath();
    let sourceFile =
      lazy.PageThumbsStorageService.getFilePathForURL(aSourceURL);
    let targetFile =
      lazy.PageThumbsStorageService.getFilePathForURL(aTargetURL);
    let options = { noOverwrite: aNoOverwrite };
    return PageThumbsWorker.post("copy", [
      sourceFile,
      targetFile,
      options,
    ]).then(
      () => this.updateRevision(aTargetURL),
      this._eatNoOverwriteError(aNoOverwrite)
    );
  },

  remove: function Storage_remove(aURL) {
    return PageThumbsWorker.post("remove", [
      lazy.PageThumbsStorageService.getFilePathForURL(aURL),
    ]);
  },

  wipe: async function Storage_wipe() {

    let blocker = () => undefined;

    IOUtils.profileBeforeChange.addBlocker(
      "PageThumbs: removing all thumbnails",
      blocker
    );


    let promise = PageThumbsWorker.post("wipe", [
      lazy.PageThumbsStorageService.path,
    ]);
    try {
      await promise;
    } finally {
      IOUtils.profileBeforeChange.removeBlocker(blocker);
    }
  },

  fileExistsForURL: function Storage_fileExistsForURL(aURL) {
    return PageThumbsWorker.post("exists", [
      lazy.PageThumbsStorageService.getFilePathForURL(aURL),
    ]);
  },

  isFileRecentForURL: function Storage_isFileRecentForURL(aURL) {
    return PageThumbsWorker.post("isFileRecent", [
      lazy.PageThumbsStorageService.getFilePathForURL(aURL),
      MAX_THUMBNAIL_AGE_SECS,
    ]);
  },

  _eatNoOverwriteError: function Storage__eatNoOverwriteError(aNoOverwrite) {
    return function onError(err) {
      if (
        !aNoOverwrite ||
        !DOMException.isInstance(err) ||
        err.name !== "TypeMismatchError"
      ) {
        throw err;
      }
    };
  },
};

var PageThumbsStorageMigrator = {
  get currentVersion() {
    try {
      return Services.prefs.getIntPref(PREF_STORAGE_VERSION);
    } catch (e) {
      return 0;
    }
  },

  set currentVersion(aVersion) {
    Services.prefs.setIntPref(PREF_STORAGE_VERSION, aVersion);
  },

  migrate: function Migrator_migrate() {
    let version = this.currentVersion;



    if (version < 3) {
      this.migrateToVersion3();
    }

    this.currentVersion = LATEST_STORAGE_VERSION;
  },

  migrateToVersion3: function Migrator_migrateToVersion3(
    local = Services.dirsvc.get("ProfLD", Ci.nsIFile).path,
    roaming = Services.dirsvc.get("ProfD", Ci.nsIFile).path
  ) {
    PageThumbsWorker.post("moveOrDeleteAllThumbnails", [
      PathUtils.join(roaming, THUMBNAIL_DIRECTORY),
      PathUtils.join(local, THUMBNAIL_DIRECTORY),
    ]);
  },
};

export var PageThumbsExpiration = {
  _filters: [],

  init: function Expiration_init() {
    lazy.gUpdateTimerManager.registerTimer(
      "browser-cleanup-thumbnails",
      this,
      EXPIRATION_INTERVAL_SECS
    );
  },

  addFilter: function Expiration_addFilter(aFilter) {
    this._filters.push(aFilter);
  },

  removeFilter: function Expiration_removeFilter(aFilter) {
    let index = this._filters.indexOf(aFilter);
    if (index > -1) {
      this._filters.splice(index, 1);
    }
  },

  notify: function Expiration_notify() {
    let urls = [];
    let filtersToWaitFor = this._filters.length;

    let expire = () => {
      this.expireThumbnails(urls);
    };

    if (!filtersToWaitFor) {
      expire();
      return;
    }

    function filterCallback(aURLs) {
      urls = urls.concat(aURLs);
      if (--filtersToWaitFor == 0) {
        expire();
      }
    }

    for (let filter of this._filters) {
      if (typeof filter == "function") {
        filter(filterCallback);
      } else {
        filter.filterForThumbnailExpiration(filterCallback);
      }
    }
  },

  expireThumbnails: function Expiration_expireThumbnails(aURLsToKeep) {
    let keep = aURLsToKeep.map(url =>
      lazy.PageThumbsStorageService.getLeafNameForURL(url)
    );
    let msg = [
      lazy.PageThumbsStorageService.path,
      keep,
      EXPIRATION_MIN_CHUNK_SIZE,
    ];

    return PageThumbsWorker.post("expireFilesInDirectory", msg);
  },
};

var PageThumbsWorker = new BasePromiseWorker(
  "resource://gre/modules/PageThumbs.worker.js"
);
