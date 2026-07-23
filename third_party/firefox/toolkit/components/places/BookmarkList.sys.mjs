/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  DeferredTask: "resource://gre/modules/DeferredTask.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
});

const OBSERVER_DEBOUNCE_RATE_MS = 500;
const OBSERVER_DEBOUNCE_TIMEOUT_MS = 5000;

export class BookmarkList {
  #urlsToFetch = new Set();

  #observer;

  #bookmarkCount = new Map();

  #guidToUrl = new Map();

  #observerTask;

  constructor(urls, observer, debounceRate, debounceTimeout) {
    this.setTrackedUrls(urls);
    this.#observer = observer;
    this.handlePlacesEvents = this.handlePlacesEvents.bind(this);
    this.addListeners(debounceRate, debounceTimeout);
  }

  addListeners(
    debounceRate = OBSERVER_DEBOUNCE_RATE_MS,
    debounceTimeout = OBSERVER_DEBOUNCE_TIMEOUT_MS
  ) {
    lazy.PlacesUtils.observers.addListener(
      ["bookmark-added", "bookmark-removed", "bookmark-url-changed"],
      this.handlePlacesEvents
    );
    this.#observerTask = new lazy.DeferredTask(
      () => this.#observer?.(),
      debounceRate,
      debounceTimeout
    );
  }

  async setTrackedUrls(urls) {
    const updatedBookmarkCount = new Map();
    for (const url of urls) {
      const urlHash = lazy.PlacesUtils.history.hashURL(url);
      const count = this.#bookmarkCount.get(urlHash);
      if (count != undefined) {
        updatedBookmarkCount.set(urlHash, count);
      } else {
        this.#urlsToFetch.add(urlHash);
      }
    }
    this.#bookmarkCount = updatedBookmarkCount;

    const updateGuidToUrl = new Map();
    for (const [guid, urlHash] of this.#guidToUrl.entries()) {
      if (updatedBookmarkCount.has(urlHash)) {
        updateGuidToUrl.set(guid, urlHash);
      }
    }
    this.#guidToUrl = updateGuidToUrl;
  }

  async isBookmark(url) {
    if (this.#urlsToFetch.size) {
      await this.#fetchTrackedUrls();
    }
    const urlHash = lazy.PlacesUtils.history.hashURL(url);
    const count = this.#bookmarkCount.get(urlHash);
    return count != undefined ? Boolean(count) : undefined;
  }

  async #fetchTrackedUrls() {
    const urls = [...this.#urlsToFetch];
    this.#urlsToFetch = new Set();
    for (const urlHash of urls) {
      this.#bookmarkCount.set(urlHash, 0);
    }
    const db = await lazy.PlacesUtils.promiseDBConnection();
    for (const chunk of lazy.PlacesUtils.chunkArray(urls, db.variableLimit)) {
      const sql = `SELECT b.guid, p.url_hash
        FROM moz_bookmarks b
        JOIN moz_places p
        ON b.fk = p.id
        WHERE p.url_hash IN (${Array(chunk.length).fill("?").join(",")})`;
      const rows = await db.executeCached(sql, chunk);
      for (const row of rows) {
        this.#cacheBookmark(
          row.getResultByName("guid"),
          row.getResultByName("url_hash")
        );
      }
    }
  }

  async handlePlacesEvents(events) {
    let cacheUpdated = false;
    let needsFetch = false;
    for (const { guid, type, url } of events) {
      const urlHash = lazy.PlacesUtils.history.hashURL(url);
      if (this.#urlsToFetch.has(urlHash)) {
        needsFetch = true;
        continue;
      }
      const isUrlTracked = this.#bookmarkCount.has(urlHash);
      switch (type) {
        case "bookmark-added":
          if (isUrlTracked) {
            this.#cacheBookmark(guid, urlHash);
            cacheUpdated = true;
          }
          break;
        case "bookmark-removed":
          if (isUrlTracked) {
            this.#removeCachedBookmark(guid, urlHash);
            cacheUpdated = true;
          }
          break;
        case "bookmark-url-changed": {
          const oldUrlHash = this.#guidToUrl.get(guid);
          if (oldUrlHash) {
            this.#removeCachedBookmark(guid, oldUrlHash);
            cacheUpdated = true;
          }
          if (isUrlTracked) {
            this.#cacheBookmark(guid, urlHash);
            cacheUpdated = true;
          }
          break;
        }
      }
    }
    if (needsFetch) {
      await this.#fetchTrackedUrls();
      cacheUpdated = true;
    }
    if (cacheUpdated) {
      this.#observerTask.arm();
    }
  }

  removeListeners() {
    lazy.PlacesUtils.observers.removeListener(
      ["bookmark-added", "bookmark-removed", "bookmark-url-changed"],
      this.handlePlacesEvents
    );
    if (!this.#observerTask.isFinalized) {
      this.#observerTask.disarm();
      this.#observerTask.finalize();
    }
    this.setTrackedUrls([]);
  }

  #cacheBookmark(guid, urlHash) {
    const count = this.#bookmarkCount.get(urlHash);
    this.#bookmarkCount.set(urlHash, count + 1);
    this.#guidToUrl.set(guid, urlHash);
  }

  #removeCachedBookmark(guid, urlHash) {
    const count = this.#bookmarkCount.get(urlHash);
    this.#bookmarkCount.set(urlHash, count - 1);
    this.#guidToUrl.delete(guid);
  }
}
