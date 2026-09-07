/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};


XPCOMUtils.defineLazyServiceGetters(lazy, {
  BrowserHandler: ["@mozilla.org/browser/clh;1", Ci.nsIBrowserHandler],
});

ChromeUtils.defineESModuleGetters(lazy, {
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "gPreferWindowsOnCurrentVirtualDesktop",
  "widget.prefer_windows_on_current_virtual_desktop"
);

const TAB_EVENTS = ["TabBrowserInserted", "TabSelect"];
const WINDOW_EVENTS = ["activate", "unload"];
const DEBUG = false;

let _lastCurrentBrowserId = 0;
let _trackedWindows = [];

function debug(s) {
  if (DEBUG) {
    dump("-*- UpdateBrowserIDHelper: " + s + "\n");
  }
}

function _updateCurrentBrowserId(browser) {
  const topNonMinimized = _trackedWindows.find(
    w => !w.closed && w.windowState != w.STATE_MINIMIZED
  );
  if (
    !browser.browserId ||
    browser.browserId === _lastCurrentBrowserId ||
    browser.documentGlobal != topNonMinimized
  ) {
    return;
  }

  if (DEBUG) {
    debug(
      `Current window uri=${browser.currentURI?.spec} browser id=${browser.browserId}`
    );
  }

  _lastCurrentBrowserId = browser.browserId;
  let idWrapper = Cc["@mozilla.org/supports-PRUint64;1"].createInstance(
    Ci.nsISupportsPRUint64
  );
  idWrapper.data = _lastCurrentBrowserId;
  Services.obs.notifyObservers(idWrapper, "net:current-browser-id");
}

function _handleEvent(event) {
  switch (event.type) {
    case "TabBrowserInserted":
      if (
        event.target.documentGlobal.gBrowser.selectedBrowser ===
        event.target.linkedBrowser
      ) {
        _updateCurrentBrowserId(event.target.linkedBrowser);
      }
      break;
    case "TabSelect":
      _updateCurrentBrowserId(event.target.linkedBrowser);
      break;
    case "activate":
      WindowHelper.onActivate(event.target);
      break;
    case "unload":
      WindowHelper.removeWindow(event.currentTarget);
      break;
  }
}

function _trackWindowOrder(window) {
  _trackedWindows.unshift(window);
}

function _untrackWindowOrder(window) {
  let idx = _trackedWindows.indexOf(window);
  if (idx >= 0) {
    _trackedWindows.splice(idx, 1);
  }
}

function topicObserved(observeTopic, checkFn) {
  return new Promise((resolve, reject) => {
    function observer(subject, topic, data) {
      try {
        if (checkFn && !checkFn(subject, data)) {
          return;
        }
        Services.obs.removeObserver(observer, topic);
        checkFn = null;
        resolve([subject, data]);
      } catch (ex) {
        Services.obs.removeObserver(observer, topic);
        checkFn = null;
        reject(ex);
      }
    }
    Services.obs.addObserver(observer, observeTopic);
  });
}

var WindowHelper = {
  addWindow(window) {
    TAB_EVENTS.forEach(function (event) {
      window.gBrowser.tabContainer.addEventListener(event, _handleEvent);
    });
    WINDOW_EVENTS.forEach(function (event) {
      window.addEventListener(event, _handleEvent);
    });

    _trackWindowOrder(window);

    _updateCurrentBrowserId(window.gBrowser.selectedBrowser);
  },

  removeWindow(window) {
    _untrackWindowOrder(window);

    TAB_EVENTS.forEach(function (event) {
      window.gBrowser.tabContainer.removeEventListener(event, _handleEvent);
    });
    WINDOW_EVENTS.forEach(function (event) {
      window.removeEventListener(event, _handleEvent);
    });
  },

  onActivate(window) {
    if (window == _trackedWindows[0]) {
      return;
    }

    _untrackWindowOrder(window);
    _trackWindowOrder(window);

    _updateCurrentBrowserId(window.gBrowser.selectedBrowser);
  },
};

export const BrowserWindowTracker = {
  pendingWindows: new Map(),

  getTopWindow(options = {}) {
    let cloakedWin = null;
    let minimizedWin = null;
    for (let win of _trackedWindows) {
      if (
        !win.closed &&
        (options.allowPopups || win.toolbar.visible) &&
        (!("private" in options) ||
          lazy.PrivateBrowsingUtils.permanentPrivateBrowsing ||
          lazy.PrivateBrowsingUtils.isWindowPrivate(win) == options.private)
      ) {
        if (win.isCloaked && lazy.gPreferWindowsOnCurrentVirtualDesktop) {
          if (!cloakedWin && options.allowFromInactiveWorkspace) {
            cloakedWin = win;
          }
          continue;
        }
        if (win.windowState == win.STATE_MINIMIZED) {
          minimizedWin ??= win;
          continue;
        }
        return win;
      }
    }
    return minimizedWin || cloakedWin;
  },

  getPendingWindow(options = {}) {
    for (let pending of this.pendingWindows.values()) {
      if (
        !("private" in options) ||
        lazy.PrivateBrowsingUtils.permanentPrivateBrowsing ||
        pending.isPrivate == options.private
      ) {
        return pending.deferred.promise;
      }
    }
    return null;
  },

  registerOpeningWindow(window, isPrivate) {
    let deferred = Promise.withResolvers();

    this.pendingWindows.set(window, {
      isPrivate,
      deferred,
    });

    const topic = "browsing-context-discarded";
    const observer = aSubject => {
      if (window.browsingContext == aSubject) {
        let pending = this.pendingWindows.get(window);
        if (pending) {
          this.pendingWindows.delete(window);
          pending.deferred.resolve(window);
        }
        Services.obs.removeObserver(observer, topic);
      }
    };
    Services.obs.addObserver(observer, topic);
  },

  openWindow(options = {}) {
    let {
      openerWindow = undefined,
      private: isPrivate = false,
      features = undefined,
      all = true,
      args = null,
      remote = undefined,
      fission = undefined,
    } = options;

    let windowFeatures = "chrome,dialog=no";
    if (all) {
      windowFeatures += ",all";
    }
    if (features) {
      windowFeatures += `,${features}`;
    }
    let loadURIString;
    if (isPrivate && lazy.PrivateBrowsingUtils.enabled) {
      windowFeatures += ",private";
      if (!args && !lazy.PrivateBrowsingUtils.permanentPrivateBrowsing) {
        loadURIString = "about:privatebrowsing";
      }
    } else {
      windowFeatures += ",non-private";
    }
    if (!args) {
      loadURIString ??= lazy.BrowserHandler.defaultArgs;
      args = Cc["@mozilla.org/supports-string;1"].createInstance(
        Ci.nsISupportsString
      );
      args.data = loadURIString;
    }

    if (remote) {
      windowFeatures += ",remote";
    } else if (remote === false) {
      windowFeatures += ",non-remote";
    }

    if (fission) {
      windowFeatures += ",fission";
    } else if (fission === false) {
      windowFeatures += ",non-fission";
    }

    if (openerWindow?.windowState == openerWindow?.STATE_MAXIMIZED) {
      windowFeatures += ",suppressanimation";
    }

    let win = Services.ww.openWindow(
      openerWindow,
      AppConstants.BROWSER_CHROME_URL,
      "_blank",
      windowFeatures,
      args
    );
    this.registerOpeningWindow(win, isPrivate);

    return win;
  },

  async promiseOpenWindow(options) {
    let win = this.openWindow(options);
    await topicObserved(
      "browser-delayed-startup-finished",
      subject => subject == win
    );
    return win;
  },

  get windowCount() {
    return _trackedWindows.length;
  },

  get orderedWindows() {
    return this.getOrderedWindows();
  },

  getOrderedWindows({ private: isPrivate = undefined } = {}) {
    const nonMinimized = [];
    const minimized = [];
    for (const w of _trackedWindows) {
      if (w.windowState == w.STATE_MINIMIZED) {
        minimized.push(w);
      } else {
        nonMinimized.push(w);
      }
    }
    let windows = nonMinimized.concat(minimized);

    if (
      typeof isPrivate !== "boolean" ||
      (isPrivate && lazy.PrivateBrowsingUtils.permanentPrivateBrowsing)
    ) {
      return windows;
    }

    return windows.filter(
      w => lazy.PrivateBrowsingUtils.isWindowPrivate(w) === isPrivate
    );
  },

  getAllVisibleTabs() {
    let tabs = [];
    for (let win of BrowserWindowTracker.orderedWindows) {
      for (let tab of win.gBrowser.visibleTabs) {
        if (tab.linkedPanel) {
          let { contentTitle, browserId } = tab.linkedBrowser;
          tabs.push({ contentTitle, browserId });
        }
      }
    }
    return tabs;
  },

  track(window) {
    let pending = this.pendingWindows.get(window);
    if (pending) {
      this.pendingWindows.delete(window);
      window.delayedStartupPromise.then(() => pending.deferred.resolve(window));
    }

    return WindowHelper.addWindow(window);
  },

  getBrowserById(browserId) {
    for (let win of BrowserWindowTracker.orderedWindows) {
      for (let tab of win.gBrowser.visibleTabs) {
        if (tab.linkedPanel && tab.linkedBrowser.browserId === browserId) {
          return tab.linkedBrowser;
        }
      }
    }
    return null;
  },

  untrackForTestsOnly(window) {
    return WindowHelper.removeWindow(window);
  },
};
