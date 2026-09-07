/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BinarySearch: "resource://gre/modules/BinarySearch.sys.mjs",
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  DeferredTask: "resource://gre/modules/DeferredTask.sys.mjs",
  ObjectUtils: "resource://gre/modules/ObjectUtils.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
});

const BULK_PLACES_EVENTS_THRESHOLD = 50;
const OBSERVER_DEBOUNCE_RATE_MS = 500;
const OBSERVER_DEBOUNCE_TIMEOUT_MS = 5000;





export class PlacesQuery {
  #cache = null;
  cachedHistoryOptions = null;
  #historyListener = null;
  #historyListenerCallback = null;
  #historyObserverTask = null;

  #isClosed = false;

  #searchInProgress = false;

  get cachedHistory() {
    return this.#cache?.data ?? null;
  }

  async getHistory({ daysOld = 60, limit, sortBy = SORT_BY.DATE } = {}) {
    const options = { daysOld, limit, sortBy };
    const cacheInvalid =
      this.cachedHistory == null ||
      !lazy.ObjectUtils.deepEqual(options, this.cachedHistoryOptions);
    if (cacheInvalid) {
      this.initializeCache(options);
      await this.fetchHistory();
    }
    if (!this.#historyListener && !this.#isClosed) {
      this.#initHistoryListener();
    }
    return this.cachedHistory;
  }

  initializeCache(options = this.cachedHistoryOptions) {
    this.#cache = new HistoryCache(options.sortBy, this);
    this.cachedHistoryOptions = options;
    this.#isClosed = false;
  }

  async fetchHistory() {
    const { daysOld, limit, sortBy } = this.cachedHistoryOptions;
    const db = await lazy.PlacesUtils.promiseDBConnection();
    let groupBy;
    switch (sortBy) {
      case SORT_BY.DATE:
      case SORT_BY.DATESITE:
        groupBy = "url, date(visit_date / 1000000, 'unixepoch', 'localtime')";
        break;
      case SORT_BY.SITE:
      case SORT_BY.LAST_VISITED:
        groupBy = "url";
        break;
    }
    const whereClause =
      daysOld == Infinity
        ? ""
        : `WHERE visit_date >= (strftime('%s','now','localtime','start of day','-${Number(
            daysOld
          )} days','utc') * 1000000)`;
    const sql = `SELECT MAX(visit_date) as visit_date, title, url, guid
      FROM moz_historyvisits v
      JOIN moz_places h
      ON v.place_id = h.id
      AND hidden = 0
      ${whereClause}
      GROUP BY ${groupBy}
      ORDER BY visit_date DESC
      LIMIT ${limit > 0 ? limit : -1}`;
    const rows = await db.executeCached(sql);
    if (this.#isClosed) {
      return;
    }
    for (const row of rows) {
      const visit = this.formatRowAsVisit(row);
      this.#cache.append(visit);
    }
  }

  async searchHistory(query, limit) {
    const { sortBy } = this.cachedHistoryOptions;
    const db = await lazy.PlacesUtils.promiseLargeCacheDBConnection();
    let orderBy;
    switch (sortBy) {
      case SORT_BY.DATE:
        orderBy = "visit_date DESC";
        break;
      case SORT_BY.SITE:
        orderBy = "url";
        break;
      case SORT_BY.DATESITE:
        orderBy = "visit_date DESC, url";
        break;
      case SORT_BY.LAST_VISITED:
        orderBy = "visit_date DESC";
        break;
    }
    const sql = `SELECT MAX(visit_date) as visit_date, title, url, guid
      FROM moz_historyvisits v
      JOIN moz_places h
      ON v.place_id = h.id
      WHERE AUTOCOMPLETE_MATCH(:query, url, title, NULL, 1, 1, 1, 1, :matchBehavior, :searchBehavior, NULL)
      AND hidden = 0
      GROUP BY url
      ORDER BY ${orderBy}
      LIMIT ${limit > 0 ? limit : -1}`;
    if (this.#searchInProgress) {
      db.interrupt();
    }
    try {
      this.#searchInProgress = true;
      const rows = await db.executeCached(sql, {
        query,
        matchBehavior: Ci.mozIPlacesAutoComplete.MATCH_ANYWHERE_UNMODIFIED,
        searchBehavior: Ci.mozIPlacesAutoComplete.BEHAVIOR_HISTORY,
      });
      return rows.map(row => this.formatRowAsVisit(row));
    } finally {
      this.#searchInProgress = false;
    }
  }

  observeHistory(callback) {
    this.#historyListenerCallback = callback;
  }

  close() {
    this.#isClosed = true;
    this.#cache = null;
    this.cachedHistoryOptions = null;
    if (this.#historyListener) {
      PlacesObservers.removeListener(
        [
          "page-removed",
          "page-visited",
          "history-cleared",
          "page-title-changed",
        ],
        this.#historyListener
      );
    }
    this.#historyListener = null;
    this.#historyListenerCallback = null;
    if (this.#historyObserverTask && !this.#historyObserverTask.isFinalized) {
      this.#historyObserverTask.disarm();
      this.#historyObserverTask.finalize();
    }
  }

