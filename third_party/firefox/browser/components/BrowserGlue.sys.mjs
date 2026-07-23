/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  ContentBlockingPrefs:
    "moz-src:///browser/components/protections/ContentBlockingPrefs.sys.mjs",
  ContextualIdentityService:
    "moz-src:///toolkit/components/contextualidentity/ContextualIdentityService.sys.mjs",
  DesktopActorRegistry:
    "moz-src:///browser/components/DesktopActorRegistry.sys.mjs",
  DownloadsViewableInternally:
    "moz-src:///browser/components/downloads/DownloadsViewableInternally.sys.mjs",
  PlacesBrowserStartup:
    "moz-src:///browser/components/places/PlacesBrowserStartup.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  Sanitizer: "resource:///modules/Sanitizer.sys.mjs",
  SessionStartup: "resource:///modules/sessionstore/SessionStartup.sys.mjs",
  SessionWindowUI: "resource:///modules/sessionstore/SessionWindowUI.sys.mjs",
  ShortcutUtils: "resource://gre/modules/ShortcutUtils.sys.mjs",
});

const OBSERVE_LASTWINDOW_CLOSE_TOPICS = AppConstants.platform != "macosx";

export let BrowserInitState = {};
BrowserInitState.startupIdleTaskPromise = new Promise(resolve => {
  BrowserInitState._resolveStartupIdleTask = resolve;
});

export function BrowserGlue() {
  this._init();
}

