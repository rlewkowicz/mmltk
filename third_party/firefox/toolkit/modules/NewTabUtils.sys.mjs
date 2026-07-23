/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import {
  getSearchProvider,
  SEARCH_SHORTCUTS_EXPERIMENT,
} from "moz-src:///toolkit/components/search/SearchShortcuts.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BinarySearch: "resource://gre/modules/BinarySearch.sys.mjs",
  PageThumbs: "resource://gre/modules/PageThumbs.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
});

let BrowserWindowTracker;
try {
  BrowserWindowTracker = ChromeUtils.importESModule(
    "resource:///modules/BrowserWindowTracker.sys.mjs"
  ).BrowserWindowTracker;
} catch (e) {
}

ChromeUtils.defineLazyGetter(lazy, "gCryptoHash", function () {
  return Cc["@mozilla.org/security/hash;1"].createInstance(Ci.nsICryptoHash);
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "IDNService",
  "@mozilla.org/network/idn-service;1",
  Ci.nsIIDNService
);

const HISTORY_RESULTS_LIMIT = 100;

const LINKS_GET_LINKS_LIMIT = 100;

const TOPIC_IDLE_DAILY = "idle-daily";

const ACTIVITY_STREAM_DEFAULT_LIMIT = 12;

const ACTIVITY_STREAM_DEFAULT_RECENT = 5 * 24 * 60 * 60;

const DEFAULT_SMALL_FAVICON_WIDTH = 16;

function toHash(aValue) {
  let value = new TextEncoder().encode(aValue);
  lazy.gCryptoHash.init(lazy.gCryptoHash.MD5);
  lazy.gCryptoHash.update(value, value.length);
  return lazy.gCryptoHash.finish(true);
}

function handleIDNHost(hostname) {
  try {
    return lazy.IDNService.convertToDisplayIDN(hostname);
  } catch (e) {
    return hostname;
  }
}

ChromeUtils.defineLazyGetter(lazy, "Storage", function () {
  return new LinksStorage();
});

function LinksStorage() {
  try {
    if (this._storedVersion < this._version) {
      if (this._storedVersion < 1) {
        throw new Error("Unsupported newTab storage version");
      }
    } else {
    }
  } catch (ex) {
    console.error(
      "Unable to migrate the newTab storage to the current version. " +
        "Restarting from scratch.\n",
      ex
    );
    this.clear();
  }

  this._storedVersion = this._version;
}

LinksStorage.prototype = {
  get _version() {
    return 1;
  },

  get _prefs() {
    return Object.freeze({
      pinnedLinks: "browser.newtabpage.pinned",
      blockedLinks: "browser.newtabpage.blocked",
    });
  },

  get _storedVersion() {
    if (this.__storedVersion === undefined) {
      this.__storedVersion = Services.prefs.getIntPref(
        "browser.newtabpage.storageVersion",
        1
      );
    }
    return this.__storedVersion;
  },
  set _storedVersion(aValue) {
    Services.prefs.setIntPref("browser.newtabpage.storageVersion", aValue);
    this.__storedVersion = aValue;
  },

  get: function Storage_get(aKey, aDefault) {
    let value;
    try {
      let prefValue = Services.prefs.getStringPref(this._prefs[aKey]);
      value = JSON.parse(prefValue);
    } catch (e) {}
    return value || aDefault;
  },

  set: function Storage_set(aKey, aValue) {
    Services.prefs.setStringPref(this._prefs[aKey], JSON.stringify(aValue));
  },

  remove: function Storage_remove(aKey) {
    Services.prefs.clearUserPref(this._prefs[aKey]);
  },

  clear: function Storage_clear() {
    for (let key in this._prefs) {
      this.remove(key);
    }
  },
};

var PinnedLinks = {
  _links: null,

  get links() {
    if (!this._links) {
      this._links = lazy.Storage.get("pinnedLinks", []);
    }

    return this._links;
  },

  pin: function PinnedLinks_pin(aLink, aIndex) {
    this.unpin(aLink);

    let changed = this._makeHistoryLink(aLink);
    this.links[aIndex] = aLink;
    this.save();
    return changed;
  },

  unpin: function PinnedLinks_unpin(aLink) {
    let index = this._indexOfLink(aLink);
    if (index == -1) {
      return;
    }
    let links = this.links;
    links[index] = null;
    let i = links.length - 1;
    while (i >= 0 && links[i] == null) {
      i--;
    }
    links.splice(i + 1);
    this.save();
  },

  save: function PinnedLinks_save() {
    lazy.Storage.set("pinnedLinks", this.links);
  },

  isPinned: function PinnedLinks_isPinned(aLink) {
    return this._indexOfLink(aLink) != -1;
  },

  resetCache: function PinnedLinks_resetCache() {
    this._links = null;
  },

  _indexOfLink: function PinnedLinks_indexOfLink(aLink) {
    for (let i = 0; i < this.links.length; i++) {
      let link = this.links[i];
      if (link && link.url == aLink.url) {
        return i;
      }
    }

    return -1;
  },

  _makeHistoryLink: function PinnedLinks_makeHistoryLink(aLink) {
    if (!aLink.type || aLink.type == "history") {
      return false;
    }
    aLink.type = "history";
    return true;
  },

  replace: function PinnedLinks_replace(aUrl, aLink) {
    let index = this._indexOfLink({ url: aUrl });
    if (index == -1) {
      return;
    }
    this.links[index] = aLink;
    this.save();
  },
};

var BlockedLinks = {
  _observers: [],

  _links: null,

  addObserver(aObserver) {
    this._observers.push(aObserver);
  },

  removeObservers() {
    this._observers = [];
  },

  get links() {
    if (!this._links) {
      this._links = lazy.Storage.get("blockedLinks", {});
    }

    return this._links;
  },

  block: function BlockedLinks_block(aLink) {
    this._callObservers("onLinkBlocked", aLink);
    this.links[toHash(aLink.url)] = 1;
    this.save();

    PinnedLinks.unpin(aLink);
  },

  unblock: function BlockedLinks_unblock(aLink) {
    if (this.isBlocked(aLink)) {
      delete this.links[toHash(aLink.url)];
      this.save();
      this._callObservers("onLinkUnblocked", aLink);
    }
  },

  save: function BlockedLinks_save() {
    lazy.Storage.set("blockedLinks", this.links);
  },

  isBlocked: function BlockedLinks_isBlocked(aLink) {
    return toHash(aLink.url) in this.links;
  },

  isEmpty: function BlockedLinks_isEmpty() {
    return !Object.keys(this.links).length;
  },

  resetCache: function BlockedLinks_resetCache() {
    this._links = null;
  },

  _callObservers(methodName, ...args) {
    for (let obs of this._observers) {
      if (typeof obs[methodName] == "function") {
        try {
          obs[methodName](...args);
        } catch (err) {
          console.error(err);
        }
      }
    }
  },
};

var PlacesProvider = {
  maxNumLinks: HISTORY_RESULTS_LIMIT,

  init: function PlacesProvider_init() {
    this._placesObserver = new PlacesWeakCallbackWrapper(
      this.handlePlacesEvents.bind(this)
    );
    PlacesObservers.addListener(
      ["page-visited", "page-title-changed", "pages-rank-changed"],
      this._placesObserver
    );
  },

  getLinks: function PlacesProvider_getLinks(aCallback) {
    let options = lazy.PlacesUtils.history.getNewQueryOptions();
    options.maxResults = this.maxNumLinks;

    options.sortingMode =
      Ci.nsINavHistoryQueryOptions.SORT_BY_FRECENCY_DESCENDING;

    let links = [];

    let callback = {
      handleResult(aResultSet) {
        let row;

        while ((row = aResultSet.getNextRow())) {
          let url = row.getResultByIndex(1);
          if (LinkChecker.checkLoadURI(url)) {
            let title = row.getResultByIndex(2);
            let frecency = row.getResultByIndex(12);
            let lastVisitDate = row.getResultByIndex(5);
            links.push({
              url,
              title,
              frecency,
              lastVisitDate,
              type: "history",
            });
          }
        }
      },

      handleError() {
        aCallback([]);
      },

      handleCompletion() {
        let i = 1;
        let outOfOrder = [];
        while (i < links.length) {
          if (Links.compareLinks(links[i - 1], links[i]) > 0) {
            outOfOrder.push(links.splice(i, 1)[0]);
          } else {
            i++;
          }
        }
        for (let link of outOfOrder) {
          i = lazy.BinarySearch.insertionIndexOf(
            Links.compareLinks,
            links,
            link
          );
          links.splice(i, 0, link);
        }

        aCallback(links);
      },
    };

    let query = lazy.PlacesUtils.history.getNewQuery();
    lazy.PlacesUtils.history.asyncExecuteLegacyQuery(query, options, callback);
  },

  addObserver: function PlacesProvider_addObserver(aObserver) {
    this._observers.push(aObserver);
  },

  _observers: [],

  handlePlacesEvents(aEvents) {
    for (let event of aEvents) {
      switch (event.type) {
        case "page-visited": {
          if (event.visitCount == 1 && event.lastKnownTitle) {
            this._callObservers("onLinkChanged", {
              url: event.url,
              title: event.lastKnownTitle,
            });
          }
          break;
        }
        case "page-title-changed": {
          this._callObservers("onLinkChanged", {
            url: event.url,
            title: event.title,
          });
          break;
        }
        case "pages-rank-changed": {
          this._callObservers("onManyLinksChanged");
          break;
        }
      }
    }
  },

  _callObservers: function PlacesProvider__callObservers(aMethodName, aArg) {
    for (let obs of this._observers) {
      if (obs[aMethodName]) {
        try {
          obs[aMethodName](this, aArg);
        } catch (err) {
          console.error(err);
        }
      }
    }
  },
};

var ActivityStreamProvider = {
  THUMB_FAVICON_SIZE: 96,

  _adjustLimitForBlocked({ ignoreBlocked, numItems }) {
    if (ignoreBlocked) {
      return numItems;
    }
    return Object.keys(BlockedLinks.links).length + numItems;
  },

  _commonBookmarkGuidSelect: `(
    SELECT guid
    FROM moz_bookmarks b
    WHERE fk = h.id
      AND type = :bookmarkType
      AND (
        SELECT id
        FROM moz_bookmarks p
        WHERE p.id = b.parent
          AND p.parent <> :tagsFolderId
      ) NOTNULL
    ) AS bookmarkGuid`,

  _commonPlacesWhere: `
    AND hidden = 0
    AND last_visit_date > 0
    AND (SUBSTR(url, 1, 6) == "https:"
      OR SUBSTR(url, 1, 5) == "http:")
  `,

  _getCommonParams(aOptions, aParams = {}) {
    return Object.assign(
      {
        bookmarkType: lazy.PlacesUtils.bookmarks.TYPE_BOOKMARK,
        limit: this._adjustLimitForBlocked(aOptions),
        tagsFolderId: lazy.PlacesUtils.tagsFolderId,
      },
      aParams
    );
  },

  _highlightsColumns: [
    "bookmarkGuid",
    "description",
    "guid",
    "preview_image_url",
    "title",
    "url",
  ],

  _processHighlights(aLinks, aOptions, aType) {
    if (!aOptions.ignoreBlocked) {
      aLinks = aLinks.filter(
        link =>
          !BlockedLinks.isBlocked(
            link.pocket_id ? { url: link.open_url } : link
          )
      );
    }

    return aLinks.slice(0, aOptions.numItems).map(item =>
      Object.assign(item, {
        type: aType,
      })
    );
  },

  _faviconBytesToDataURI(aLinks) {
    return aLinks.map(link => {
      if (link.favicon) {
        let encodedData = btoa(String.fromCharCode.apply(null, link.favicon));
        link.favicon = `data:${link.mimeType};base64,${encodedData}`;
        delete link.mimeType;
      }

      if (link.smallFavicon) {
        let encodedData = btoa(
          String.fromCharCode.apply(null, link.smallFavicon)
        );
        link.smallFavicon = `data:${link.smallFaviconMimeType};base64,${encodedData}`;
        delete link.smallFaviconMimeType;
      }

      return link;
    });
  },

  async _loadIcons(aUri, preferredFaviconWidth) {
    let iconData = {};
    try {
      let faviconData = await lazy.PlacesUtils.favicons.getFaviconForPage(
        aUri,
        this.THUMB_FAVICON_SIZE
      );
      let rawData = faviconData.rawData;
      Object.assign(iconData, {
        favicon: rawData,
        faviconLength: rawData.length,
        faviconRef: faviconData.uri.ref,
        faviconSize: faviconData.width,
        mimeType: faviconData.mimeType,
      });
    } catch (e) {
      return null;
    }

    try {
      let faviconData = await lazy.PlacesUtils.favicons.getFaviconForPage(
        aUri,
        preferredFaviconWidth
      );
      let rawData = faviconData.rawData;
      Object.assign(iconData, {
        smallFavicon: rawData,
        smallFaviconLength: rawData.length,
        smallFaviconRef: faviconData.uri.ref,
        smallFaviconSize: faviconData.width,
        smallFaviconMimeType: faviconData.mimeType,
      });
    } catch (e) {
    }

    return iconData;
  },

  _addFavicons(aLinks) {
    let win;
    if (BrowserWindowTracker) {
      win = BrowserWindowTracker.getTopWindow({
        allowFromInactiveWorkspace: true,
      });
    }
    const preferredFaviconWidth =
      DEFAULT_SMALL_FAVICON_WIDTH * (win ? win.devicePixelRatio : 2);
    return Promise.all(
      aLinks.map(
        link =>
          // eslint-disable-next-line no-async-promise-executor
          new Promise(async resolve => {
            let iconData;
            try {
              let linkUri = Services.io.newURI(link.url);
              iconData = await this._loadIcons(linkUri, preferredFaviconWidth);

              if (!iconData) {
                linkUri = linkUri
                  .mutate()
                  .setScheme(linkUri.scheme === "https" ? "http" : "https")
                  .finalize();
                iconData = await this._loadIcons(
                  linkUri,
                  preferredFaviconWidth
                );
              }
            } catch (e) {
            }

            resolve(Object.assign(link, iconData));
          })
      )
    );
  },

  async getRecentBookmarks(aOptions) {
    const options = Object.assign(
      {
        bookmarkSecondsAgo: ACTIVITY_STREAM_DEFAULT_RECENT,
        ignoreBlocked: false,
        numItems: ACTIVITY_STREAM_DEFAULT_LIMIT,
      },
      aOptions || {}
    );

    const sqlQuery = `
      SELECT
        b.guid AS bookmarkGuid,
        description,
        h.guid,
        preview_image_url,
        b.title,
        b.dateAdded / 1000 AS date_added,
        url
      FROM moz_bookmarks b
      JOIN moz_bookmarks p
        ON p.id = b.parent
      JOIN moz_places h
        ON h.id = b.fk
      WHERE b.dateAdded >= :dateAddedThreshold
        AND b.title NOTNULL
        AND b.type = :bookmarkType
        AND p.parent <> :tagsFolderId
        ${this._commonPlacesWhere}
      ORDER BY b.dateAdded DESC
      LIMIT :limit
    `;

    return this._processHighlights(
      await this.executePlacesQuery(sqlQuery, {
        columns: [...this._highlightsColumns, "date_added"],
        params: this._getCommonParams(options, {
          dateAddedThreshold:
            (Date.now() - options.bookmarkSecondsAgo * 1000) * 1000,
        }),
      }),
      options,
      "bookmark"
    );
  },

  async getTotalBookmarksCount() {
    let sqlQuery = `
      SELECT count(*) FROM moz_bookmarks b
      JOIN moz_bookmarks t ON t.id = b.parent
      AND t.parent <> :tags_folder
     WHERE b.type = :type_bookmark
    `;

    const result = await this.executePlacesQuery(sqlQuery, {
      params: {
        tags_folder: lazy.PlacesUtils.tagsFolderId,
        type_bookmark: lazy.PlacesUtils.bookmarks.TYPE_BOOKMARK,
      },
    });

    return result[0][0];
  },

  async getRecentHistory(aOptions) {
    const options = Object.assign(
      {
        ignoreBlocked: false,
        numItems: ACTIVITY_STREAM_DEFAULT_LIMIT,
      },
      aOptions || {}
    );

    const sqlQuery = `
      SELECT
        ${this._commonBookmarkGuidSelect},
        description,
        guid,
        preview_image_url,
        title,
        url
      FROM moz_places h
      WHERE description NOTNULL
        AND preview_image_url NOTNULL
        ${this._commonPlacesWhere}
      ORDER BY last_visit_date DESC
      LIMIT :limit
    `;

    return this._processHighlights(
      await this.executePlacesQuery(sqlQuery, {
        columns: this._highlightsColumns,
        params: this._getCommonParams(options),
      }),
      options,
      "history"
    );
  },

  async getTopFrecentSites(aOptions) {
    if (!AppConstants.MOZ_PLACES) {
      return [];
    }
    const options = Object.assign(
      {
        ignoreBlocked: false,
        numItems: ACTIVITY_STREAM_DEFAULT_LIMIT,
        topsiteFrecency: lazy.PlacesUtils.history.pageFrecencyThreshold(
          30,
          7,
          false
        ),
        onePerDomain: true,
        includeFavicon: true,
        hideWithSearchParam: Services.prefs.getCharPref(
          "browser.newtabpage.activity-stream.hideTopSitesWithSearchParam",
          ""
        ),
      },
      aOptions || {}
    );

    const origNumItems = options.numItems;
    if (options.onePerDomain) {
      options.numItems *= 2 * 10;
    }

    const sqlQuery = `
      SELECT
        ${this._commonBookmarkGuidSelect},
        frecency,
        guid,
        last_visit_date / 1000 AS lastVisitDate,
        rev_host,
        title,
        url,
        "history" as type
      FROM moz_places h
      WHERE frecency >= :frecencyThreshold
        ${this._commonPlacesWhere}
      ORDER BY frecency DESC
      LIMIT :limit
    `;

    let links = await this.executePlacesQuery(sqlQuery, {
      columns: [
        "bookmarkGuid",
        "frecency",
        "guid",
        "lastVisitDate",
        "title",
        "url",
        "type",
      ],
      params: this._getCommonParams(options, {
        frecencyThreshold: options.topsiteFrecency,
      }),
    });

    function isOtherBetter(link, other) {
      if (other.frecency === link.frecency) {
        if (other.lastVisitDate === link.lastVisitDate) {
          return other.url < link.url;
        }
        return other.lastVisitDate > link.lastVisitDate;
      }
      return other.frecency > link.frecency;
    }

    function setBetterLink(map, link, hostMatcher, combiner = () => {}) {
      const host = hostMatcher(link.url)[1];
      if (map.has(host)) {
        const other = map.get(host);
        if (isOtherBetter(link, other)) {
          link = other;
        }
        combiner(link, other);
      }
      map.set(host, link);
    }

    if (
      Services.prefs.getBoolPref(
        `browser.newtabpage.activity-stream.${SEARCH_SHORTCUTS_EXPERIMENT}`,
        false
      )
    ) {
      links.forEach(link => {
        let searchProvider = getSearchProvider(NewTabUtils.shortURL(link));
        if (searchProvider) {
          link.original_url = link.url;
          link.url = searchProvider.url;
        }
      });
    }

    if (options.hideWithSearchParam) {
      let [key, value] = options.hideWithSearchParam.split("=");
      links = links.filter(link => {
        let searchParams = URL.parse(link.url)?.searchParams;
        if (searchParams) {
          return value === undefined
            ? !searchParams.has(key)
            : !searchParams.getAll(key).includes(value);
        }
        return true;
      });
    }

    if (!options.ignoreBlocked) {
      links = links.filter(link => !BlockedLinks.isBlocked(link));
    }

    if (options.onePerDomain) {
      const exactHosts = new Map();
      for (const link of links) {
        setBetterLink(exactHosts, link, url => url.match(/:\/\/([^\/]+)/));
      }

      const hosts = new Map();
      for (const link of exactHosts.values()) {
        setBetterLink(
          hosts,
          link,
          url => url.match(/:\/\/(?:www\.)?([^\/]+)/),
          (targetLink, otherLink) => {
            targetLink.frecency = link.frecency + otherLink.frecency;
          }
        );
      }

      links = [...hosts.values()];
    }
    links = links.sort(isOtherBetter).slice(0, origNumItems);

    if (!options.includeFavicon) {
      return links;
    }
    return this._faviconBytesToDataURI(await this._addFavicons(links));
  },

  async getBookmark(aInfo) {
    let bookmark = await lazy.PlacesUtils.bookmarks.fetch(aInfo);
    if (!bookmark) {
      return null;
    }
    let result = {};
    result.bookmarkGuid = bookmark.guid;
    result.bookmarkTitle = bookmark.title;
    result.lastModified = bookmark.lastModified.getTime();
    result.url = bookmark.url.href;
    return result;
  },

  getUserMonthlyActivity() {
    let sqlQuery = `
      SELECT count(*),
        strftime('%Y-%m-%d', visit_date/1000000.0, 'unixepoch', 'localtime') as date_format
      FROM moz_historyvisits
      WHERE visit_date > 0
      AND visit_date > strftime('%s','now','localtime','start of day','-27 days','utc') * 1000000
      GROUP BY date_format
      ORDER BY date_format ASC
    `;

    return this.executePlacesQuery(sqlQuery);
  },

  async executePlacesQuery(aQuery, aOptions = {}) {
    let { columns, params } = aOptions;
    let items = [];
    let queryError = null;
    let conn = await lazy.PlacesUtils.promiseDBConnection();
    await conn.executeCached(aQuery, params, (aRow, aCancel) => {
      try {
        let item = null;
        if (columns && Array.isArray(columns)) {
          item = {};
          columns.forEach(column => {
            item[column] = aRow.getResultByName(column);
          });
        } else {
          item = [];
          for (let i = 0; i < aRow.numEntries; i++) {
            item.push(aRow.getResultByIndex(i));
          }
        }
        items.push(item);
      } catch (e) {
        queryError = e;
        aCancel();
      }
    });
    if (queryError) {
      throw new Error(queryError);
    }
    return items;
  },
};

var ActivityStreamLinks = {
  blockURL(aLink) {
    BlockedLinks.block(aLink);
  },

  onLinkBlocked(aLink) {
    Services.obs.notifyObservers(null, "newtab-linkBlocked", aLink.url);
  },

  addBookmark(aData, aBrowserWindow) {
    const { url, title } = aData;
    return aBrowserWindow.PlacesCommandHook.bookmarkLink(url, title);
  },

  deleteBookmark(aBookmarkGuid) {
    return lazy.PlacesUtils.bookmarks.remove(aBookmarkGuid);
  },

  deleteHistoryEntry(aUrl) {
    const url = aUrl;
    PinnedLinks.unpin({ url });
    return lazy.PlacesUtils.history.remove(url);
  },

  async getHighlights(aOptions = {}) {
    if (!AppConstants.MOZ_PLACES) {
      return [];
    }
    aOptions.numItems = aOptions.numItems || ACTIVITY_STREAM_DEFAULT_LIMIT;
    const results = [];

    if (!aOptions.excludeBookmarks) {
      results.push(
        ...(await ActivityStreamProvider.getRecentBookmarks(aOptions))
      );
    }

    if (aOptions.numItems - results.length > 0 && !aOptions.excludeHistory) {
      const history = await ActivityStreamProvider.getRecentHistory(aOptions);

      const bookmarkUrls = new Set(results.map(({ url }) => url));
      for (const page of history) {
        if (!bookmarkUrls.has(page.url)) {
          results.push(page);

          if (results.length === aOptions.numItems) {
            break;
          }
        }
      }
    }

    if (aOptions.withFavicons) {
      return ActivityStreamProvider._faviconBytesToDataURI(
        await ActivityStreamProvider._addFavicons(results)
      );
    }

    return results;
  },

  async getTopSites(aOptions = {}) {
    return ActivityStreamProvider.getTopFrecentSites(aOptions);
  },
};

var Links = {
  maxNumLinks: LINKS_GET_LINKS_LIMIT,

  _providers: new Map(),

  _sortProperties: ["frecency", "lastVisitDate", "url"],

  _populateCallbacks: [],

  _observers: [],

  addObserver(aObserver) {
    this._observers.push(aObserver);
  },

  addProvider: function Links_addProvider(aProvider) {
    this._providers.set(aProvider, null);
    aProvider.addObserver(this);
  },

  removeProvider: function Links_removeProvider(aProvider) {
    if (!this._providers.delete(aProvider)) {
      throw new Error("Unknown provider");
    }
  },

  populateCache: function Links_populateCache(aCallback, aForce) {
    let callbacks = this._populateCallbacks;

    callbacks.push(aCallback);

    if (callbacks.length > 1) {
      return;
    }

    function executeCallbacks() {
      while (callbacks.length) {
        let callback = callbacks.shift();
        if (callback) {
          try {
            callback();
          } catch (e) {
          }
        }
      }
    }

    let numProvidersRemaining = this._providers.size;
    for (let [provider ] of this._providers) {
      this._populateProviderCache(
        provider,
        () => {
          if (--numProvidersRemaining == 0) {
            executeCallbacks();
          }
        },
        aForce
      );
    }

    this._addObserver();
  },

  getLinks: function Links_getLinks() {
    let pinnedLinks = Array.from(PinnedLinks.links);
    let links = this._getMergedProviderLinks();

    let sites = new Set();
    for (let link of pinnedLinks) {
      if (link) {
        sites.add(NewTabUtils.extractSite(link.url));
      }
    }

    links = links.filter(function (link) {
      let site = NewTabUtils.extractSite(link.url);
      if (site == null || sites.has(site)) {
        return false;
      }
      sites.add(site);

      return !BlockedLinks.isBlocked(link) && !PinnedLinks.isPinned(link);
    });

    for (let i = 0; i < pinnedLinks.length && links.length; i++) {
      if (!pinnedLinks[i]) {
        pinnedLinks[i] = links.shift();
      }
    }

    if (links.length) {
      pinnedLinks = pinnedLinks.concat(links);
    }

    for (let link of pinnedLinks) {
      if (link) {
        link.baseDomain = NewTabUtils.extractSite(link.url);
      }
    }
    return pinnedLinks;
  },

  resetCache: function Links_resetCache() {
    for (let provider of this._providers.keys()) {
      this._providers.set(provider, null);
    }
  },

  compareLinks: function Links_compareLinks(aLink1, aLink2) {
    for (let prop of this._sortProperties) {
      if (!(prop in aLink1) || !(prop in aLink2)) {
        throw new Error("Comparable link missing required property: " + prop);
      }
    }
    return (
      aLink2.frecency - aLink1.frecency ||
      aLink2.lastVisitDate - aLink1.lastVisitDate ||
      aLink1.url.localeCompare(aLink2.url)
    );
  },

  _incrementSiteMap(map, link) {
    if (NewTabUtils.blockedLinks.isBlocked(link)) {
      return;
    }
    let site = NewTabUtils.extractSite(link.url);
    map.set(site, (map.get(site) || 0) + 1);
  },

  _decrementSiteMap(map, link) {
    if (NewTabUtils.blockedLinks.isBlocked(link)) {
      return;
    }
    let site = NewTabUtils.extractSite(link.url);
    let previousURLCount = map.get(site);
    if (previousURLCount === 1) {
      map.delete(site);
    } else {
      map.set(site, previousURLCount - 1);
    }
  },

  _adjustSiteMapAndNotify(aLink, increment = true) {
    for (let [,  cache] of this._providers) {
      if (cache.linkMap.get(aLink.url)) {
        if (increment) {
          this._incrementSiteMap(cache.siteMap, aLink);
          continue;
        }
        this._decrementSiteMap(cache.siteMap, aLink);
      }
    }
    this._callObservers("onLinkChanged", aLink);
  },

  onLinkBlocked(aLink) {
    this._adjustSiteMapAndNotify(aLink, false);
  },

  onLinkUnblocked(aLink) {
    this._adjustSiteMapAndNotify(aLink);
  },

  populateProviderCache(provider, callback) {
    if (!this._providers.has(provider)) {
      throw new Error(
        "Can only populate provider cache for existing provider."
      );
    }

    return this._populateProviderCache(provider, callback, false);
  },

  _populateProviderCache(aProvider, aCallback, aForce) {
    let cache = this._providers.get(aProvider);
    let createCache = !cache;
    if (createCache) {
      cache = {
        populatePromise: new Promise(resolve => resolve()),
      };
      this._providers.set(aProvider, cache);
    }
    cache.populatePromise = cache.populatePromise.then(() => {
      return new Promise(resolve => {
        if (!createCache && !aForce) {
          aCallback();
          resolve();
          return;
        }
        aProvider.getLinks(links => {
          links = links.filter(link => !!link);
          cache.sortedLinks = links;
          cache.siteMap = links.reduce((map, link) => {
            this._incrementSiteMap(map, link);
            return map;
          }, new Map());
          cache.linkMap = links.reduce((map, link) => {
            map.set(link.url, link);
            return map;
          }, new Map());
          aCallback();
          resolve();
        });
      });
    });
  },

  _getMergedProviderLinks: function Links__getMergedProviderLinks() {
    let linkLists = [];
    for (let provider of this._providers.keys()) {
      let links = this._providers.get(provider);
      if (links && links.sortedLinks) {
        linkLists.push(links.sortedLinks.slice());
      }
    }

    return this.mergeLinkLists(linkLists);
  },

  mergeLinkLists: function Links_mergeLinkLists(linkLists) {
    if (linkLists.length == 1) {
      return linkLists[0];
    }

    function getNextLink() {
      let minLinks = null;
      for (let links of linkLists) {
        if (
          links.length &&
          (!minLinks || Links.compareLinks(links[0], minLinks[0]) < 0)
        ) {
          minLinks = links;
        }
      }
      return minLinks ? minLinks.shift() : null;
    }

    let finalLinks = [];
    for (
      let nextLink = getNextLink();
      nextLink && finalLinks.length < this.maxNumLinks;
      nextLink = getNextLink()
    ) {
      finalLinks.push(nextLink);
    }

    return finalLinks;
  },

  onLinkChanged: function Links_onLinkChanged(
    aProvider,
    aLink,
    aIndex = -1,
    aDeleted = false
  ) {
    if (!("url" in aLink)) {
      throw new Error("Changed links must have a url property");
    }

    let links = this._providers.get(aProvider);
    if (!links || !links.linkMap) {
      return;
    }

    let { sortedLinks, siteMap, linkMap } = links;
    let existingLink = linkMap.get(aLink.url);
    let insertionLink = null;

    if (existingLink) {
      if (this._sortProperties.some(prop => prop in aLink)) {
        let idx = aIndex;
        if (idx < 0) {
          idx = this._indexOf(sortedLinks, existingLink);
        } else if (this.compareLinks(aLink, sortedLinks[idx]) != 0) {
          throw new Error("aLink should be the same as sortedLinks[idx]");
        }

        if (idx < 0) {
          throw new Error("Link should be in _sortedLinks if in _linkMap");
        }
        sortedLinks.splice(idx, 1);

        if (aDeleted) {
          linkMap.delete(existingLink.url);
          this._decrementSiteMap(siteMap, existingLink);
        } else {
          Object.assign(existingLink, aLink);

          insertionLink = existingLink;
        }
      }
      if ("title" in aLink && aLink.title != existingLink.title) {
        existingLink.title = aLink.title;
      }
    } else if (this._sortProperties.every(prop => prop in aLink)) {
      if (sortedLinks.length && sortedLinks.length == aProvider.maxNumLinks) {
        let lastLink = sortedLinks[sortedLinks.length - 1];
        if (this.compareLinks(lastLink, aLink) < 0) {
          return;
        }
      }
      insertionLink = {};
      for (let prop in aLink) {
        insertionLink[prop] = aLink[prop];
      }
      linkMap.set(aLink.url, insertionLink);
      this._incrementSiteMap(siteMap, aLink);
    }

    if (insertionLink) {
      let idx = this._insertionIndexOf(sortedLinks, insertionLink);
      sortedLinks.splice(idx, 0, insertionLink);
      if (sortedLinks.length > aProvider.maxNumLinks) {
        let lastLink = sortedLinks.pop();
        linkMap.delete(lastLink.url);
        this._decrementSiteMap(siteMap, lastLink);
      }
    }
  },

  onManyLinksChanged: function Links_onManyLinksChanged(aProvider) {
    this._populateProviderCache(aProvider, () => {}, true);
  },

  _indexOf: function Links__indexOf(aArray, aLink) {
    return this._binsearch(aArray, aLink, "indexOf");
  },

  _insertionIndexOf: function Links__insertionIndexOf(aArray, aLink) {
    return this._binsearch(aArray, aLink, "insertionIndexOf");
  },

  _binsearch: function Links__binsearch(aArray, aLink, aMethod) {
    return lazy.BinarySearch[aMethod](this.compareLinks, aArray, aLink);
  },

  observe: function Links_observe() {
    this.resetCache();
  },

  _callObservers(methodName, ...args) {
    for (let obs of this._observers) {
      if (typeof obs[methodName] == "function") {
        try {
          obs[methodName](this, ...args);
        } catch (err) {
          console.error(err);
        }
      }
    }
  },

  _addObserver: function Links_addObserver() {
    Services.obs.addObserver(this, "browser:purge-session-history");
    this._addObserver = function () {};
  },

  QueryInterface: ChromeUtils.generateQI(["nsIObserver"]),
};

Links.compareLinks = Links.compareLinks.bind(Links);

var Telemetry = {
  init: function Telemetry_init() {
    Services.obs.addObserver(this, TOPIC_IDLE_DAILY);
  },

  uninit: function Telemetry_uninit() {
    Services.obs.removeObserver(this, TOPIC_IDLE_DAILY);
  },

  _collect: function Telemetry_collect() {
  },

  observe: function Telemetry_observe() {
    this._collect();
  },
};

var LinkChecker = {
  _cache: {},

  get flags() {
    return (
      Ci.nsIScriptSecurityManager.DISALLOW_INHERIT_PRINCIPAL |
      Ci.nsIScriptSecurityManager.DONT_REPORT_ERRORS
    );
  },

  checkLoadURI: function LinkChecker_checkLoadURI(aURI) {
    if (!(aURI in this._cache)) {
      this._cache[aURI] = this._doCheckLoadURI(aURI);
    }

    return this._cache[aURI];
  },

  _doCheckLoadURI: function Links_doCheckLoadURI(aURI) {
    try {
      let systemPrincipal = Services.scriptSecurityManager.getSystemPrincipal();
      Services.scriptSecurityManager.checkLoadURIStrWithPrincipal(
        systemPrincipal,
        aURI,
        this.flags
      );
      return true;
    } catch (e) {
      return false;
    }
  },
};

var ExpirationFilter = {
  init: function ExpirationFilter_init() {
    lazy.PageThumbs.addExpirationFilter(this);
  },

  filterForThumbnailExpiration:
    function ExpirationFilter_filterForThumbnailExpiration(aCallback) {
      Links.populateCache(function () {
        let urls = [];

        for (let link of Links.getLinks().slice(0, 25)) {
          if (link && link.url) {
            urls.push(link.url);
          }
        }

        aCallback(urls);
      });
    },
};

export var NewTabUtils = {
  _initialized: false,

  extractSite: function Links_extractSite(url) {
    let host;
    try {
      host = Services.io.newURI(url).asciiHost;
    } catch (ex) {
      return null;
    }

    return host.replace(/^(m|mobile|www\d*)\./, "");
  },

  init: function NewTabUtils_init() {
    if (this.initWithoutProviders() && AppConstants.MOZ_PLACES) {
      PlacesProvider.init();
      Links.addProvider(PlacesProvider);
      BlockedLinks.addObserver(Links);
      BlockedLinks.addObserver(ActivityStreamLinks);
    }
  },

  initWithoutProviders: function NewTabUtils_initWithoutProviders() {
    if (!this._initialized) {
      this._initialized = true;
      ExpirationFilter.init();
      Telemetry.init();
      return true;
    }
    return false;
  },

  uninit: function NewTabUtils_uninit() {
    if (this.initialized) {
      Telemetry.uninit();
      BlockedLinks.removeObservers();
    }
  },

  getProviderLinks(aProvider) {
    let cache = Links._providers.get(aProvider);
    if (cache && cache.sortedLinks) {
      return cache.sortedLinks;
    }
    return [];
  },

  isTopSiteGivenProvider(aSite, aProvider) {
    let cache = Links._providers.get(aProvider);
    if (cache && cache.siteMap) {
      return cache.siteMap.has(aSite);
    }
    return false;
  },

  restore: function NewTabUtils_restore() {
    lazy.Storage.clear();
    Links.resetCache();
    PinnedLinks.resetCache();
    BlockedLinks.resetCache();

    Links.populateCache(() => {}, true);
  },

  undoAll: function NewTabUtils_undoAll(aCallback) {
    lazy.Storage.remove("blockedLinks");
    Links.resetCache();
    BlockedLinks.resetCache();
    Links.populateCache(aCallback, true);
  },

  getETLD: function NewTabUtils_getETLD(host) {
    try {
      return Services.eTLD.getPublicSuffixFromHost(host);
    } catch (err) {
      return "";
    }
  },

  shortHostname: function NewTabUtils_shortHostname(hostname) {
    if (!hostname) {
      return "";
    }

    const newHostname = hostname.replace(/^www\./i, "").toLowerCase();

    const eTLD = this.getETLD(newHostname);
    const eTLDExtra =
      eTLD.length && newHostname.endsWith(eTLD) ? -(eTLD.length + 1) : Infinity;

    return handleIDNHost(newHostname.slice(0, eTLDExtra) || newHostname);
  },

  shortURL: function NewTabUtils_shortURL({ url }) {
    if (!url) {
      return "";
    }

    let parsed;
    try {
      parsed = new URL(url);
    } catch (ex) {
      return url;
    }

    return (
      this.shortHostname(parsed.hostname) || parsed.pathname || parsed.href
    );
  },

  getUtcOffset(surfaceID) {
    const surfaceRestrictions = { NEW_TAB_EN_US: { min: 24 - 8, max: 24 - 4 } }; 
    const restriction = surfaceID && surfaceRestrictions[surfaceID];
    if (surfaceID && !restriction) {
      return 0;
    }
    const offsetInMinutes = new Date().getTimezoneOffset(); 
    const offsetInHours = -offsetInMinutes / 60; 
    let utc_offset = Math.round(offsetInHours);

    if (utc_offset <= 0) {
      utc_offset += 24;
    }
    if (restriction) {
      if (utc_offset < restriction.min) {
        utc_offset = restriction.min;
      }
      if (utc_offset > restriction.max) {
        utc_offset = restriction.max;
      }
    }

    return utc_offset;
  },

  normalizeOs() {
    const osString = Services.appinfo.OS;
    if (osString.startsWith("Windows") || osString.startsWith("WINNT")) {
      return "windows";
    } else if (osString.startsWith("Darwin")) {
      return "mac";
    } else if (
      osString.includes("Linux") ||
      osString.includes("BSD") ||
      osString.includes("SunOS") ||
      osString.includes("Solaris")
    ) {
      return "linux";
    } else if (osString.startsWith("iOS") || osString.includes("iPhone")) {
      return "ios";
    } else if (osString.startsWith("Android")) {
      return "android";
    }
    return "other";
  },

  links: Links,
  pinnedLinks: PinnedLinks,
  blockedLinks: BlockedLinks,
  activityStreamLinks: ActivityStreamLinks,
  activityStreamProvider: ActivityStreamProvider,
};
