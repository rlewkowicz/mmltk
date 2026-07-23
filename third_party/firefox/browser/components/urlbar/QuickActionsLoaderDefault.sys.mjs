/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ActionsProviderQuickActions:
    "moz-src:///browser/components/urlbar/ActionsProviderQuickActions.sys.mjs",
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
});

let openUrlFun = url => (_queryContext, controller) =>
  openUrl(url, controller.browserWindow);
let openUrl = (url, window) => {
  if (url.startsWith("about:")) {
    window.switchToTabHavingURI(Services.io.newURI(url), true, {
      ignoreFragment: "whenComparing",
    });
  } else {
    window.gBrowser.addTab(url, {
      inBackground: false,
      triggeringPrincipal: Services.scriptSecurityManager.getSystemPrincipal(),
    });
  }
  return { focusContent: true };
};

let currentWindow = () => lazy.BrowserWindowTracker.getTopWindow();
let currentBrowser = () => currentWindow().gBrowser.selectedBrowser;

let unmutedAudioTabs = () =>
  lazy.BrowserWindowTracker.orderedWindows.flatMap(win =>
    Array.from(win.gBrowser.tabs).filter(
      tab =>
        (tab.soundPlaying ||
          tab.hasAttribute("soundplaying-scheduledremoval")) &&
        !tab.muted
    )
  );

ChromeUtils.defineLazyGetter(lazy, "gFluentStrings", function () {
  return new Localization(
    [
      "branding/brand.ftl",
      "browser/browser.ftl",
      "toolkit/branding/brandings.ftl",
    ],
    true
  );
});

const DEFAULT_ACTIONS = {
  bookmarks: {
    l10nCommands: ["quickactions-cmd-bookmarks", "quickactions-bookmarks2"],
    icon: "chrome://browser/skin/bookmark.svg",
    label: "quickactions-bookmarks2",
    onPick: (_queryContext, controller) => {
      controller.browserWindow.top.PlacesCommandHook.showPlacesOrganizer(
        "BookmarksToolbar"
      );
    },
  },
  clear: {
    l10nCommands: [
      "quickactions-cmd-clearrecenthistory2",
      "quickactions-clearrecenthistory",
    ],
    icon: "chrome://browser/skin/forget.svg",
    label: "quickactions-clearrecenthistory",
    onPick: (_queryContext, controller) => {
      controller.browserWindow.document
        .getElementById("Tools:Sanitize")
        .doCommand();
    },
  },
  downloads: {
    l10nCommands: ["quickactions-cmd-downloads"],
    icon: "chrome://browser/skin/downloads/downloads.svg",
    label: "quickactions-downloads2",
    onPick: openUrlFun("about:downloads"),
  },
  help: {
    l10nCommands: ["quickactions-cmd-help"],
    icon: "chrome://global/skin/icons/help.svg",
    label: "quickactions-help",
    onPick: openUrlFun("https://support.mozilla.org/products/firefox?as=u"),
  },
  library: {
    l10nCommands: ["quickactions-cmd-library"],
    icon: "chrome://browser/skin/library.svg",
    label: "quickactions-library",
    onPick: (_queryContext, controller) => {
      controller.browserWindow.top.PlacesCommandHook.showPlacesOrganizer();
    },
  },
  mute: {
    l10nCommands: ["quickactions-cmd-mute"],
    label: "quickactions-mute",
    icon: "chrome://global/skin/media/audio-muted.svg",
    isVisible: () => !!unmutedAudioTabs().length,
    onPick: () => {
      for (let tab of unmutedAudioTabs()) {
        tab.toggleMuteAudio();
      }
    },
  },
  private: {
    l10nCommands: ["quickactions-cmd-private"],
    label: "quickactions-private2",
    icon: "chrome://global/skin/icons/indicator-private-browsing.svg",
    onPick: (_queryContext, controller) => {
      controller.browserWindow.OpenBrowserWindow({ private: true });
    },
  },
  restart: {
    l10nCommands: ["quickactions-cmd-restart"],
    icon: "chrome://global/skin/icons/reload.svg",
    label: "quickactions-restart",
    onPick: restartBrowser,
  },
  settings: {
    l10nCommands: ["quickactions-cmd-settings2"],
    icon: "chrome://global/skin/icons/settings.svg",
    label: "quickactions-settings2",
    onPick: openUrlFun("about:preferences"),
  },
  viewsource: {
    l10nCommands: ["quickactions-cmd-viewsource2"],
    icon: "chrome://browser/skin/reader-mode.svg",
    label: "quickactions-viewsource2",
    isVisible: () => currentBrowser().currentURI.scheme !== "view-source",
    onPick: (_queryContext, controller) =>
      openUrl(
        "view-source:" + controller.browserWindow.gBrowser.currentURI.spec,
        controller.browserWindow
      ),
  },
};

function restartBrowser() {
  let cancelQuit = Cc["@mozilla.org/supports-PRBool;1"].createInstance(
    Ci.nsISupportsPRBool
  );
  Services.obs.notifyObservers(
    cancelQuit,
    "quit-application-requested",
    "restart"
  );
  if (cancelQuit.data) {
    return;
  }
  Services.startup.quit(
    Ci.nsIAppStartup.eAttemptQuit | Ci.nsIAppStartup.eRestart
  );
}

export class QuickActionsLoaderDefault {
  static #loadedPromise = null;

  static async load() {
    let keys = Object.keys(DEFAULT_ACTIONS);
    for (const key of keys) {
      let actionData = DEFAULT_ACTIONS[key];
      let messages = await lazy.gFluentStrings.formatMessages(
        actionData.l10nCommands.map(id => ({ id }))
      );
      actionData.commands = messages
        .map(({ value }) => value.split(",").map(x => x.trim().toLowerCase()))
        .flat();
      lazy.ActionsProviderQuickActions.addAction(key, actionData);
    }
  }
  static async ensureLoaded() {
    if (!this.#loadedPromise) {
      this.#loadedPromise = this.load();
    }
    await this.#loadedPromise;
  }
}
