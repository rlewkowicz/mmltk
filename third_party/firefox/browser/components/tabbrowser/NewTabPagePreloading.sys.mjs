/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AboutNewTab: "resource:///modules/AboutNewTab.sys.mjs",
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

export let NewTabPagePreloading = {
  MAX_COUNT: 3,

  browserCounts: {
    normal: 0,
    private: 0,
  },

  get enabled() {
    return (
      this.prefEnabled &&
      this.newTabEnabled &&
      !lazy.AboutNewTab.newTabURLOverridden
    );
  },

  _createBrowser(win) {
    const { gBrowser, BROWSER_NEW_TAB_URL } = win;

    let remoteType = ChromeUtils.predictRemoteTypeForURI(BROWSER_NEW_TAB_URL, {
      window: win,
    });
    let browser = gBrowser.createBrowser({
      isPreloadBrowser: true,
      remoteType,
    });
    gBrowser.preloadedBrowser = browser;

    let panel = gBrowser.getPanel(browser);
    gBrowser.tabpanels.appendChild(panel);

    return browser;
  },

  _adoptBrowserFromOtherWindow(window) {
    let winPrivate = lazy.PrivateBrowsingUtils.isWindowPrivate(window);
    let oldWin = lazy.BrowserWindowTracker.orderedWindows
      .filter(w => {
        return (
          winPrivate == lazy.PrivateBrowsingUtils.isWindowPrivate(w) &&
          w.gBrowser &&
          w.gBrowser.preloadedBrowser
        );
      })
      .pop();
    if (!oldWin) {
      return null;
    }
    let oldBrowser = oldWin.gBrowser.preloadedBrowser;
    oldWin.gBrowser.preloadedBrowser = null;

    let newBrowser = this._createBrowser(window);

    oldBrowser.swapBrowsers(newBrowser);

    newBrowser.permanentKey = oldBrowser.permanentKey;

    oldWin.gBrowser.getPanel(oldBrowser).remove();
    return newBrowser;
  },

  maybeCreatePreloadedBrowser(window) {
    if (
      !this.enabled ||
      window.gBrowser.preloadedBrowser ||
      !window.toolbar.visible ||
      window.document.hidden
    ) {
      return;
    }

    let windowPrivate = lazy.PrivateBrowsingUtils.isWindowPrivate(window);
    let countKey = windowPrivate ? "private" : "normal";
    let topWindows = lazy.BrowserWindowTracker.orderedWindows.filter(
      w => lazy.PrivateBrowsingUtils.isWindowPrivate(w) == windowPrivate
    );
    if (topWindows.indexOf(window) >= this.MAX_COUNT) {
      return;
    }

    if (this.browserCounts[countKey] >= this.MAX_COUNT) {
      let browser = this._adoptBrowserFromOtherWindow(window);
      if (browser) {
        return;
      }
    }

    let browser = this._createBrowser(window);
    let tabURI = Services.io.newURI(window.BROWSER_NEW_TAB_URL);
    browser.loadURI(tabURI, {
      triggeringPrincipal: Services.scriptSecurityManager.getSystemPrincipal(),
    });
    browser.docShellIsActive = false;

    window.gURLBar.getBrowserState(browser).urlbarFocused = true;

    window.FullZoom.onLocationChange(tabURI, false, browser);

    this.browserCounts[countKey]++;
  },

  getPreloadedBrowser(window) {
    if (!this.enabled) {
      return null;
    }

    let browser = window.gBrowser.preloadedBrowser;

    window.gBrowser.preloadedBrowser = null;

    if (browser) {
      let countKey = lazy.PrivateBrowsingUtils.isWindowPrivate(window)
        ? "private"
        : "normal";
      this.browserCounts[countKey]--;
      browser.removeAttribute("preloadedState");
    }

    return browser;
  },

  removePreloadedBrowser(window) {
    let browser = this.getPreloadedBrowser(window);
    if (browser) {
      window.gBrowser.getPanel(browser).remove();
    }
  },
};

XPCOMUtils.defineLazyPreferenceGetter(
  NewTabPagePreloading,
  "prefEnabled",
  "browser.newtab.preload",
  true
);
XPCOMUtils.defineLazyPreferenceGetter(
  NewTabPagePreloading,
  "newTabEnabled",
  "browser.newtabpage.enabled",
  true
);
