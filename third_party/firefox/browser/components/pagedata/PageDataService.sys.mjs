/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

import { EventEmitter } from "resource://gre/modules/EventEmitter.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  HiddenBrowserManager: "resource://gre/modules/HiddenFrame.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "logConsole", function () {
  return console.createInstance({
    prefix: "PageData",
    maxLogLevel: Services.prefs.getBoolPref("browser.pagedata.log", false)
      ? "Debug"
      : "Warn",
  });
});

XPCOMUtils.defineLazyServiceGetters(lazy, {
  idleService: ["@mozilla.org/widget/useridleservice;1", Ci.nsIUserIdleService],
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "fetchIdleTime",
  "browser.pagedata.fetchIdleTime",
  300
);

const ALLOWED_PROTOCOLS = new Set(["http:", "https:", "data:", "blob:"]);

function shift(set) {
  let iter = set.values();
  let { value, done } = iter.next();

  if (done) {
    return undefined;
  }

  set.delete(value);
  return value;
}


class PageDataCache {
  #cache = new Map();

  set(url, pageData) {
    let entry = this.#cache.get(url);

    if (entry) {
      entry.pageData = pageData;
    }
  }

  get(url) {
    let entry = this.#cache.get(url);
    return entry?.pageData ?? null;
  }

  lockData(actor, url) {
    let entry = this.#cache.get(url);
    if (entry) {
      entry.actors.add(actor);
    } else {
      this.#cache.set(url, {
        pageData: undefined,
        actors: new Set([actor]),
      });
    }
  }

  unlockData(actor, url) {
    let entries = [];
    if (url) {
      let entry = this.#cache.get(url);
      if (!entry) {
        return;
      }

      entries.push([url, entry]);
    } else {
      entries = [...this.#cache];
    }

    for (let [entryUrl, entry] of entries) {
      if (entry.actors.delete(actor)) {
        if (entry.actors.size == 0) {
          this.#cache.delete(entryUrl);
        }
      }
    }
  }
}


