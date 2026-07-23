/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  InteractionsBlocklist:
    "moz-src:///browser/components/places/InteractionsBlocklist.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  clearTimeout: "resource://gre/modules/Timer.sys.mjs",
  setTimeout: "resource://gre/modules/Timer.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "logConsole", function () {
  return console.createInstance({
    prefix: "InteractionsManager",
    maxLogLevel: Services.prefs.getBoolPref(
      "browser.places.interactions.log",
      false
    )
      ? "Debug"
      : "Warn",
  });
});

XPCOMUtils.defineLazyServiceGetters(lazy, {
  idleService: ["@mozilla.org/widget/useridleservice;1", Ci.nsIUserIdleService],
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "pageViewIdleTime",
  "browser.places.interactions.pageViewIdleTime",
  60
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "saveInterval",
  "browser.places.interactions.saveInterval",
  10000
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "isHistoryEnabled",
  "places.history.enabled",
  false
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "breakupIfNoUpdatesForSeconds",
  "browser.places.interactions.breakupIfNoUpdatesForSeconds",
  60 * 60
);

const DOMWINDOW_OPENED_TOPIC = "domwindowopened";
const RECENT_BROWSER_INTERACTION_EXPIRY_TIME_MS = 60000;

let gLastTime = 0;
function monotonicNow() {
  let time = Date.now();
  if (time == gLastTime) {
    time++;
  }
  return (gLastTime = time);
}



class _Interactions {
  DOCUMENT_TYPE = {
    GENERIC: 0,
    MEDIA: 1,
  };

  #interactions = new WeakMap();

  #activeWindow = undefined;

  #userIsIdle = false;

  _pageViewStartTime = ChromeUtils.now();

  #store = undefined;

  #initialized = false;

  #recentInteractions = new WeakMap();

  init() {
    if (
      !Services.prefs.getBoolPref("browser.places.interactions.enabled", false)
    ) {
      return;
    }

    ChromeUtils.registerWindowActor("Interactions", {
      parent: {
        esModuleURI: "resource:///actors/InteractionsParent.sys.mjs",
      },
      child: {
        esModuleURI: "resource:///actors/InteractionsChild.sys.mjs",
        events: {
          DOMContentLoaded: {},
          pagehide: { mozSystemGroup: true },
        },
      },
      messageManagerGroups: ["browsers"],
      safeForUntrustedWebProcess: true,
    });

    this.#activeWindow = Services.wm.getMostRecentBrowserWindow();

    for (let win of lazy.BrowserWindowTracker.orderedWindows) {
      if (!win.closed) {
        this.#registerWindow(win);
      }
    }
    Services.obs.addObserver(this, DOMWINDOW_OPENED_TOPIC, true);
    lazy.idleService.addIdleObserver(this, lazy.pageViewIdleTime);

    this.#initialized = true;
  }

  uninit() {
    if (this.#initialized) {
      lazy.idleService.removeIdleObserver(this, lazy.pageViewIdleTime);
    }
  }

  async reset() {
    lazy.logConsole.debug("Database reset");
    this.#interactions = new WeakMap();
    this.#userIsIdle = false;
    this._pageViewStartTime = ChromeUtils.now();
    ChromeUtils.consumeInteractionData();
    await _Interactions.interactionUpdatePromise;
    await this.store.reset();
  }

  get store() {
    if (!this.#store) {
      this.#store = new InteractionsStore();
    }
    return this.#store;
  }

  registerNewInteraction(browser, docInfo) {
    if (
      !browser ||
      !lazy.isHistoryEnabled ||
      !browser.browsingContext.useGlobalHistory
    ) {
      return;
    }
    let interaction = this.#interactions.get(browser);
    if (interaction && interaction.url != docInfo.url) {
      this.registerEndOfInteraction(browser);
    }

    if (lazy.InteractionsBlocklist.isUrlBlocklisted(docInfo.url)) {
      lazy.logConsole.debug(
        "Ignoring a page as the URL is blocklisted",
        docInfo
      );
      return;
    }

    lazy.logConsole.debug("Tracking a new interaction", docInfo);
    let now = monotonicNow();
    interaction = {
      url: docInfo.url,
      referrer: docInfo.referrer,
      totalViewTime: 0,
      typingTime: 0,
      keypresses: 0,
      scrollingTime: 0,
      scrollingDistance: 0,
      created_at: now,
      updated_at: now,
    };
    this.#interactions.set(browser, interaction);

    if (docInfo.isActive && browser.documentGlobal == this.#activeWindow) {
      this._pageViewStartTime = ChromeUtils.now();
    }

    this.#recentInteractions.set(browser, [
      ...(this.#recentInteractions.get(browser) ?? []),
      interaction,
    ]);

    this.#pruneOldRecentInteractions(browser);
  }

  registerEndOfInteraction(browser) {
    if (
      !browser ||
      !lazy.isHistoryEnabled ||
      !browser.browsingContext.useGlobalHistory
    ) {
      return;
    }
    lazy.logConsole.debug("Saw the end of an interaction");

    this.#updateInteraction(browser);
    this.#interactions.delete(browser);
  }

  #updateInteraction(browser = undefined) {
    _Interactions.#updateInteraction_async(
      browser,
      this.#activeWindow,
      this.#userIsIdle,
      this.#interactions,
      this._pageViewStartTime,
      this.store
    );
  }

  async getRecentInteractionsForBrowser(browser) {
    this.#updateInteraction();
    await _Interactions.interactionUpdatePromise;
    return this.#recentInteractions.get(browser);
  }

  #pruneOldRecentInteractions(browser) {
    const now = Date.now();

    const interactions = this.#recentInteractions.get(browser);
    if (!interactions) {
      return;
    }

    const interactionstoTrack = interactions.filter(
      interaction =>
        now - interaction.updated_at <=
        RECENT_BROWSER_INTERACTION_EXPIRY_TIME_MS
    );

    if (interactionstoTrack.length) {
      this.#recentInteractions.set(browser, interactionstoTrack);
    } else {
      this.#recentInteractions.delete(browser);
    }
  }

  static interactionUpdatePromise = Promise.resolve();

  get interactionUpdatePromise() {
    return _Interactions.interactionUpdatePromise;
  }

  static async #updateInteraction_async(
    browser,
    activeWindow,
    userIsIdle,
    interactions,
    pageViewStartTime,
    store
  ) {
    if (!activeWindow || (browser && browser.documentGlobal != activeWindow)) {
      lazy.logConsole.debug(
        "Not updating interaction as there is no active window"
      );
      return;
    }

    if (userIsIdle) {
      lazy.logConsole.debug("Not updating interaction as the user is idle");
      return;
    }

    if (!browser) {
      browser = activeWindow.gBrowser.selectedTab.linkedBrowser;
    }

    let interaction = interactions.get(browser);
    if (!interaction) {
      lazy.logConsole.debug("No interaction to update");
      return;
    }

    interaction.totalViewTime += ChromeUtils.now() - pageViewStartTime;
    Interactions._pageViewStartTime = ChromeUtils.now();

    const interactionData = ChromeUtils.consumeInteractionData();
    const typing = interactionData.Typing;
    if (typing) {
      interaction.typingTime += typing.interactionTimeInMilliseconds;
      interaction.keypresses += typing.interactionCount;
    }

    _Interactions.interactionUpdatePromise =
      _Interactions.interactionUpdatePromise
        .then(async () => ChromeUtils.collectScrollingData())
        .then(
          result => {
            interaction.scrollingTime += result.interactionTimeInMilliseconds;
            interaction.scrollingDistance += result.scrollingDistanceInPixels;
          },
          reason => {
            console.error(reason);
          }
        )
        .then(() => {
          interaction.updated_at = monotonicNow();

          lazy.logConsole.debug("Add to store: ", interaction);
          store.add(interaction);
        });
  }

  #onActivateWindow(win) {
    lazy.logConsole.debug("Window activated");

    if (lazy.PrivateBrowsingUtils.isWindowPrivate(win)) {
      return;
    }

    this.#activeWindow = win;
    this._pageViewStartTime = ChromeUtils.now();
  }

  #onDeactivateWindow() {
    lazy.logConsole.debug("Window deactivate");

    this.#updateInteraction();
    this.#activeWindow = undefined;
  }

  #onTabSelect(previousBrowser) {
    lazy.logConsole.debug("Tab switched");

    this.#updateInteraction(previousBrowser);

    this._pageViewStartTime = ChromeUtils.now();

    let browser = this.#activeWindow?.gBrowser.selectedBrowser;
    if (browser && this.#interactions.has(browser)) {
      let interaction = this.#interactions.get(browser);
      let timePassedSinceUpdateSeconds =
        (Date.now() - interaction.updated_at) / 1000;
      if (timePassedSinceUpdateSeconds >= lazy.breakupIfNoUpdatesForSeconds) {
        this.registerEndOfInteraction(browser);
        this.registerNewInteraction(browser, {
          url: browser.currentURI.spec,
          referrer: null,
          isActive: true,
        });
      }
    }
  }

  handleEvent(event) {
    switch (event.type) {
      case "TabSelect":
        this.#onTabSelect(event.detail.previousTab.linkedBrowser);
        break;
      case "activate":
        this.#onActivateWindow(event.target);
        break;
      case "deactivate":
        this.#onDeactivateWindow(event.target);
        break;
      case "unload":
        this.#unregisterWindow(event.target);
        break;
    }
  }

  observe(subject, topic) {
    switch (topic) {
      case DOMWINDOW_OPENED_TOPIC:
        this.#onWindowOpen(subject);
        break;
      case "idle":
        lazy.logConsole.debug("User went idle");
        this.#updateInteraction();
        this.#userIsIdle = true;
        break;
      case "active":
        lazy.logConsole.debug("User became active");
        this.#userIsIdle = false;
        this._pageViewStartTime = ChromeUtils.now();
        break;
    }
  }

  #registerWindow(win) {
    if (lazy.PrivateBrowsingUtils.isWindowPrivate(win)) {
      return;
    }

    win.addEventListener("TabSelect", this, true);
    win.addEventListener("deactivate", this, true);
    win.addEventListener("activate", this, true);
  }

  #unregisterWindow(win) {
    win.removeEventListener("TabSelect", this, true);
    win.removeEventListener("deactivate", this, true);
    win.removeEventListener("activate", this, true);
  }

  #onWindowOpen(win) {
    win.addEventListener(
      "load",
      () => {
        if (
          win.document.documentElement.getAttribute("windowtype") !=
          "navigator:browser"
        ) {
          return;
        }
        this.#registerWindow(win);
      },
      { once: true }
    );
  }

  QueryInterface = ChromeUtils.generateQI([
    "nsIObserver",
    "nsISupportsWeakReference",
  ]);
}

