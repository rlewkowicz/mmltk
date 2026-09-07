/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

ChromeUtils.defineLazyGetter(
  lazy,
  "l10n",
  () => new Localization(["toolkit/downloads/downloadUI.ftl"], true)
);

export var DownloadUIHelper = {
  getPrompter(aParent) {
    return new DownloadPrompter(aParent || null);
  },

  loadFileIn(
    file,
    {
      chromeWindow: browserWin,
      openWhere = "tab",
      isPrivate,
      userContextId = 0,
      browsingContextId = 0,
      openInBackgroundIfSwitchedBrowsingContext = false,
    } = {}
  ) {
    let fileURI = Services.io.newFileURI(file);
    let allowPrivate =
      isPrivate || lazy.PrivateBrowsingUtils.permanentPrivateBrowsing;

    if (
      !browserWin ||
      browserWin.document.documentElement.getAttribute("windowtype") !==
        "navigator:browser"
    ) {
      browserWin = lazy.BrowserWindowTracker.getTopWindow({
        private: allowPrivate,
      });
    }
    if (!browserWin) {
      let args = Cc["@mozilla.org/array;1"].createInstance(Ci.nsIMutableArray);
      let strURI = Cc["@mozilla.org/supports-string;1"].createInstance(
        Ci.nsISupportsString
      );
      strURI.data = fileURI.spec;
      args.appendElement(strURI);
      let features = "chrome,dialog=no,all";
      if (isPrivate) {
        features += ",private";
      }
      browserWin = Services.ww.openWindow(
        null,
        AppConstants.BROWSER_CHROME_URL,
        null,
        features,
        args
      );
      return;
    }

    let browsingContext = browserWin?.BrowsingContext.get(browsingContextId);
    let openerBrowser = browsingContext?.top?.embedderElement;

    let inBackground =
      openInBackgroundIfSwitchedBrowsingContext &&
      !!browsingContextId &&
      openerBrowser != browserWin.gBrowser.selectedBrowser;

    browserWin.openTrustedLinkIn(fileURI.spec, openWhere, {
      triggeringPrincipal: Services.scriptSecurityManager.getSystemPrincipal(),
      private: isPrivate,
      userContextId,
      openerBrowser,
      inBackground,
    });
  },
};

var DownloadPrompter = function (aParent) {
  this._prompter = Services.ww.getNewPrompter(aParent);
};

DownloadPrompter.prototype = {
  ON_QUIT: "prompt-on-quit",
  ON_OFFLINE: "prompt-on-offline",
  ON_LEAVE_PRIVATE_BROWSING: "prompt-on-leave-private-browsing",

  _prompter: null,

  async confirmLaunchExecutable(path) {
    const kPrefSkipConfirm = "browser.download.skipConfirmLaunchExecutable";

    if (!this._prompter) {
      return true;
    }

    try {
      if (Services.prefs.getBoolPref(kPrefSkipConfirm)) {
        return true;
      }
    } catch (ex) {
    }

    const title = lazy.l10n.formatValueSync(
      "download-ui-file-executable-security-warning-title"
    );
    const message = lazy.l10n.formatValueSync(
      "download-ui-file-executable-security-warning",
      { executable: PathUtils.filename(path) }
    );
    let flags =
      Ci.nsIPrompt.BUTTON_DELAY_ENABLE | Ci.nsIPrompt.STD_OK_CANCEL_BUTTONS;
    let nulls = Array(4).fill(null);
    return 0 == this._prompter.confirmEx(title, message, flags, ...nulls, {});
  },

  confirmCancelDownloads: function DP_confirmCancelDownload(
    aDownloadsCount,
    aPromptType
  ) {
    if (!this._prompter || aDownloadsCount <= 0) {
      return false;
    }

    let message, cancelButton;

    switch (aPromptType) {
      case this.ON_QUIT:
        message =
          AppConstants.platform == "macosx"
            ? "download-ui-confirm-quit-cancel-downloads-mac"
            : "download-ui-confirm-quit-cancel-downloads";
        cancelButton = "download-ui-dont-quit-button";
        break;

      case this.ON_OFFLINE:
        message = "download-ui-confirm-offline-cancel-downloads";
        cancelButton = "download-ui-dont-go-offline-button";
        break;

      case this.ON_LEAVE_PRIVATE_BROWSING:
        message =
          "download-ui-confirm-leave-private-browsing-windows-cancel-downloads";
        cancelButton = "download-ui-dont-leave-private-browsing-button";
        break;
    }

    const buttonFlags =
      Ci.nsIPrompt.BUTTON_TITLE_IS_STRING * Ci.nsIPrompt.BUTTON_POS_0 +
      Ci.nsIPrompt.BUTTON_TITLE_IS_STRING * Ci.nsIPrompt.BUTTON_POS_1;

    let rv = this._prompter.confirmEx(
      lazy.l10n.formatValueSync("download-ui-confirm-title"),
      lazy.l10n.formatValueSync(message, { downloadsCount: aDownloadsCount }),
      buttonFlags,
      lazy.l10n.formatValueSync("download-ui-cancel-downloads-ok", {
        downloadsCount: aDownloadsCount,
      }),
      lazy.l10n.formatValueSync(cancelButton),
      null,
      null,
      {}
    );
    return rv == 1;
  },
};
