/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

let lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  CommonDialog: "resource://gre/modules/CommonDialog.sys.mjs",
  SessionStartup: "resource:///modules/sessionstore/SessionStartup.sys.mjs",
});

export var DefaultBrowserCheck = {
  async prompt(win) {
    const shellService = win.getShellService();
    const needPin =
      (await shellService.doesAppNeedPin()) ||
      (await shellService.doesAppNeedStartMenuPin());

    win.MozXULElement.insertFTLIfNeeded("branding/brand.ftl");
    win.MozXULElement.insertFTLIfNeeded(
      "browser/defaultBrowserNotification.ftl"
    );
    let now = Math.floor(Date.now() / 1000).toString();
    Services.prefs.setCharPref(
      "browser.shell.mostRecentDefaultPromptSeen",
      now
    );


    let pinMessage;
    if (AppConstants.platform == "macosx") {
      pinMessage = "default-browser-prompt-message-pin-mac";
    } else if (
      AppConstants.platform == "win" &&
      Services.sysinfo.getProperty("hasWinPackageId", false)
    ) {
      pinMessage = "default-browser-prompt-message-pin-msix";
    } else {
      pinMessage = "default-browser-prompt-message-pin";
    }
    let [promptTitle, promptMessage, askLabel, yesButton, notNowButton] = (
      await win.document.l10n.formatMessages([
        {
          id: needPin
            ? "default-browser-prompt-title-pin"
            : "default-browser-prompt-title-alt",
        },
        {
          id: needPin ? pinMessage : "default-browser-prompt-message-alt",
        },
        { id: "default-browser-prompt-checkbox-not-again-label" },
        {
          id: needPin
            ? "default-browser-prompt-button-primary-set"
            : "default-browser-prompt-button-primary-alt",
        },
        { id: "default-browser-prompt-button-secondary" },
      ])
    ).map(({ value }) => value);

    let ps = Services.prompt;
    let buttonFlags =
      ps.BUTTON_TITLE_IS_STRING * ps.BUTTON_POS_0 +
      ps.BUTTON_TITLE_IS_STRING * ps.BUTTON_POS_1 +
      ps.BUTTON_POS_0_DEFAULT;
    let rv = await ps.asyncConfirmEx(
      win.browsingContext,
      ps.MODAL_TYPE_INTERNAL_WINDOW,
      promptTitle,
      promptMessage,
      buttonFlags,
      yesButton,
      notNowButton,
      null,
      askLabel,
      false, 
      {
        headerIconCSSValue: lazy.CommonDialog.DEFAULT_APP_ICON_CSS,
      }
    );
    let buttonNumClicked = rv.get("buttonNumClicked");
    let checkboxState = rv.get("checked");
    if (buttonNumClicked == 0) {
      try {
        await shellService.pinToTaskbar();
      } catch (e) {
        this.log.error("Failed to pin to taskbar", e);
      }
      try {
        await shellService.pinToStartMenu();
      } catch (e) {
        this.log.error("Failed to pin to Start Menu", e);
      }
      try {
        await shellService.setAsDefault();
      } catch (e) {
        this.log.error("Failed to set the default browser", e);
      }
    }
    if (checkboxState) {
      shellService.shouldCheckDefaultBrowser = false;
      Services.prefs.setCharPref("browser.shell.userDisabledDefaultCheck", now);
    }

    try {
      let resultEnum = buttonNumClicked * 2 + !checkboxState;
    } catch (ex) {
    }
  },

  async willCheckDefaultBrowser(isStartupCheck) {
    let win = lazy.BrowserWindowTracker.getTopWindow({
      allowFromInactiveWorkspace: true,
    });
    let shellService = win.getShellService();

    if (!shellService) {
      return false;
    }

    let runningUnderFlatpak = false;
    if (Cc["@mozilla.org/gio-service;1"]) {
      let gIOSvc = Cc["@mozilla.org/gio-service;1"].getService(
        Ci.nsIGIOService
      );
      runningUnderFlatpak = gIOSvc.isRunningUnderFlatpak;
    }

    let shouldCheck =
      !AppConstants.DEBUG &&
      !runningUnderFlatpak &&
      shellService.shouldCheckDefaultBrowser;

    if (!shouldCheck && !isStartupCheck) {
      return false;
    }

    const skipDefaultBrowserCheck =
      Services.prefs.getBoolPref(
        "browser.shell.skipDefaultBrowserCheckOnFirstRun"
      ) &&
      !Services.prefs.getBoolPref(
        "browser.shell.didSkipDefaultBrowserCheckOnFirstRun"
      );

    let promptCount = Services.prefs.getIntPref(
      "browser.shell.defaultBrowserCheckCount",
      0
    );

    await lazy.SessionStartup.onceInitialized;
    let willRecoverSession =
      lazy.SessionStartup.sessionType == lazy.SessionStartup.RECOVER_SESSION;

    let isDefault = false;
    let isDefaultError = false;
    try {
      isDefault = shellService.isDefaultBrowser(isStartupCheck, false);
    } catch (ex) {
      isDefaultError = true;
    }

    if (isDefault && isStartupCheck) {
      let now = Math.floor(Date.now() / 1000).toString();
      Services.prefs.setCharPref(
        "browser.shell.mostRecentDateSetAsDefault",
        now
      );
    }

    let willPrompt = shouldCheck && !isDefault && !willRecoverSession;

    if (willPrompt) {
      if (skipDefaultBrowserCheck) {
        if (isStartupCheck) {
          Services.prefs.setBoolPref(
            "browser.shell.didSkipDefaultBrowserCheckOnFirstRun",
            true
          );
        }
        willPrompt = false;
      } else {
        promptCount++;
        if (isStartupCheck) {
          Services.prefs.setIntPref(
            "browser.shell.defaultBrowserCheckCount",
            promptCount
          );
        }
        if (!AppConstants.RELEASE_OR_BETA && promptCount > 3) {
          willPrompt = false;
        }
      }
    }

    if (isStartupCheck) {
      try {
      } catch (ex) {
      }
    }

    return willPrompt;
  },
};