export const Interactions = new _Interactions();

class InteractionsStore {
  #timer = undefined;
  #interactions = new Map();
  #timerResolve = undefined;

  constructor() {
    this.progress = {};
    lazy.PlacesUtils.history.shutdownClient.jsclient.addBlocker(
      "Interactions.sys.mjs:: store",
      async () => this.flush(),
      { fetchState: () => this.progress }
    );

    this.pendingPromise = Promise.resolve();
  }

  async flush() {
    if (this.#timer) {
      lazy.clearTimeout(this.#timer);
      this.#timerResolve();
      await this.#updateDatabase();
    }
  }

  async reset() {
    await lazy.PlacesUtils.withConnectionWrapper(
      "Interactions.sys.mjs::reset",
      async db => {
        await db.executeCached(`DELETE FROM moz_places_metadata`);
      }
    );
    if (this.#timer) {
      lazy.clearTimeout(this.#timer);
      this.#timer = undefined;
      this.#timerResolve();
      this.#interactions.clear();
    }
  }

  add(interaction) {
    lazy.logConsole.debug("Preparing interaction for storage", interaction);

    let interactionsForUrl = this.#interactions.get(interaction.url);
    if (!interactionsForUrl) {
      interactionsForUrl = new Map();
      this.#interactions.set(interaction.url, interactionsForUrl);
    }
    interactionsForUrl.set(interaction.created_at, interaction);

    if (!this.#timer) {
      let promise = new Promise(resolve => {
        this.#timerResolve = resolve;
        this.#timer = lazy.setTimeout(() => {
          this.#updateDatabase().catch(console.error).then(resolve);
        }, lazy.saveInterval);
      });
      this.pendingPromise = this.pendingPromise.then(() => promise);
    }
  }

