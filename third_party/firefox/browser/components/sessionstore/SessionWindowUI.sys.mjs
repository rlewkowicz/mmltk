/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  SessionStore: "resource:///modules/sessionstore/SessionStore.sys.mjs",
});

export var SessionWindowUI = {
  restoreLastClosedTabOrWindowOrSession(window) {
    let lastActionTaken = lazy.SessionStore.popLastClosedAction();
    if (lastActionTaken) {
      switch (lastActionTaken.type) {
        case lazy.SessionStore.LAST_ACTION_CLOSED_TAB:
          {
            const sourceWindow = lazy.SessionStore.getWindowForTabClosedId(
              lastActionTaken.closedId
            );
            this.undoCloseTab(window, undefined, sourceWindow?.__SSi);
          }
          break;
        case lazy.SessionStore.LAST_ACTION_CLOSED_WINDOW: {
          this.undoCloseWindow();
          break;
        }
      }
    } else {
      let closedTabCount = lazy.SessionStore.getLastClosedTabCount(window);
      if (lazy.SessionStore.canRestoreLastSession) {
        lazy.SessionStore.restoreLastSession();
      } else if (closedTabCount) {
        this.undoCloseTab(window);
      }
    }
  },

  undoCloseTab(window, aIndex, sourceWindowSSId) {
    let targetWindow = window;
    let sourceWindow;
    if (sourceWindowSSId) {
      sourceWindow = lazy.SessionStore.getWindowById(sourceWindowSSId);
      if (!sourceWindow) {
        throw new Error(
          "sourceWindowSSId argument to undoCloseTab didn't resolve to a window"
        );
      }
    } else {
      sourceWindow = window;
    }

    let blankTabToRemove = null;
    if (
      targetWindow.gBrowser.visibleTabs.length == 1 &&
      targetWindow.gBrowser.selectedTab.isEmpty
    ) {
      blankTabToRemove = targetWindow.gBrowser.selectedTab;
    }

    let tabsRemoved = false;
    let tab = null;
    const lastClosedTabGroupId =
      lazy.SessionStore.getLastClosedTabGroupId(sourceWindow);
    if (aIndex === undefined && lastClosedTabGroupId) {
      let group;
      if (lazy.SessionStore.getSavedTabGroup(lastClosedTabGroupId)) {
        group = lazy.SessionStore.openSavedTabGroup(
          lastClosedTabGroupId,
          targetWindow
        );
      } else {
        group = lazy.SessionStore.undoCloseTabGroup(
          window,
          lastClosedTabGroupId,
          targetWindow
        );
      }
      tabsRemoved = true;
      tab = group.tabs.at(-1);
    } else {
      let lastClosedTabCount =
        lazy.SessionStore.getLastClosedTabCount(sourceWindow);
      let tabsToRemove =
        aIndex !== undefined ? [aIndex] : new Array(lastClosedTabCount).fill(0);
      for (let index of tabsToRemove) {
        if (
          lazy.SessionStore.getClosedTabCountForWindow(sourceWindow) > index
        ) {
          tab = lazy.SessionStore.undoCloseTab(
            sourceWindow,
            index,
            targetWindow
          );
          tabsRemoved = true;
        }
      }
    }

    if (tabsRemoved && blankTabToRemove) {
      targetWindow.gBrowser.removeTab(blankTabToRemove);
    }

    return tab;
  },

  undoCloseWindow(aIndex) {
    let restoredWindow = null;
    if (lazy.SessionStore.getClosedWindowCount() > (aIndex || 0)) {
      restoredWindow = lazy.SessionStore.undoCloseWindow(aIndex || 0);
    }

    return restoredWindow;
  },

  async maybeShowRestoreSessionInfoBar() {
    let win = lazy.BrowserWindowTracker.getTopWindow({
      allowFromInactiveWorkspace: true,
    });
    let count = Services.prefs.getIntPref(
      "browser.startup.couldRestoreSession.count",
      0
    );
    if (count < 0 || count >= 2) {
      return;
    }
    if (count == 0) {
      Services.prefs.setIntPref(
        "browser.startup.couldRestoreSession.count",
        ++count
      );
      return;
    }

    if (
      !lazy.SessionStore.canRestoreLastSession ||
      lazy.PrivateBrowsingUtils.isWindowPrivate(win)
    ) {
      return;
    }

    Services.prefs.setIntPref(
      "browser.startup.couldRestoreSession.count",
      ++count
    );

    const messageFragment = win.document.createDocumentFragment();
    const message = win.document.createElement("span");
    const icon = win.document.createElement("img");
    icon.src = "chrome://browser/skin/menu.svg";
    icon.setAttribute("data-l10n-name", "icon");
    icon.className = "inline-icon";
    message.appendChild(icon);
    messageFragment.appendChild(message);
    win.document.l10n.setAttributes(
      message,
      "restore-session-startup-suggestion-message"
    );

    const buttons = [
      {
        "l10n-id": "restore-session-startup-suggestion-button",
        primary: true,
        callback: () => {
          win.PanelUI.selectAndMarkItem([
            "appMenu-history-button",
            "appMenu-restoreSession",
          ]);
        },
      },
    ];

    const notifyBox = win.gBrowser.getNotificationBox();
    const notification = await notifyBox.appendNotification(
      "startup-restore-session-suggestion",
      {
        label: messageFragment,
        priority: notifyBox.PRIORITY_INFO_MEDIUM,
      },
      buttons
    );
    notification.timeout = Date.now() + 3000;
  },
};

export class RestoreLastSessionObserver {
  constructor(window) {
    this._window = window;
    this._window.addEventListener("unload", this);
    this._observersAdded = false;
  }

  init() {
    if (
      lazy.SessionStore.canRestoreLastSession &&
      !lazy.PrivateBrowsingUtils.isWindowPrivate(this._window)
    ) {
      Services.obs.addObserver(this, "sessionstore-last-session-cleared", true);
      Services.obs.addObserver(
        this,
        "sessionstore-last-session-re-enable",
        true
      );
      this._observersAdded = true;
      this._window.goSetCommandEnabled("Browser:RestoreLastSession", true);
    } else if (lazy.SessionStore.willAutoRestore) {
      this._window.document.getElementById(
        "Browser:RestoreLastSession"
      ).hidden = true;
    }
  }

  uninit() {
    if (this._window) {
      if (this._observersAdded) {
        Services.obs.removeObserver(this, "sessionstore-last-session-cleared");
        Services.obs.removeObserver(
          this,
          "sessionstore-last-session-re-enable"
        );
        this._observersAdded = false;
      }

      this._window.removeEventListener("unload", this);
      this._window = null;
    }
  }

  handleEvent(event) {
    if (event.type === "unload") {
      this.uninit();
    }
  }

  observe(aSubject, aTopic) {
    if (!this._window) {
      return;
    }

    switch (aTopic) {
      case "sessionstore-last-session-cleared":
        this._window.goSetCommandEnabled("Browser:RestoreLastSession", false);
        break;
      case "sessionstore-last-session-re-enable":
        this._window.goSetCommandEnabled("Browser:RestoreLastSession", true);
        break;
    }
  }

  QueryInterface = ChromeUtils.generateQI([
    "nsIObserver",
    "nsISupportsWeakReference",
  ]);
}
