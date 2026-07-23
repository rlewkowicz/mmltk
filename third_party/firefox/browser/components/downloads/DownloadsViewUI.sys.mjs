/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  DownloadUtils: "resource://gre/modules/DownloadUtils.sys.mjs",
  Downloads: "resource://gre/modules/Downloads.sys.mjs",
  DownloadsCommon:
    "moz-src:///browser/components/downloads/DownloadsCommon.sys.mjs",
  FileUtils: "resource://gre/modules/FileUtils.sys.mjs",
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "handlerSvc",
  "@mozilla.org/uriloader/handler-service;1",
  Ci.nsIHandlerService
);

import { Integration } from "resource://gre/modules/Integration.sys.mjs";

Integration.downloads.defineESModuleGetter(
  lazy,
  "DownloadIntegration",
  "resource://gre/modules/DownloadIntegration.sys.mjs"
);

const HTML_NS = "http://www.w3.org/1999/xhtml";

var gDownloadElementButtons = {
  cancel: {
    commandName: "downloadsCmd_cancel",
    l10nId: "downloads-cmd-cancel",
    descriptionL10nId: "downloads-cancel-download",
    panelL10nId: "downloads-cmd-cancel-panel",
    iconClass: "downloadIconCancel",
  },
  retry: {
    commandName: "downloadsCmd_retry",
    l10nId: "downloads-cmd-retry",
    descriptionL10nId: "downloads-retry-download",
    panelL10nId: "downloads-cmd-retry-panel",
    iconClass: "downloadIconRetry",
  },
  show: {
    commandName: "downloadsCmd_show",
    l10nId: "downloads-cmd-show-button-2",
    descriptionL10nId: "downloads-cmd-show-description-2",
    panelL10nId: "downloads-cmd-show-panel-2",
    iconClass: "downloadIconShow",
  },
};

var gDownloadListItemFragments = new WeakMap();