  async #updateDatabase() {
    this.#timer = undefined;

    let interactions = this.#interactions;
    if (!interactions.size) {
      return;
    }
    this.#interactions = new Map();

    let params = {};
    let SQLInsertFragments = [];
    let i = 0;
    for (let interactionsForUrl of interactions.values()) {
      for (let interaction of interactionsForUrl.values()) {
        params[`url${i}`] = interaction.url;
        params[`referrer${i}`] = interaction.referrer;
        params[`created_at${i}`] = interaction.created_at;
        params[`updated_at${i}`] = interaction.updated_at;
        params[`document_type${i}`] =
          interaction.documentType ?? Interactions.DOCUMENT_TYPE.GENERIC;
        params[`total_view_time${i}`] =
          Math.round(interaction.totalViewTime) || 0;
        params[`typing_time${i}`] = Math.round(interaction.typingTime) || 0;
        params[`key_presses${i}`] = interaction.keypresses || 0;
        params[`scrolling_time${i}`] =
          Math.round(interaction.scrollingTime) || 0;
        params[`scrolling_distance${i}`] =
          Math.round(interaction.scrollingDistance) || 0;
        SQLInsertFragments.push(`(
          (SELECT id FROM moz_places_metadata
            WHERE place_id = (SELECT id FROM moz_places WHERE url_hash = hash(:url${i}) AND url = :url${i})
              AND created_at = :created_at${i}),
          (SELECT id FROM moz_places WHERE url_hash = hash(:url${i}) AND url = :url${i}),
          (SELECT id FROM moz_places WHERE url_hash = hash(:referrer${i}) AND url = :referrer${i} AND :referrer${i} != :url${i}),
          :created_at${i},
          :updated_at${i},
          :document_type${i},
          :total_view_time${i},
          :typing_time${i},
          :key_presses${i},
          :scrolling_time${i},
          :scrolling_distance${i}
        )`);
        i++;
      }
    }

    lazy.logConsole.debug(`Storing ${i} entries in the database`);

    this.progress.pendingUpdates = i;
    await lazy.PlacesUtils.withConnectionWrapper(
      "Interactions.sys.mjs::updateDatabase",
      async db => {
        await db.executeCached(
          `
          WITH inserts (id, place_id, referrer_place_id, created_at, updated_at, document_type, total_view_time, typing_time, key_presses, scrolling_time, scrolling_distance) AS (
            VALUES ${SQLInsertFragments.join(", ")}
          )
          INSERT OR REPLACE INTO moz_places_metadata (
            id, place_id, referrer_place_id, created_at, updated_at, document_type, total_view_time, typing_time, key_presses, scrolling_time, scrolling_distance
          ) SELECT * FROM inserts WHERE place_id NOT NULL;
        `,
          params
        );
      }
    );
    this.progress.pendingUpdates = 0;

    Services.obs.notifyObservers(null, "places-metadata-updated");
  }
}