export const PageDataService = new (class PageDataService extends EventEmitter {
  #pageDataCache = new PageDataCache();

  #backgroundFetches = 0;

  #backgroundQueue = new Set();

  #userIsIdle = false;

  #backgroundBrowsers = new WeakMap();

  #trackedWindows = new Map();

  constructor() {
    super();

    XPCOMUtils.defineLazyPreferenceGetter(
      this,
      "MAX_BACKGROUND_FETCHES",
      "browser.pagedata.maxBackgroundFetches",
      5,
      () => this.#startBackgroundWorkers()
    );
  }

  init() {
    if (!Services.prefs.getBoolPref("browser.pagedata.enabled", false)) {
      return;
    }

    ChromeUtils.registerWindowActor("PageData", {
      parent: {
        esModuleURI: "resource:///actors/PageDataParent.sys.mjs",
      },
      child: {
        esModuleURI: "resource:///actors/PageDataChild.sys.mjs",
        events: {
          DOMContentLoaded: {},
          pageshow: {},
        },
      },
      safeForUntrustedWebProcess: true,
    });

    lazy.logConsole.debug("Service started");

    for (let win of lazy.BrowserWindowTracker.orderedWindows) {
      if (!win.closed) {
        for (let tab of win.gBrowser.tabs) {
          let parent =
            tab.linkedBrowser.browsingContext?.currentWindowGlobal.getActor(
              "PageData"
            );

          parent.sendAsyncMessage("PageData:CheckLoaded");
        }
      }
    }

    lazy.idleService.addIdleObserver(this, lazy.fetchIdleTime);
  }

  uninit() {
    lazy.logConsole.debug("Service stopped");
  }

  #trackBrowser(browser) {
    let window = browser.documentGlobal;

    let browsers = this.#trackedWindows.get(window);
    if (browsers) {
      browsers.add(browser);

      return;
    }

    browsers = new Set([browser]);
    this.#trackedWindows.set(window, browsers);

    window.addEventListener("unload", () => {
      for (let closedBrowser of browsers) {
        this.unlockEntry(closedBrowser);
      }

      this.#trackedWindows.delete(window);
    });

    window.addEventListener("TabClose", ({ target: tab }) => {
      let closedBrowser = tab.linkedBrowser;
      this.unlockEntry(closedBrowser);
      browsers.delete(closedBrowser);
    });
  }

  lockEntry(actor, url) {
    this.#pageDataCache.lockData(actor, url);
  }

  unlockEntry(actor, url) {
    this.#pageDataCache.unlockData(actor, url);
  }

  async pageLoaded(actor, url) {
    if (!ALLOWED_PROTOCOLS.has(new URL(url).protocol)) {
      return;
    }

    let browser = actor.browsingContext?.embedderElement;

    if (!browser) {
      return;
    }

    let backgroundResolve = this.#backgroundBrowsers.get(browser);
    if (backgroundResolve) {
      backgroundResolve(actor);
      return;
    }

    if (!this.#isATabBrowser(browser)) {
      return;
    }

    try {
      let data = await actor.collectPageData();
      if (data) {
        this.#trackBrowser(browser);
        this.lockEntry(browser, data.url);

        this.pageDataDiscovered(data);
      }
    } catch (e) {
      lazy.logConsole.error(e);
    }
  }

  pageDataDiscovered(pageData) {
    lazy.logConsole.debug("Discovered page data", pageData);

    this.#pageDataCache.set(pageData.url, {
      ...pageData,
      data: pageData.data ?? {},
    });

    this.emit("page-data", pageData);
  }

  getCached(url) {
    return this.#pageDataCache.get(url);
  }

  async fetchPageData(url) {
    return lazy.HiddenBrowserManager.withHiddenBrowser(async browser => {
      try {
        let { promise, resolve } = Promise.withResolvers();
        this.#backgroundBrowsers.set(browser, resolve);

        let principal = Services.scriptSecurityManager.getSystemPrincipal();
        let loadURIOptions = {
          triggeringPrincipal: principal,
        };
        browser.fixupAndLoadURIString(url, loadURIOptions);

        let actor = await promise;
        return await actor.collectPageData();
      } finally {
        this.#backgroundBrowsers.delete(browser);
      }
    });
  }

  observe(subject, topic) {
    switch (topic) {
      case "idle":
        lazy.logConsole.debug("User went idle");
        this.#userIsIdle = true;
        this.#startBackgroundWorkers();
        break;
      case "active":
        lazy.logConsole.debug("User became active");
        this.#userIsIdle = false;
        break;
    }
  }

  #startBackgroundWorkers() {
    if (!this.#userIsIdle) {
      return;
    }

    let toStart;

    if (this.MAX_BACKGROUND_FETCHES) {
      toStart = this.MAX_BACKGROUND_FETCHES - this.#backgroundFetches;
    } else {
      toStart = this.#backgroundQueue.size;
    }

    for (let i = 0; i < toStart; i++) {
      this.#backgroundFetch();
    }
  }

  async #backgroundFetch() {
    this.#backgroundFetches++;

    let url = shift(this.#backgroundQueue);
    while (url) {
      try {
        let pageData = await this.fetchPageData(url);

        if (pageData) {
          this.#pageDataCache.set(url, pageData);
          this.emit("page-data", pageData);
        }
      } catch (e) {
        lazy.logConsole.error(e);
      }

      if (
        !this.#userIsIdle ||
        (this.MAX_BACKGROUND_FETCHES > 0 &&
          this.#backgroundFetches > this.MAX_BACKGROUND_FETCHES)
      ) {
        break;
      }

      url = shift(this.#backgroundQueue);
    }

    this.#backgroundFetches--;
  }

  queueFetch(url) {
    this.#backgroundQueue.add(url);

    this.#startBackgroundWorkers();
  }

  #isATabBrowser(browser) {
    return browser.documentGlobal.gBrowser?.getTabForBrowser(browser);
  }
})();