export var DownloadsViewUI = {
  isCommandName(name) {
    return name.startsWith("cmd_") || name.startsWith("downloadsCmd_");
  },

  getStrippedUrl(download) {
    return lazy.UrlbarUtils.stripPrefixAndTrim(download?.source?.url, {
      stripHttp: true,
      stripHttps: true,
    })[0];
  },

  getDisplayName(download) {
    return download.target.path
      ? PathUtils.filename(download.target.path)
      : download.source.url;
  },

  getSizeWithUnits(download) {
    if (download.target.size === undefined) {
      return "";
    }

    let [size, unit] = lazy.DownloadUtils.convertByteUnits(
      download.target.size
    );
    return lazy.DownloadsCommon.strings.sizeWithUnits(size, unit);
  },

  updateContextMenuForElement(contextMenu, element) {
    let state = parseInt(element.getAttribute("state"), 10);

    const document = contextMenu.ownerDocument;

    const {
      DOWNLOAD_NOTSTARTED,
      DOWNLOAD_DOWNLOADING,
      DOWNLOAD_FINISHED,
      DOWNLOAD_FAILED,
      DOWNLOAD_CANCELED,
      DOWNLOAD_PAUSED,
      DOWNLOAD_BLOCKED_POLICY,
    } = lazy.DownloadsCommon;

    contextMenu.querySelector(".downloadPauseMenuItem").hidden =
      state != DOWNLOAD_DOWNLOADING;

    contextMenu.querySelector(".downloadResumeMenuItem").hidden =
      state != DOWNLOAD_PAUSED;

    contextMenu.querySelector(".downloadRemoveFromHistoryMenuItem").hidden = ![
      DOWNLOAD_FINISHED,
      DOWNLOAD_FAILED,
      DOWNLOAD_CANCELED,
      DOWNLOAD_BLOCKED_POLICY,
    ].includes(state);

    contextMenu.querySelector(".downloadShowMenuItem").hidden =
      ![
        DOWNLOAD_NOTSTARTED,
        DOWNLOAD_DOWNLOADING,
        DOWNLOAD_FINISHED,
        DOWNLOAD_PAUSED,
      ].includes(state) ||
      (state == DOWNLOAD_FINISHED && !element.hasAttribute("exists"));

    contextMenu.querySelector(".downloadCommandsSeparator").hidden =
      contextMenu.querySelector(".downloadUnblockMenuItem").hidden &&
      contextMenu.querySelector(".downloadShowMenuItem").hidden;

    let download = element._shell.download;
    let mimeInfo = lazy.DownloadsCommon.getMimeInfo(download);
    let { preferredAction, useSystemDefault, defaultDescription } = mimeInfo
      ? mimeInfo
      : {};

    contextMenu.querySelector(".downloadDeleteFileMenuItem").hidden =
      download.deleted ||
      !(download.target?.exists || download.target?.partFileExists);

    contextMenu.querySelector(".downloadOpenReferrerMenuItem").hidden =
      !download.source.referrerInfo?.originalReferrer;

    let useSystemViewerItem = contextMenu.querySelector(
      ".downloadUseSystemDefaultMenuItem"
    );
    let alwaysUseSystemViewerItem = contextMenu.querySelector(
      ".downloadAlwaysUseSystemDefaultMenuItem"
    );
    let canViewInternally = element.hasAttribute("viewable-internally");
    useSystemViewerItem.hidden =
      !lazy.DownloadsCommon.openInSystemViewerItemEnabled ||
      !canViewInternally ||
      !download.target?.exists;

    alwaysUseSystemViewerItem.hidden =
      !lazy.DownloadsCommon.alwaysOpenInSystemViewerItemEnabled ||
      !canViewInternally;

    try {
      document.l10n.pauseObserving();
      if (defaultDescription && defaultDescription.length < 40) {
        document.l10n.setAttributes(
          useSystemViewerItem,
          "downloads-cmd-use-system-default-named",
          { handler: defaultDescription }
        );
        document.l10n.setAttributes(
          alwaysUseSystemViewerItem,
          "downloads-cmd-always-use-system-default-named",
          { handler: defaultDescription }
        );
      } else {
        document.l10n.setAttributes(
          useSystemViewerItem,
          "downloads-cmd-use-system-default"
        );
        document.l10n.setAttributes(
          alwaysUseSystemViewerItem,
          "downloads-cmd-always-use-system-default"
        );
      }
    } finally {
      document.l10n.resumeObserving();
    }
    document.l10n.translateElements([
      useSystemViewerItem,
      alwaysUseSystemViewerItem,
    ]);

    let alwaysOpenSimilarFilesItem = contextMenu.querySelector(
      ".downloadAlwaysOpenSimilarFilesMenuItem"
    );

    let shouldNotRememberChoice =
      !mimeInfo?.type ||
      mimeInfo.type === "application/octet-stream" ||
      mimeInfo.type === "application/x-msdownload" ||
      mimeInfo.type === "application/x-msdos-program";

    alwaysOpenSimilarFilesItem.hidden =
      canViewInternally ||
      state !== DOWNLOAD_FINISHED ||
      shouldNotRememberChoice;

    if (preferredAction === useSystemDefault) {
      alwaysUseSystemViewerItem.setAttribute("checked", "true");
      alwaysOpenSimilarFilesItem.setAttribute("checked", "true");
    } else {
      alwaysUseSystemViewerItem.removeAttribute("checked");
      alwaysOpenSimilarFilesItem.removeAttribute("checked");
    }
  },
};

XPCOMUtils.defineLazyPreferenceGetter(
  DownloadsViewUI,
  "clearHistoryOnDelete",
  "browser.download.clearHistoryOnDelete",
  0
);

DownloadsViewUI.BaseView = class {
  canClearDownloads(nodeContainer) {
    for (let elt = nodeContainer.lastChild; elt; elt = elt.previousSibling) {
      let download = elt._shell.download;
      if (download.stopped && !(download.canceled && download.hasPartialData)) {
        return true;
      }
    }
    return false;
  }
};

DownloadsViewUI.DownloadElementShell = function () {};