BrowserGlue.prototype = {
  _saveSession: false,
  _defaultCookieBehaviorAtStartup: null,

  _setPrefToSaveSession: function BG__setPrefToSaveSession(aForce) {
    if (!this._saveSession && !aForce) {
      return;
    }

    if (!lazy.PrivateBrowsingUtils.permanentPrivateBrowsing) {
      Services.prefs.setBoolPref(
        "browser.sessionstore.resume_session_once",
        true
      );
    }

    Services.prefs.savePrefFile(null);
  },

  observe: async function BG_observe(subject, topic, data) {
    switch (topic) {
      case "final-ui-startup":
        this._beforeUIStartup();
        break;
      case "browser-delayed-startup-finished":
        this._onFirstWindowLoaded(subject);
        Services.obs.removeObserver(this, "browser-delayed-startup-finished");
        break;
      case "sessionstore-windows-restored":
        this._onWindowsRestored();
        break;
      case "browser:purge-session-history":
        Services.console.logStringMessage(null); 
        Services.console.reset();
        break;
      case "quit-application-requested":
        this._onQuitRequest(subject, data);
        break;
      case "quit-application-granted":
        this._onQuitApplicationGranted();
        break;
      case "browser-lastwindow-close-requested":
        if (OBSERVE_LASTWINDOW_CLOSE_TOPICS) {
          this._onQuitRequest(subject, "lastwindow");
        }
        break;
      case "browser-lastwindow-close-granted":
        if (OBSERVE_LASTWINDOW_CLOSE_TOPICS) {
          this._setPrefToSaveSession();
        }
        break;
      case "session-save":
        this._setPrefToSaveSession(true);
        subject.QueryInterface(Ci.nsISupportsPRBool);
        subject.data = true;
        break;
      case "places-init-complete":
        Services.obs.removeObserver(this, "places-init-complete");
        lazy.PlacesBrowserStartup.backendInitComplete();
        break;
      case "browser-glue-test": 
        if (data == "places-browser-init-complete") {
          lazy.PlacesBrowserStartup.notifyIfInitializationComplete();
        }
        break;
      case "handle-xul-text-link": {
        let linkHandled = subject.QueryInterface(Ci.nsISupportsPRBool);
        if (!linkHandled.data) {
          let win =
            lazy.BrowserWindowTracker.getTopWindow() ??
            (await lazy.BrowserWindowTracker.promiseOpenWindow());
          if (win) {
            data = JSON.parse(data);
            let where = lazy.BrowserUtils.whereToOpenLink(data);
            if (where == "current") {
              where = "tab";
            }
            win.openTrustedLinkIn(data.href, where);
            linkHandled.data = true;
          }
        }
        break;
      }
      case "profile-before-change":
        this._dispose();
        break;
      case "handlersvc-store-initialized":
        lazy.DownloadsViewableInternally.register();

        break;
      case "app-startup": {
        this._earlyBlankFirstPaint(subject);
        break;
      }
    }
  },

  _init: function BG__init() {
    let os = Services.obs;
    [
      "final-ui-startup",
      "browser-delayed-startup-finished",
      "sessionstore-windows-restored",
      "browser:purge-session-history",
      "quit-application-requested",
      "quit-application-granted",
      "session-save",
      "places-init-complete",
      "handle-xul-text-link",
      "profile-before-change",
      "handlersvc-store-initialized",
    ].forEach(topic => os.addObserver(this, topic, true));
    if (OBSERVE_LASTWINDOW_CLOSE_TOPICS) {
      os.addObserver(this, "browser-lastwindow-close-requested", true);
      os.addObserver(this, "browser-lastwindow-close-granted", true);
    }

    lazy.DesktopActorRegistry.init();
  },

  _dispose: function BG__dispose() {

    lazy.ContentBlockingPrefs.uninit();
  },

  _beforeUIStartup: function BG__beforeUIStartup() {
    lazy.SessionStartup.init();

    lazy.BrowserUtils.callModulesFromCategory({
      categoryName: "browser-before-ui-startup",
    });

    Services.obs.notifyObservers(null, "browser-ui-startup-complete");
  },


  _earlyBlankFirstPaint(cmdLine) {
    let shouldCreateWindow = isPrivateWindow => {
      if (cmdLine.findFlag("wait-for-jsdebugger", false) != -1) {
        return true;
      }

      if (
        AppConstants.platform == "macosx" ||
        Services.startup.wasSilentlyStarted ||
        !Services.prefs.getBoolPref("browser.startup.blankWindow", false)
      ) {
        return false;
      }

      if (
        Services.prefs.getBoolPref(
          "privacy.resistFingerprinting.skipEarlyBlankFirstPaint",
          true
        ) &&
        ChromeUtils.shouldResistFingerprinting(
          "RoundWindowSize",
          null,
          isPrivateWindow ||
            Services.prefs.getBoolPref(
              "browser.privatebrowsing.autostart",
              false
            )
        )
      ) {
        return false;
      }

      let width = getValue("width");
      let height = getValue("height");

      if (!width || !height) {
        return false;
      }

      return true;
    };

    let makeWindowPrivate =
      cmdLine.findFlag("private-window", false) != -1 &&
      lazy.PrivateBrowsingUtils.enabled;
    if (!shouldCreateWindow(makeWindowPrivate)) {
      return;
    }

    let browserWindowFeatures =
      "chrome,all,dialog=no,extrachrome,menubar,resizable,scrollbars,status," +
      "location,toolbar,personalbar";
    if (makeWindowPrivate) {
      browserWindowFeatures += ",private";
    }

    let win = Services.ww.openWindow(
      null,
      null,
      null,
      browserWindowFeatures,
      null
    );

    let hiddenTitlebar = Services.appinfo.drawInTitlebar;
    if (hiddenTitlebar) {
      win.windowUtils.setCustomTitlebar(true);
    }

    let docElt = win.document.documentElement;
    docElt.setAttribute("screenX", getValue("screenX"));
    docElt.setAttribute("screenY", getValue("screenY"));

    let appWin = win.docShell.treeOwner
      .QueryInterface(Ci.nsIInterfaceRequestor)
      .getInterface(Ci.nsIAppWindow);

    let sizemode = getValue("sizemode");
    let width = getValue("width") || 500;
    let height = getValue("height") || 500;
    if (sizemode == "maximized") {
      docElt.setAttribute("sizemode", sizemode);

      height -= appWin.outerToInnerHeightDifferenceInCSSPixels;
      width -= appWin.outerToInnerWidthDifferenceInCSSPixels;
      docElt.setAttribute("height", height);
      docElt.setAttribute("width", width);
    } else {
      win.resizeTo(width, height);
    }

    docElt.setAttribute("windowtype", "navigator:blank");

    appWin.showInitialViewer();

    function getValue(attr) {
      return Services.xulStore.getValue(
        AppConstants.BROWSER_CHROME_URL,
        "main-window",
        attr
      );
    }

  },

  _onFirstWindowLoaded: function BG__onFirstWindowLoaded(aWindow) {
    lazy.BrowserUtils.callModulesFromCategory(
      {
        categoryName: "browser-first-window-ready",
      },
      aWindow
    );
  },

  _onQuitApplicationGranted() {
    lazy.BrowserUtils.callModulesFromCategory({
      categoryName: "browser-quit-application-granted",
    });

    let tasks = [
      () => this._setPrefToSaveSession(),
    ];

    for (let task of tasks) {
      try {
        task();
      } catch (ex) {
        console.error(`Error during quit-application-granted: ${ex}`);
        failureHandler(ex);
      }
    }
  },

  _onWindowsRestored: function BG__onWindowsRestored() {
    if (this._windowsWereRestored) {
      return;
    }
    this._windowsWereRestored = true;

    lazy.Sanitizer.onStartup();
    lazy.SessionWindowUI.maybeShowRestoreSessionInfoBar();
    this._scheduleStartupIdleTasks();
  },

  _scheduleStartupIdleTasks() {
    function runIdleTasks(idleTasks) {
      for (let task of idleTasks) {
        if ("condition" in task && !task.condition) {
          continue;
        }

        ChromeUtils.idleDispatch(
          async () => {
            if (!Services.startup.shuttingDown) {
              try {
                await task.task();
              } catch (ex) {
                console.error(ex);
              }
            }
          },
          task.timeout ? { timeout: task.timeout } : undefined
        );
      }
    }

    const earlyTasks = [
      {
        name: "ContextualIdentityService.load",
        task: async () => {
          await lazy.ContextualIdentityService.load();
        },
      },
    ];

    runIdleTasks(earlyTasks);

    lazy.BrowserUtils.callModulesFromCategory({
      categoryName: "browser-idle-startup",
      idleDispatch: true,
    });

    const lateTasks = [
      {
        name: "handlerService.asyncInit",
        task: () => {
          let handlerService = Cc[
            "@mozilla.org/uriloader/handler-service;1"
          ].getService(Ci.nsIHandlerService);
          handlerService.asyncInit();
        },
      },

      {
        name: "unblock-untrusted-modules-thread",
        condition: AppConstants.platform == "win",
        task: () => {
          Services.obs.notifyObservers(
            null,
            "unblock-untrusted-modules-thread"
          );
        },
      },

      {
        name: "start-orb-javascript-oracle",
        task: () => {
          ChromeUtils.ensureJSOracleStarted();
        },
      },

      {
        name: "Init hasSSD for SystemInfo",
        condition: AppConstants.platform == "win",
        task: () => Services.sysinfo.diskInfo,
      },

      {
        name: "browser-startup-idle-tasks-finished",
        task: () => {
          ChromeUtils.idleDispatch(() => {
            Services.obs.notifyObservers(
              null,
              "browser-startup-idle-tasks-finished"
            );
            BrowserInitState._resolveStartupIdleTask();
          });
        },
      },
    ];

    runIdleTasks(lateTasks);
  },

  _quitSource: "unknown",
  _registerQuitSource(source) {
    this._quitSource = source;
  },

  _onQuitRequest: function BG__onQuitRequest(aCancelQuit, aQuitType) {
    if (aCancelQuit instanceof Ci.nsISupportsPRBool && aCancelQuit.data) {
      return;
    }


    if (aQuitType == "restart" || aQuitType == "os-restart") {
      return;
    }

    if (!Services.prefs.getBoolPref("browser.warnOnQuit")) {
      return;
    }

    let windowcount = 0;
    let pagecount = 0;
    for (let win of lazy.BrowserWindowTracker.orderedWindows) {
      if (win.closed) {
        continue;
      }
      windowcount++;
      let tabbrowser = win.gBrowser;
      if (tabbrowser) {
        pagecount += tabbrowser.visibleTabs.length - tabbrowser.pinnedTabCount;
      }
    }

    if (!windowcount) {
      return;
    }

    let shouldWarnForShortcut =
      this._quitSource == "shortcut" &&
      Services.prefs.getBoolPref("browser.warnOnQuitShortcut");
    let shouldWarnForTabs =
      pagecount >= 2 && Services.prefs.getBoolPref("browser.tabs.warnOnClose");
    if (!shouldWarnForTabs && !shouldWarnForShortcut) {
      return;
    }

    if (!aQuitType) {
      aQuitType = "quit";
    }

    let win = lazy.BrowserWindowTracker.getTopWindow({
      allowFromInactiveWorkspace: true,
    });

    win.gDialogBox.replaceDialogIfOpen();

    let titleId = {
      id: "tabbrowser-confirm-close-tabs-title",
      args: { tabCount: pagecount },
    };
    let quitButtonLabelId = "tabbrowser-confirm-close-tabs-button";
    let closeTabButtonLabelId = "tabbrowser-confirm-close-tab-only-button";

    let showCloseCurrentTabOption = false;
    if (windowcount > 1) {
      if (shouldWarnForShortcut) {
        showCloseCurrentTabOption = true;
        titleId = "tabbrowser-confirm-close-warn-shortcut-title";
        quitButtonLabelId =
          "tabbrowser-confirm-close-windows-warn-shortcut-button";
      } else {
        titleId = {
          id: "tabbrowser-confirm-close-windows-title",
          args: { windowCount: windowcount },
        };
        quitButtonLabelId = "tabbrowser-confirm-close-windows-button";
      }
    } else if (shouldWarnForShortcut) {
      if (win.gBrowser.visibleTabs.length > 1) {
        showCloseCurrentTabOption = true;
        titleId = "tabbrowser-confirm-close-warn-shortcut-title";
        quitButtonLabelId = "tabbrowser-confirm-close-tabs-with-key-button";
      } else {
        titleId = "tabbrowser-confirm-close-tabs-with-key-title";
        quitButtonLabelId = "tabbrowser-confirm-close-tabs-with-key-button";
      }
    }

    let checkboxLabelId;
    if (shouldWarnForShortcut) {
      const quitKeyElement = win.document.getElementById("key_quitApplication");
      const quitKey = lazy.ShortcutUtils.prettifyShortcut(quitKeyElement);
      checkboxLabelId = {
        id: "tabbrowser-ask-close-tabs-with-key-checkbox",
        args: { quitKey },
      };
    } else {
      checkboxLabelId = "tabbrowser-ask-close-tabs-checkbox";
    }

    const [title, quitButtonLabel, checkboxLabel] =
      win.gBrowser.tabLocalization.formatMessagesSync([
        titleId,
        quitButtonLabelId,
        checkboxLabelId,
      ]);

    let closeTabButtonLabel;
    if (showCloseCurrentTabOption) {
      [closeTabButtonLabel] = win.gBrowser.tabLocalization.formatMessagesSync([
        closeTabButtonLabelId,
      ]);
    }

    let warnOnClose = { value: true };

    let flags;
    if (showCloseCurrentTabOption) {
      flags =
        (Services.prompt.BUTTON_TITLE_IS_STRING * Services.prompt.BUTTON_POS_0 +
          Services.prompt.BUTTON_TITLE_CANCEL * Services.prompt.BUTTON_POS_1 +
          Services.prompt.BUTTON_TITLE_IS_STRING *
            Services.prompt.BUTTON_POS_2) |
        Services.prompt.BUTTON_POS_1_IS_SECONDARY;
      Services.prompt.BUTTON_TITLE_CANCEL * Services.prompt.BUTTON_POS_1;
    } else {
      flags =
        Services.prompt.BUTTON_TITLE_IS_STRING * Services.prompt.BUTTON_POS_0 +
        Services.prompt.BUTTON_TITLE_CANCEL * Services.prompt.BUTTON_POS_1;
    }

    let buttonPressed = Services.prompt.confirmEx(
      win,
      title.value,
      null,
      flags,
      quitButtonLabel.value,
      null,
      showCloseCurrentTabOption ? closeTabButtonLabel.value : null,
      checkboxLabel.value,
      warnOnClose
    );

    if (buttonPressed == 0 && !warnOnClose.value) {
      if (shouldWarnForShortcut) {
        Services.prefs.setBoolPref("browser.warnOnQuitShortcut", false);
      } else {
        Services.prefs.setBoolPref("browser.tabs.warnOnClose", false);
      }
    }

    if (buttonPressed === 2) {
      win.gBrowser.removeTab(win.gBrowser.selectedTab);
    }

    this._quitSource = "unknown";

    aCancelQuit.data = buttonPressed != 0;
  },

  QueryInterface: ChromeUtils.generateQI([
    "nsIObserver",
    "nsISupportsWeakReference",
  ]),
};