  #initHistoryListener() {
    this.#historyObserverTask = new lazy.DeferredTask(
      async () => {
        if (typeof this.#historyListenerCallback === "function") {
          const history = await this.getHistory(this.cachedHistoryOptions);
          this.#historyListenerCallback(history);
        }
      },
      OBSERVER_DEBOUNCE_RATE_MS,
      OBSERVER_DEBOUNCE_TIMEOUT_MS
    );
    this.#historyListener = async events => {
      if (
        events.length >= BULK_PLACES_EVENTS_THRESHOLD ||
        events.some(({ type }) => type === "page-removed")
      ) {
        this.#cache = null;
      } else if (this.cachedHistory != null) {
        for (const event of events) {
          switch (event.type) {
            case "page-visited":
              this.handlePageVisited( (event));
              break;
            case "history-cleared":
              this.initializeCache();
              break;
            case "page-title-changed":
              this.handlePageTitleChanged(
                 (event)
              );
              break;
          }
        }
      }
      this.#historyObserverTask.arm();
    };
    PlacesObservers.addListener(
      ["page-removed", "page-visited", "history-cleared", "page-title-changed"],
      this.#historyListener
    );
  }

  handlePageVisited(event) {
    if (event.hidden) {
      return null;
    }
    const visit = this.formatEventAsVisit(event);
    this.#cache.insertSorted(visit);
    return visit;
  }

  handlePageTitleChanged(event) {
    this.#cache.updateTitle(event.url, event.title);
  }

  getStartOfDayTimestamp(date) {
    return new Date(
      date.getFullYear(),
      date.getMonth(),
      date.getDate()
    ).getTime();
  }

  getStartOfMonthTimestamp(date) {
    return new Date(date.getFullYear(), date.getMonth()).getTime();
  }

  formatRowAsVisit(row) {
    return {
      date: lazy.PlacesUtils.toDate(row.getResultByName("visit_date")),
      // @ts-expect-error - Bug 1966462
      title: row.getResultByName("title"),
      // @ts-expect-error - Bug 1966462
      url: row.getResultByName("url"),
      // @ts-expect-error - Bug 1966462
      guid: row.getResultByName("guid"),
    };
  }

  formatEventAsVisit(event) {
    return {
      date: new Date(event.visitTime),
      title: event.lastKnownTitle,
      url: event.url,
      guid: event.pageGuid,
    };
  }
}

const SORT_BY = Object.freeze({
  DATE: "date",

  DATESITE: "datesite",

  LAST_VISITED: "lastvisited",

  SITE: "site",
});

class HistoryCache {
  #cache;
  #urlCache = new Map();
  #sortBy;
  #placesQuery;

  constructor(sortBy, placesQuery) {
    this.#sortBy = sortBy;
    this.#placesQuery = placesQuery;
    this.#cache = sortBy === SORT_BY.LAST_VISITED ? [] : new Map();
  }

  get data() {
    return this.#cache;
  }

  append(visit) {
    let container = this.#getContainerForVisit(visit);
    container.push(visit);
    this.#addUrlToCache(visit);
  }

  insertSorted(visit) {
    let container = this.#getContainerForVisit(visit);
    if (!this.#handleDuplicate(visit, container)) {
      return false;
    }
    this.#insertSortedIntoContainer(visit, container);
    this.#addUrlToCache(visit);
    return true;
  }

  updateTitle(url, title) {
    let visits = this.#urlCache.get(url);
    if (visits) {
      for (let visit of visits) {
        visit.title = title;
      }
    }
  }

  #getContainerForVisit(visit) {
    switch (this.#sortBy) {
      case SORT_BY.LAST_VISITED: {
        return  (this.#cache);
      }

      case SORT_BY.DATE: {
        let dateKey = this.#placesQuery.getStartOfDayTimestamp(visit.date);
        return this.#getOrCreateContainer(
           (this.#cache),
          dateKey
        );
      }

      case SORT_BY.SITE: {
        let siteKey = this.#getSiteKey(visit.url);
        return this.#getOrCreateContainer(
           (this.#cache),
          siteKey
        );
      }

      case SORT_BY.DATESITE: {
        let dateKey = this.#placesQuery.getStartOfDayTimestamp(visit.date);
        let siteKey = this.#getSiteKey(visit.url);

        let typedCache =
           (
            this.#cache
          );
        if (!typedCache.has(dateKey)) {
          typedCache.set(dateKey, new Map());
        }

        let dateContainer = typedCache.get(dateKey);
        return this.#getOrCreateContainer(dateContainer, siteKey);
      }

      default:
        throw new Error(`Unknown sortBy option: ${this.#sortBy}`);
    }
  }

  #getOrCreateContainer(map, key) {
    let container = map.get(key);
    if (!container) {
      container = [];
      map.set(key, container);
    }
    return container;
  }

  #getSiteKey(url) {
    let protocol = URL.parse(url)?.protocol;
    return protocol == "http:" || protocol == "https:"
      ? lazy.BrowserUtils.formatURIStringForDisplay(url)
      : "";
  }

  #addUrlToCache(visit) {
    let existing = this.#urlCache.get(visit.url);
    if (existing) {
      existing.add(visit);
    } else {
      this.#urlCache.set(visit.url, new Set([visit]));
    }
  }

  #handleDuplicate(visit, container) {
    let existingVisitsForUrl = this.#urlCache.get(visit.url);
    if (!existingVisitsForUrl) {
      return true;
    }

    for (let existingVisit of existingVisitsForUrl) {
      if (container.includes(existingVisit)) {
        if (existingVisit.date.getTime() >= visit.date.getTime()) {
          return false;
        }
        container.splice(container.indexOf(existingVisit), 1);
        existingVisitsForUrl.delete(existingVisit);
        break;
      }
    }

    return true;
  }

  #insertSortedIntoContainer(visit, container) {
    let insertionPoint = 0;
    if (visit.date.getTime() < container[0]?.date.getTime()) {
      insertionPoint = lazy.BinarySearch.insertionIndexOf(
        (a, b) => b.date.getTime() - a.date.getTime(),
        container,
        visit
      );
    }
    container.splice(insertionPoint, 0, visit);
  }
}