DownloadsViewUI.DownloadElementShell.prototype = {
  element: null,

  ensureActive() {
    if (!this._active) {
      this._active = true;
      this.connect();
      this.onChanged();
    }
  },
  get active() {
    return !!this._active;
  },

  connect() {
    let document = this.element.ownerDocument;
    let downloadListItemFragment = gDownloadListItemFragments.get(document);
    if (!downloadListItemFragment) {
      let MozXULElement = document.defaultView.MozXULElement;
      downloadListItemFragment = MozXULElement.parseXULToFragment(`
        <hbox class="downloadMainArea" flex="1" align="center">
          <image class="downloadTypeIcon"/>
          <vbox class="downloadContainer" flex="1" pack="center">
            <description class="downloadTarget" crop="center"/>
            <description class="downloadDetails downloadDetailsNormal"
                         crop="end"/>
            <description class="downloadDetails downloadDetailsHover"
                         crop="end"/>
            <description class="downloadDetails downloadDetailsButtonHover"
                         crop="end"/>
          </vbox>
          <image class="downloadBlockedBadge" />
        </hbox>
        <button class="downloadButton"/>
      `);
      gDownloadListItemFragments.set(document, downloadListItemFragment);
    }
    this.element.setAttribute("active", true);
    this.element.setAttribute("orient", "horizontal");
    this.element.addEventListener("click", ev => {
      ev.target.documentGlobal.DownloadsView.onDownloadClick(ev);
    });
    this.element.appendChild(
      document.importNode(downloadListItemFragment, true)
    );
    let downloadButton = this.element.querySelector(".downloadButton");
    downloadButton.addEventListener("command", function (event) {
      event.target.documentGlobal.DownloadsView.onDownloadButton(event);
    });
    for (let [propertyName, selector] of [
      ["_downloadTypeIcon", ".downloadTypeIcon"],
      ["_downloadTarget", ".downloadTarget"],
      ["_downloadDetailsNormal", ".downloadDetailsNormal"],
      ["_downloadDetailsHover", ".downloadDetailsHover"],
      ["_downloadDetailsButtonHover", ".downloadDetailsButtonHover"],
      ["_downloadButton", ".downloadButton"],
    ]) {
      this[propertyName] = this.element.querySelector(selector);
    }

    let progress = (this._downloadProgress = document.createElementNS(
      HTML_NS,
      "progress"
    ));
    progress.className = "downloadProgress";
    progress.setAttribute("max", "100");
    this._downloadTarget.insertAdjacentElement("afterend", progress);
  },

  get image() {
    if (!this.download.target.path) {
      return "moz-icon://.unknown?size=32";
    }

    return (
      "moz-icon://" +
      this.download.target.path +
      "?size=32" +
      (this.download.succeeded ? "&state=normal" : "")
    );
  },

  get browserWindow() {
    return lazy.BrowserWindowTracker.getTopWindow({
      allowFromInactiveWorkspace: true,
    });
  },

  showDisplayNameAndIcon(displayName, icon) {
    if (displayName.l10n) {
      let document = this.element.ownerDocument;
      document.l10n.setAttributes(
        this._downloadTarget,
        displayName.l10n.id,
        displayName.l10n.args
      );
    } else {
      this._downloadTarget.setAttribute("value", displayName);
      this._downloadTarget.setAttribute("tooltiptext", displayName);
    }
    this._downloadTypeIcon.setAttribute("src", icon);
  },

  showProgress(mode, value, paused) {
    if (mode == "undetermined") {
      this._downloadProgress.removeAttribute("value");
    } else {
      this._downloadProgress.setAttribute("value", value);
    }
    this._downloadProgress.toggleAttribute("paused", !!paused);
  },

  showStatus(status, hoverStatus = status) {
    let document = this.element.ownerDocument;
    if (status?.l10n) {
      document.l10n.setAttributes(
        this._downloadDetailsNormal,
        status.l10n.id,
        status.l10n.args
      );
    } else {
      this._downloadDetailsNormal.removeAttribute("data-l10n-id");
      this._downloadDetailsNormal.setAttribute("value", status);
      this._downloadDetailsNormal.setAttribute("tooltiptext", status);
    }
    if (hoverStatus?.l10n) {
      document.l10n.setAttributes(
        this._downloadDetailsHover,
        hoverStatus.l10n.id,
        hoverStatus.l10n.args
      );
    } else {
      this._downloadDetailsHover.removeAttribute("data-l10n-id");
      this._downloadDetailsHover.setAttribute("value", hoverStatus);
      this._downloadDetailsHover.setAttribute("tooltiptext", hoverStatus);
    }
  },

  showStatusWithDetails(stateLabel, hoverStatus) {
    if (stateLabel.l10n) {
      this.showStatus(stateLabel, hoverStatus);
      return;
    }
    let uri = URL.parse(this.download.source.url)?.URI;
    let displayHost = uri
      ? lazy.BrowserUtils.formatURIForDisplay(uri, {
          onlyBaseDomain: true,
        })
      : "";

    let [displayDate] = lazy.DownloadUtils.getReadableDates(
      new Date(this.download.endTime)
    );

    let firstPart = lazy.DownloadsCommon.strings.statusSeparator(
      stateLabel,
      displayHost
    );
    let fullStatus = lazy.DownloadsCommon.strings.statusSeparator(
      firstPart,
      displayDate
    );

    if (!this.isPanel) {
      this.showStatus(fullStatus);
    } else {
      this.showStatus(stateLabel, hoverStatus || fullStatus);
    }
  },

  showButton(type) {
    let { commandName, l10nId, descriptionL10nId, panelL10nId, iconClass } =
      gDownloadElementButtons[type];

    this.buttonCommandName = commandName;
    let stringId = this.isPanel ? panelL10nId : l10nId;
    let document = this.element.ownerDocument;
    document.l10n.setAttributes(this._downloadButton, stringId);
    if (this.isPanel && descriptionL10nId) {
      document.l10n.setAttributes(
        this._downloadDetailsButtonHover,
        descriptionL10nId
      );
    }
    this._downloadButton.setAttribute("class", "downloadButton " + iconClass);
    this._downloadButton.removeAttribute("hidden");
  },

  hideButton() {
    this._downloadButton.hidden = true;
  },

  lastEstimatedSecondsLeft: Infinity,

  _updateState() {
    this.showDisplayNameAndIcon(
      DownloadsViewUI.getDisplayName(this.download),
      this.image
    );
    this.element.setAttribute(
      "state",
      lazy.DownloadsCommon.stateOfDownload(this.download)
    );

    if (!this.download.stopped) {
      this.showButton("cancel");

      this.element.removeAttribute("verdict");
    }

    this.lastEstimatedSecondsLeft = Infinity;

    this._updateStateInner();
  },

  _updateStateInner() {
    let progressPaused = false;

    this.element.classList.toggle("openWhenFinished", !this.download.stopped);

    if (!this.download.stopped) {
      let totalBytes = this.download.hasProgress
        ? this.download.totalBytes
        : -1;
      let [status, newEstimatedSecondsLeft] =
        lazy.DownloadUtils.getDownloadStatus(
          this.download.currentBytes,
          totalBytes,
          this.download.speed,
          this.lastEstimatedSecondsLeft
        );
      this.lastEstimatedSecondsLeft = newEstimatedSecondsLeft;

      if (this.download.launchWhenSucceeded) {
        status = lazy.DownloadUtils.getFormattedTimeStatus(
          newEstimatedSecondsLeft
        );
      }
      let hoverStatus = {
        l10n: { id: "downloading-file-click-to-open" },
      };
      this.showStatus(status, hoverStatus);
    } else {
      let verdict = "";

      if (this.download.deleted) {
        this.showDeletedOrMissing();
      } else if (this.download.succeeded) {
        lazy.DownloadsCommon.log(
          "_updateStateInner, target exists? ",
          this.download.target.path,
          this.download.target.exists
        );
        if (this.download.target.exists) {
          this.element.setAttribute("exists", "true");

          this.element.toggleAttribute(
            "viewable-internally",
            lazy.DownloadIntegration.shouldViewDownloadInternally(
              lazy.DownloadsCommon.getMimeInfo(this.download)?.type
            )
          );

          let sizeWithUnits = DownloadsViewUI.getSizeWithUnits(this.download);
          if (this.isPanel) {
            let status = lazy.DownloadsCommon.strings.stateCompleted;
            if (sizeWithUnits) {
              status = lazy.DownloadsCommon.strings.statusSeparator(
                status,
                sizeWithUnits
              );
            }
            this.showStatus(status, { l10n: { id: "downloads-open-file" } });
          } else {
            this.showStatusWithDetails(
              sizeWithUnits || lazy.DownloadsCommon.strings.sizeUnknown
            );
          }
          this.showButton("show");
        } else {
          this.showDeletedOrMissing();
        }
      } else if (this.download.error) {
        this.showStatusWithDetails(
          lazy.DownloadsCommon.strings.stateFailed,
          this.download.error.localizedReason
        );
        this.showButton("retry");
      } else if (this.download.canceled) {
        if (this.download.hasPartialData) {
          let totalBytes = this.download.hasProgress
            ? this.download.totalBytes
            : -1;
          let transfer = lazy.DownloadUtils.getTransferTotal(
            this.download.currentBytes,
            totalBytes
          );
          this.showStatus(
            lazy.DownloadsCommon.strings.statusSeparatorBeforeNumber(
              lazy.DownloadsCommon.strings.statePaused,
              transfer
            )
          );
          this.showButton("cancel");
          progressPaused = true;
        } else {
          this.showStatusWithDetails(
            lazy.DownloadsCommon.strings.stateCanceled
          );
          this.showButton("retry");
        }
      } else {
        this.showStatus(lazy.DownloadsCommon.strings.stateStarting);
        this.showButton("cancel");
      }

      this.element.removeAttribute("verdict");
    }

    if (this.download.hasProgress) {
      this.showProgress("normal", this.download.progress, progressPaused);
    } else {
      this.showProgress("undetermined", 100, progressPaused);
    }
  },


  showDeletedOrMissing() {
    this.element.removeAttribute("exists");
    let stringKey;
    if (this.download.deleted && this.download.error?.becauseBlocked) {
      stringKey = "fileBlockedAndDeleted";
    } else if (this.download.deleted) {
      stringKey = "fileDeleted";
    } else {
      stringKey = "fileMovedOrMissing";
    }
    let label = lazy.DownloadsCommon.strings[stringKey];
    this.showStatusWithDetails(label, label);
    this.hideButton();
  },


  get currentDefaultCommandName() {
    switch (lazy.DownloadsCommon.stateOfDownload(this.download)) {
      case lazy.DownloadsCommon.DOWNLOAD_NOTSTARTED:
        return "downloadsCmd_cancel";
      case lazy.DownloadsCommon.DOWNLOAD_FAILED:
      case lazy.DownloadsCommon.DOWNLOAD_CANCELED:
        return "downloadsCmd_retry";
      case lazy.DownloadsCommon.DOWNLOAD_PAUSED:
        return "downloadsCmd_pauseResume";
      case lazy.DownloadsCommon.DOWNLOAD_FINISHED:
        return "downloadsCmd_open";
    }
    return "";
  },

  isCommandEnabled(aCommand) {
    switch (aCommand) {
      case "downloadsCmd_retry":
        return this.download.canceled || !!this.download.error;
      case "downloadsCmd_pauseResume":
        return this.download.hasPartialData && !this.download.error;
      case "downloadsCmd_openReferrer": {
        let referrer = this.download.source.referrerInfo?.originalReferrer;
        return !!referrer && referrer.asciiSpec != "about:blank";
      }
      case "downloadsCmd_cancel":
        return this.download.hasPartialData || !this.download.stopped;
      case "downloadsCmd_open":
      case "downloadsCmd_open:current":
      case "downloadsCmd_open:tab":
      case "downloadsCmd_open:tabshifted":
      case "downloadsCmd_open:window":
      case "downloadsCmd_alwaysOpenSimilarFiles":
        return this.download.target.exists;

      case "downloadsCmd_show":
      case "downloadsCmd_deleteFile": {
        let { target } = this.download;
        return (
          !this.download.deleted && (target.exists || target.partFileExists)
        );
      }
      case "downloadsCmd_delete":
      case "cmd_delete":
        return this.download.stopped;
      case "downloadsCmd_openInSystemViewer":
      case "downloadsCmd_alwaysOpenInSystemViewer":
        return lazy.DownloadIntegration.shouldViewDownloadInternally(
          lazy.DownloadsCommon.getMimeInfo(this.download)?.type
        );
    }
    return DownloadsViewUI.isCommandName(aCommand) && !!this[aCommand];
  },

  doCommand(aCommand) {
    let [command, modifier] = aCommand.split(":");
    if (DownloadsViewUI.isCommandName(command)) {
      this[command](modifier);
    }
  },

  onButton() {
    this.doCommand(this.buttonCommandName);
  },

  downloadsCmd_cancel() {
    this.download.cancel().catch(() => {});
    this.download
      .removePartialData()
      .catch(console.error)
      .finally(() => this.download.target.refresh());
  },

  downloadsCmd_open(openWhere = "tab") {
    lazy.DownloadsCommon.openDownload(this.download, {
      openWhere,
    });
  },

  downloadsCmd_openReferrer() {
    this.element.documentGlobal.openURL(
      this.download.source.referrerInfo.originalReferrer
    );
  },

  downloadsCmd_pauseResume() {
    if (this.download.stopped) {
      this.download.start();
    } else {
      this.download.cancel();
    }
  },

  downloadsCmd_show() {
    let file = new lazy.FileUtils.File(this.download.target.path);
    lazy.DownloadsCommon.showDownloadedFile(file);
  },

  downloadsCmd_retry() {
    if (this.download.start) {
      this.download.start().catch(() => {});
      return;
    }

    let window = this.browserWindow || this.element.documentGlobal;
    let document = window.document;

    let targetPath = this.download.target.path
      ? PathUtils.filename(this.download.target.path)
      : null;
    window.DownloadURL(this.download.source.url, targetPath, document);
  },

  downloadsCmd_delete() {
    this.cmd_delete();
  },

  cmd_delete() {
    lazy.DownloadsCommon.deleteDownload(this.download).catch(console.error);
  },

  async downloadsCmd_deleteFile() {
    await lazy.DownloadsCommon.deleteDownloadFiles(
      this.download,
      DownloadsViewUI.clearHistoryOnDelete
    );
  },

  downloadsCmd_openInSystemViewer() {
    lazy.DownloadsCommon.openDownload(this.download, {
      useSystemDefault: true,
    }).catch(console.error);
  },

  downloadsCmd_alwaysOpenInSystemViewer() {
    const mimeInfo = lazy.DownloadsCommon.getMimeInfo(this.download);
    if (!mimeInfo) {
      throw new Error(
        "Can't open download with unknown mime-type in system viewer"
      );
    }
    if (mimeInfo.preferredAction !== mimeInfo.useSystemDefault) {
      lazy.DownloadsCommon.log(
        "downloadsCmd_alwaysOpenInSystemViewer command for download: ",
        this.download,
        "switching to use system default for " + mimeInfo.type
      );
      mimeInfo.preferredAction = mimeInfo.useSystemDefault;
      mimeInfo.alwaysAskBeforeHandling = false;
    } else {
      lazy.DownloadsCommon.log(
        "downloadsCmd_alwaysOpenInSystemViewer command for download: ",
        this.download,
        "currently uses system default, switching to handleInternally"
      );
      mimeInfo.preferredAction = mimeInfo.handleInternally;
    }
    lazy.handlerSvc.store(mimeInfo);
    lazy.DownloadsCommon.openDownload(this.download).catch(console.error);
  },

  downloadsCmd_alwaysOpenSimilarFiles() {
    const mimeInfo = lazy.DownloadsCommon.getMimeInfo(this.download);
    if (!mimeInfo) {
      throw new Error("Can't open download with unknown mime-type");
    }

    if (mimeInfo.preferredAction !== mimeInfo.useSystemDefault) {
      mimeInfo.preferredAction = mimeInfo.useSystemDefault;
      lazy.handlerSvc.store(mimeInfo);
      lazy.DownloadsCommon.openDownload(this.download).catch(console.error);
    } else {
      mimeInfo.preferredAction = mimeInfo.saveToDisk;
      lazy.handlerSvc.store(mimeInfo);
    }
  },
};
