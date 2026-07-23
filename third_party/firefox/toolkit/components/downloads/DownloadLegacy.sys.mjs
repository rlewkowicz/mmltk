/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  DownloadError: "resource://gre/modules/DownloadCore.sys.mjs",
  Downloads: "resource://gre/modules/Downloads.sys.mjs",
});

export function DownloadLegacyTransfer() {
  this._promiseDownload = new Promise(r => (this._resolveDownload = r));
}

DownloadLegacyTransfer.prototype = {
  classID: Components.ID("{1b4c85df-cbdd-4bb6-b04e-613caece083c}"),

  QueryInterface: ChromeUtils.generateQI([
    "nsIWebProgressListener",
    "nsIWebProgressListener2",
    "nsITransfer",
  ]),

  onStateChange: function DLT_onStateChange(
    aWebProgress,
    aRequest,
    aStateFlags,
    aStatus
  ) {
    if (!Components.isSuccessCode(aStatus)) {
      this._componentFailed = true;
    }

    if (
      aStateFlags & Ci.nsIWebProgressListener.STATE_START &&
      aStateFlags & Ci.nsIWebProgressListener.STATE_IS_NETWORK
    ) {
      this._promiseDownload
        .then(download => {
          download.source.triggeredByContentDispositionHeader = false;
          if (aRequest instanceof Ci.nsIHttpChannel) {
            try {
              download.source.triggeredByContentDispositionHeader =
                !!aRequest.contentDispositionHeader;
            } catch (e) {}
          }

          download.saver.onTransferStarted(aRequest);

          return download.saver.deferCanceled.promise.then(() => {
            if (this._cancelable && !this._componentFailed) {
              this._cancelable.cancel(Cr.NS_ERROR_ABORT);
            }
          });
        })
        .catch(console.error);
    } else if (
      aStateFlags & Ci.nsIWebProgressListener.STATE_STOP &&
      aStateFlags & Ci.nsIWebProgressListener.STATE_IS_NETWORK
    ) {
      this._promiseDownload
        .then(download => {
          if (Components.isSuccessCode(aStatus)) {
            download.saver.setRedirects(this._redirects);
          }
          download.saver.onTransferFinished(aStatus);
        })
        .catch(console.error);

      this._cancelable = null;
    }
  },

  onProgressChange: function DLT_onProgressChange(
    aWebProgress,
    aRequest,
    aCurSelfProgress,
    aMaxSelfProgress,
    aCurTotalProgress,
    aMaxTotalProgress
  ) {
    this.onProgressChange64(
      aWebProgress,
      aRequest,
      aCurSelfProgress,
      aMaxSelfProgress,
      aCurTotalProgress,
      aMaxTotalProgress
    );
  },

  onLocationChange() {},

  onStatusChange(webProgress, request, status, message) {
    if (!Components.isSuccessCode(status)) {
      this._componentFailed = true;

      this._promiseDownload
        .then(download => {
          download.saver.onTransferFinished(status, message);
        })
        .catch(console.error);
    }
  },

  onSecurityChange() {},

  onContentBlockingEvent() {},

  onProgressChange64: function DLT_onProgressChange64(
    aWebProgress,
    aRequest,
    aCurSelfProgress,
    aMaxSelfProgress,
    aCurTotalProgress,
    aMaxTotalProgress
  ) {
    if (this._download) {
      this._hasDelayedProgress = false;
      this._download.saver.onProgressBytes(
        aCurTotalProgress,
        aMaxTotalProgress
      );
      return;
    }

    this._delayedCurTotalProgress = aCurTotalProgress;
    this._delayedMaxTotalProgress = aMaxTotalProgress;

    if (this._hasDelayedProgress) {
      return;
    }
    this._hasDelayedProgress = true;

    this._promiseDownload
      .then(download => {
        if (!this._hasDelayedProgress) {
          return;
        }
        download.saver.onProgressBytes(
          this._delayedCurTotalProgress,
          this._delayedMaxTotalProgress
        );
      })
      .catch(console.error);
  },
  _hasDelayedProgress: false,
  _delayedCurTotalProgress: 0,
  _delayedMaxTotalProgress: 0,

  onRefreshAttempted: function DLT_onRefreshAttempted() {
    return true;
  },

  init: function DLT_init(
    aSource,
    aSourceOriginalURI,
    aTarget,
    aDisplayName,
    aMIMEInfo,
    aStartTime,
    aTempFile,
    aCancelable,
    aIsPrivate,
    aDownloadClassification,
    aReferrerInfo,
    aOpenDownloadsListOnStart
  ) {
    return this._nsITransferInitInternal(
      aSource,
      aSourceOriginalURI,
      aTarget,
      aDisplayName,
      aMIMEInfo,
      aStartTime,
      aTempFile,
      aCancelable,
      aIsPrivate,
      aDownloadClassification,
      aReferrerInfo,
      aOpenDownloadsListOnStart
    );
  },

  initWithBrowsingContext(
    aSource,
    aTarget,
    aDisplayName,
    aMIMEInfo,
    aStartTime,
    aTempFile,
    aCancelable,
    aIsPrivate,
    aDownloadClassification,
    aReferrerInfo,
    aOpenDownloadsListOnStart,
    aBrowsingContext,
    aHandleInternally,
    aHttpChannel
  ) {
    let browsingContextId;
    let userContextId;
    if (aBrowsingContext && aBrowsingContext.currentWindowGlobal) {
      browsingContextId = aBrowsingContext.id;
      let windowGlobal = aBrowsingContext.currentWindowGlobal;
      let originAttributes = windowGlobal.documentPrincipal.originAttributes;
      userContextId = originAttributes.userContextId;
    }
    return this._nsITransferInitInternal(
      aSource,
      null,
      aTarget,
      aDisplayName,
      aMIMEInfo,
      aStartTime,
      aTempFile,
      aCancelable,
      aIsPrivate,
      aDownloadClassification,
      aReferrerInfo,
      aOpenDownloadsListOnStart,
      userContextId,
      browsingContextId,
      aHandleInternally,
      aHttpChannel
    );
  },

  _nsITransferInitInternal(
    aSource,
    aSourceOriginalURI,
    aTarget,
    aDisplayName,
    aMIMEInfo,
    aStartTime,
    aTempFile,
    aCancelable,
    isPrivate,
    aDownloadClassification,
    referrerInfo,
    openDownloadsListOnStart = true,
    userContextId = 0,
    browsingContextId = 0,
    handleInternally = false,
    aHttpChannel = null
  ) {
    this._cancelable = aCancelable;
    let launchWhenSucceeded = false,
      contentType = null,
      launcherPath = null,
      launcherId = null;

    if (aMIMEInfo instanceof Ci.nsIMIMEInfo) {
      launchWhenSucceeded =
        aMIMEInfo.preferredAction != Ci.nsIMIMEInfo.saveToDisk;
      contentType = aMIMEInfo.type;

      let appHandler = aMIMEInfo.preferredApplicationHandler;
      if (aMIMEInfo.preferredAction == Ci.nsIMIMEInfo.useHelperApp) {
        if (appHandler instanceof Ci.nsILocalHandlerApp) {
          launcherPath = appHandler.executable.path;
        } else if (appHandler instanceof Ci.nsIGIOHandlerApp) {
          launcherId = appHandler.id;
        }
      }
    }
    let authHeader = null;
    if (aHttpChannel) {
      try {
        authHeader = aHttpChannel.getRequestHeader("Authorization");
      } catch (e) {}
    }
    let serialisedDownload = {
      source: {
        url: aSource.spec,
        originalUrl: aSourceOriginalURI && aSourceOriginalURI.spec,
        isPrivate,
        userContextId,
        browsingContextId,
        referrerInfo,
        authHeader,
      },
      target: {
        path: aTarget.QueryInterface(Ci.nsIFileURL).file.path,
        partFilePath: aTempFile && aTempFile.path,
      },
      saver: "legacy",
      launchWhenSucceeded,
      contentType,
      launcherPath,
      launcherId,
      handleInternally,
      openDownloadsListOnStart,
    };

    lazy.Downloads.createDownload(serialisedDownload)
      .then(async aDownload => {
        if (aTempFile) {
          aDownload.tryToKeepPartialData = true;
        }

        aDownload.start().catch(() => {});

        this._download = aDownload;
        this._resolveDownload(aDownload);

        await (await lazy.Downloads.getList(lazy.Downloads.ALL)).add(aDownload);
        if (serialisedDownload.errorObj) {
          aDownload._notifyChange();
        }
      })
      .catch(console.error);
  },

  get downloadPromise() {
    return this._promiseDownload;
  },

  setRedirects(redirects) {
    this._redirects = redirects;
  },

  _download: null,

  _promiseDownload: null,
  _resolveDownload: null,

  _cancelable: null,

  _componentFailed: false,

};
