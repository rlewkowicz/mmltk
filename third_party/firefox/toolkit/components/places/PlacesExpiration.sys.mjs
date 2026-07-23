/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
});

const TOPIC_DEBUG_START_EXPIRATION = "places-debug-start-expiration";
const TOPIC_IDLE_BEGIN = "idle";
const TOPIC_IDLE_END = "active";
const TOPIC_IDLE_DAILY = "idle-daily";
const TOPIC_TESTING_MODE = "testing-mode";
const TOPIC_TEST_INTERVAL_CHANGED = "test-interval-changed";

const DATABASE_MAX_SIZE = 78643200; 

const EXPIRE_LIMIT_PER_STEP = 6;
const EXPIRE_LIMIT_PER_LARGE_STEP_MULTIPLIER = 10;

const EXPIRE_AGGRESSIVITY_MULTIPLIER = 3;

const URIENTRY_AVG_SIZE = 700;

const IDLE_TIMEOUT_SECONDS = 5 * 60;

const OVERLIMIT_PAGES_THRESHOLD = 1000;

const MSECS_PER_DAY = 86400000;

const LIMIT = {
  SMALL: 0,

  LARGE: 1,

  UNLIMITED: 2,

  DEBUG: 3,
};

const STATUS = {
  CLEAN: 0,
  DIRTY: 1,
  UNKNOWN: 2,
};

const ACTION = {
  TIMED: 1 << 0, 

  TIMED_OVERLIMIT: 1 << 1,

  SHUTDOWN_DIRTY: 1 << 2,

  IDLE_DIRTY: 1 << 3,

  IDLE_DAILY: 1 << 4,

  DEBUG: 1 << 5,
};

const EXPIRATION_QUERIES = {
  QUERY_FIND_EXOTIC_VISITS_TO_EXPIRE: {
    sql: `INSERT INTO expiration_notify (v_id, url, guid, visit_date, reason)
      WITH visits AS (
        SELECT v.id, url, guid, visit_type, visit_date, visit_count, hidden, typed
        FROM moz_historyvisits v
        JOIN moz_places h ON h.id = v.place_id
        WHERE visit_date < strftime('%s','now','localtime','start of day','-90 days','utc') * 1000000
        AND visit_type NOT IN (5,6)
      )
      SELECT id, url, guid, visit_date, "exotic"
      FROM visits
      WHERE (hidden = 1 AND typed = 0 AND visit_count <= 1) OR visit_type = 7
      UNION ALL
      SELECT id, url, guid, visit_date, "exotic"
      FROM visits
      WHERE visit_count = 1 AND LENGTH(url) > 255
      ORDER BY visit_date ASC
      LIMIT :limit_visits`,
    actions:
      ACTION.TIMED_OVERLIMIT |
      ACTION.IDLE_DIRTY |
      ACTION.IDLE_DAILY |
      ACTION.DEBUG,
  },

  QUERY_FIND_VISITS_TO_EXPIRE: {
    sql: `INSERT INTO expiration_notify
            (v_id, url, guid, visit_date, expected_results)
          SELECT v.id, h.url, h.guid, v.visit_date, :limit_visits
          FROM moz_historyvisits v
          JOIN moz_places h ON h.id = v.place_id
          WHERE (SELECT COUNT(*) FROM moz_places) > :max_uris
          AND visit_date < strftime('%s','now','localtime','start of day','-7 days','utc') * 1000000
          ORDER BY v.visit_date ASC
          LIMIT :limit_visits`,
    actions:
      ACTION.TIMED_OVERLIMIT |
      ACTION.IDLE_DIRTY |
      ACTION.IDLE_DAILY |
      ACTION.DEBUG,
  },

  QUERY_EXPIRE_VISITS: {
    sql: `DELETE FROM moz_historyvisits WHERE id IN (
            SELECT v_id FROM expiration_notify WHERE v_id NOTNULL
          )`,
    actions:
      ACTION.TIMED_OVERLIMIT |
      ACTION.IDLE_DIRTY |
      ACTION.IDLE_DAILY |
      ACTION.DEBUG,
  },

  QUERY_FIND_URIS_TO_EXPIRE: {
    sql: `INSERT INTO expiration_notify (p_id, url, guid, visit_date)
          SELECT h.id, h.url, h.guid, h.last_visit_date
          FROM moz_places h
          LEFT JOIN moz_historyvisits v ON h.id = v.place_id
          WHERE h.last_visit_date IS NULL
            AND h.foreign_count = 0
            AND v.id IS NULL
            AND frecency <> -1
          LIMIT :limit_uris`,
    actions:
      ACTION.TIMED |
      ACTION.TIMED_OVERLIMIT |
      ACTION.SHUTDOWN_DIRTY |
      ACTION.IDLE_DIRTY |
      ACTION.IDLE_DAILY |
      ACTION.DEBUG,
  },

  QUERY_EXPIRE_URIS: {
    sql: `DELETE FROM moz_places WHERE id IN (
            SELECT p_id FROM expiration_notify WHERE p_id NOTNULL
          ) AND foreign_count = 0 AND last_visit_date ISNULL`,
    actions:
      ACTION.TIMED |
      ACTION.TIMED_OVERLIMIT |
      ACTION.SHUTDOWN_DIRTY |
      ACTION.IDLE_DIRTY |
      ACTION.IDLE_DAILY |
      ACTION.DEBUG,
  },

  QUERY_UPDATE_HOSTS: {
    sql: `DELETE FROM moz_updateoriginsdelete_temp`,
    actions:
      ACTION.TIMED |
      ACTION.TIMED_OVERLIMIT |
      ACTION.SHUTDOWN_DIRTY |
      ACTION.IDLE_DIRTY |
      ACTION.IDLE_DAILY |
      ACTION.DEBUG,
  },

  QUERY_EXPIRE_OLD_FAVICON_RELATIONS: {
    sql: `
    DELETE FROM moz_icons_to_pages
    WHERE (page_id, icon_id) IN (
      SELECT page_id, icon_id
      FROM moz_icons_to_pages ip
      JOIN moz_pages_w_icons pi ON pi.id = page_id
      JOIN moz_places ON url_hash = page_url_hash
      WHERE
        last_visit_date BETWEEN
          strftime('%s', ip.expire_ms / 1000, 'unixepoch', '+6 months', 'localtime', 'utc') * 1000000
          AND strftime('%s', 'now', 'localtime', '-6 months', 'utc') * 1000000
        AND foreign_count = 0
        AND NOT EXISTS (SELECT 1 FROM moz_icons WHERE id = icon_id AND root = 1)
      LIMIT 50
    )
    `,
    actions: ACTION.IDLE_DIRTY | ACTION.IDLE_DAILY | ACTION.DEBUG,
  },

  QUERY_EXPIRE_OLD_FAVICONS: {
    sql: `
    DELETE FROM moz_pages_w_icons WHERE id IN (
      WITH pages_with_old_relations (page_id, page_url_hash) AS (
        SELECT page_id, page_url_hash
        FROM moz_icons_to_pages ip
        JOIN moz_pages_w_icons p ON p.id = page_id
        GROUP BY page_id
        HAVING max(expire_ms) < strftime('%s','now','localtime','start of day','-180 days','utc') * 1000
      )
      SELECT page_id
      FROM pages_with_old_relations
      JOIN moz_places h ON h.url_hash = page_url_hash
      JOIN moz_origins o ON h.origin_id = o.id
      WHERE foreign_count = 0
      AND EXISTS (
        SELECT 1 FROM moz_icons
        WHERE root = 1
          AND fixed_icon_url_hash = hash(fixup_url(o.host) || '/favicon.ico')
      )
      LIMIT 100
    )`,
    actions: ACTION.IDLE_DIRTY | ACTION.IDLE_DAILY | ACTION.DEBUG,
  },

  QUERY_EXPIRE_FAVICONS_PAGES: {
    sql: `DELETE FROM moz_pages_w_icons
          WHERE page_url_hash NOT IN (
            SELECT url_hash FROM moz_places
          ) OR NOT EXISTS (
            SELECT 1 FROM moz_icons_to_pages WHERE page_id = moz_pages_w_icons.id
          )`,
    actions:
      ACTION.TIMED_OVERLIMIT |
      ACTION.SHUTDOWN_DIRTY |
      ACTION.IDLE_DIRTY |
      ACTION.IDLE_DAILY |
      ACTION.DEBUG,
  },

  QUERY_EXPIRE_FAVICONS: {
    sql: `DELETE FROM moz_icons WHERE id IN (
            SELECT id FROM moz_icons WHERE root = 0
            EXCEPT
            SELECT icon_id FROM moz_icons_to_pages
          )`,
    actions:
      ACTION.TIMED_OVERLIMIT |
      ACTION.SHUTDOWN_DIRTY |
      ACTION.IDLE_DIRTY |
      ACTION.IDLE_DAILY |
      ACTION.DEBUG,
  },

  QUERY_EXPIRE_ANNOS: {
    sql: `DELETE FROM moz_annos WHERE id in (
            SELECT a.id FROM moz_annos a
            LEFT JOIN moz_places h ON a.place_id = h.id
            WHERE h.id IS NULL
            LIMIT :limit_annos
          )`,
    actions:
      ACTION.TIMED |
      ACTION.TIMED_OVERLIMIT |
      ACTION.SHUTDOWN_DIRTY |
      ACTION.IDLE_DIRTY |
      ACTION.IDLE_DAILY |
      ACTION.DEBUG,
  },

  QUERY_EXPIRE_INPUTHISTORY: {
    sql: `DELETE FROM moz_inputhistory
          WHERE place_id IN (SELECT p_id FROM expiration_notify)
          AND place_id IN (
            SELECT i.place_id FROM moz_inputhistory i
            LEFT JOIN moz_places h ON h.id = i.place_id
            WHERE h.id IS NULL
            LIMIT :limit_inputhistory
          )`,
    actions:
      ACTION.TIMED |
      ACTION.TIMED_OVERLIMIT |
      ACTION.SHUTDOWN_DIRTY |
      ACTION.IDLE_DIRTY |
      ACTION.IDLE_DAILY |
      ACTION.DEBUG,
  },

  QUERY_SELECT_NOTIFICATIONS: {
    sql: `/* do not warn (bug no): temp table has no index */
          SELECT url, guid, MAX(visit_date) AS visit_date,
                 MAX(IFNULL(MIN(p_id, 1), MIN(v_id, 0))) AS whole_entry,
                 MAX(expected_results) AS expected_results,
                 (SELECT MAX(visit_date) FROM expiration_notify
                  WHERE reason = "expired" AND url = n.url AND p_id ISNULL
                 ) AS most_recent_expired_visit
          FROM expiration_notify n
          GROUP BY url`,
    actions:
      ACTION.TIMED |
      ACTION.TIMED_OVERLIMIT |
      ACTION.SHUTDOWN_DIRTY |
      ACTION.IDLE_DIRTY |
      ACTION.IDLE_DAILY |
      ACTION.DEBUG,
  },

  QUERY_DELETE_NOTIFICATIONS: {
    sql: "DELETE FROM expiration_notify",
    actions:
      ACTION.TIMED |
      ACTION.TIMED_OVERLIMIT |
      ACTION.SHUTDOWN_DIRTY |
      ACTION.IDLE_DIRTY |
      ACTION.IDLE_DAILY |
      ACTION.DEBUG,
  },

  QUERY_EXPIRE_INTERACTIONS: {
    sql: `DELETE FROM moz_places_metadata
          WHERE id IN (
            SELECT id FROM moz_places_metadata
            WHERE updated_at < strftime('%s','now','localtime','-' || :days_interactions || ' day','start of day','utc') * 1000
            ORDER BY updated_at ASC
            LIMIT :limit_interactions
          )`,
    get disabled() {
      return !Services.prefs.getBoolPref(
        "browser.places.interactions.enabled",
        false
      );
    },
    actions:
      ACTION.TIMED_OVERLIMIT |
      ACTION.SHUTDOWN_DIRTY |
      ACTION.IDLE_DIRTY |
      ACTION.IDLE_DAILY |
      ACTION.DEBUG,
  },
};

export function nsPlacesExpiration() {
  this.wrappedJSObject = this;

  XPCOMUtils.defineLazyServiceGetter(
    this,
    "_idle",
    "@mozilla.org/widget/useridleservice;1",
    Ci.nsIUserIdleService
  );

  XPCOMUtils.defineLazyPreferenceGetter(
    this,
    "maxPages",
    "places.history.expiration.max_pages",
    -1,
    () => {
      dump("max_pages changing\n");
      this._pagesLimit = null;
    }
  );

  XPCOMUtils.defineLazyPreferenceGetter(
    this,
    "intervalSeconds",
    "places.history.expiration.interval_seconds",
    3 * 60, 
    () => {
      this._newTimer();
    },
    v => (v > 0 ? v : 3 * 60) 
  );

  this._dbInitializedPromise = lazy.PlacesUtils.withConnectionWrapper(
    "PlacesExpiration.sys.mjs: setup",
    async db => {
      await db.execute(
        `CREATE TEMP TABLE expiration_notify (
          id INTEGER PRIMARY KEY,
          v_id INTEGER,
          p_id INTEGER,
          url TEXT NOT NULL,
          guid TEXT NOT NULL,
          visit_date INTEGER,
          expected_results INTEGER NOT NULL DEFAULT 0,
          reason TEXT NOT NULL DEFAULT "expired"
        )`
      );
    }
  )
    .then(() => {
      this._newTimer();
      Services.obs.addObserver(this, TOPIC_IDLE_DAILY, true);
    })
    .catch(console.error);

  let shutdownClient =
    lazy.PlacesUtils.history.connectionShutdownClient.jsclient;
  shutdownClient.addBlocker("Places Expiration: shutdown", () => {
    if (this._shuttingDown) {
      return;
    }
    this._shuttingDown = true;
    this.expireOnIdle = false;
    if (this._timer) {
      this._timer.cancel();
      this._timer = null;
    }
    if (this.status == STATUS.DIRTY) {
      this._expire(ACTION.SHUTDOWN_DIRTY, LIMIT.LARGE).catch(console.error);
    }
  });
}

nsPlacesExpiration.prototype = {
  observe(aSubject, aTopic, aData) {
    if (this._shuttingDown) {
      return;
    }

    if (aTopic == TOPIC_DEBUG_START_EXPIRATION) {
      let limit = parseInt(aData);
      if (limit == -1) {
        this._expire(ACTION.DEBUG, LIMIT.UNLIMITED).catch(console.error);
      } else if (limit > 0) {
        this._debugLimit = limit;
        this._expire(ACTION.DEBUG, LIMIT.DEBUG).catch(console.error);
      } else {
        this._debugLimit = -1;
        this._expire(ACTION.DEBUG, LIMIT.DEBUG).catch(console.error);
      }
    } else if (aTopic == TOPIC_IDLE_BEGIN) {
      if (this._timer) {
        this._timer.cancel();
        this._timer = null;
      }
      if (this.expireOnIdle) {
        this._expire(ACTION.IDLE_DIRTY, LIMIT.LARGE).catch(console.error);
      }
    } else if (aTopic == TOPIC_IDLE_END) {
      if (!this._timer) {
        this._newTimer();
      }
    } else if (aTopic == TOPIC_IDLE_DAILY) {
      this._expire(ACTION.IDLE_DAILY, LIMIT.LARGE).catch(console.error);
    } else if (aTopic == TOPIC_TESTING_MODE) {
      this._testingMode = true;
    } else if (aTopic == lazy.PlacesUtils.TOPIC_INIT_COMPLETE) {
      this._placesObserver = new PlacesWeakCallbackWrapper(
        () => {
          this.status = STATUS.CLEAN;
        }
      );
      PlacesObservers.addListener(["history-cleared"], this._placesObserver);
    }
  },


  name: "nsPlacesExpiration",


  notify() {
    Services.tm.idleDispatchToMainThread(async () => {
      let db = await lazy.PlacesUtils.promiseDBConnection();
      let pagesCount = (
        await db.executeCached("SELECT count(*) AS count FROM moz_places")
      )[0].getResultByName("count");
      let pagesLimit = await this.getPagesLimit();
      let overLimitPages = pagesCount - pagesLimit;
      let action = overLimitPages > 0 ? ACTION.TIMED_OVERLIMIT : ACTION.TIMED;
      let limit =
        overLimitPages > OVERLIMIT_PAGES_THRESHOLD ? LIMIT.LARGE : LIMIT.SMALL;
      this._expire(action, limit).catch(console.error);
    }, 300000);
  },

  _handleQueryResultAndAddNotification(row, notifications) {
    if (this._shuttingDown) {
      return;
    }

    let expectedResults = row.getResultByName("expected_results");
    if (expectedResults > 0) {
      if (!("_expectedResultsCount" in this)) {
        // @ts-ignore - Bug 1965966 this is dynamically created/deleted.
        this._expectedResultsCount = expectedResults;
      }
      if (this._expectedResultsCount > 0) {
        this._expectedResultsCount--;
      }
    }

    let uri = Services.io.newURI(row.getResultByName("url"));
    let guid = row.getResultByName("guid");
    let visitDate = row.getResultByName("visit_date");
    let wholeEntry = row.getResultByName("whole_entry");
    let mostRecentExpiredVisit = row.getResultByName(
      "most_recent_expired_visit"
    );

    if (mostRecentExpiredVisit) {
      let days = Math.floor(
        (Date.now() - mostRecentExpiredVisit / 1000) / MSECS_PER_DAY
      );
      if (!this._mostRecentExpiredVisitDays) {
        this._mostRecentExpiredVisitDays = days;
      } else if (days < this._mostRecentExpiredVisitDays) {
        this._mostRecentExpiredVisitDays = days;
      }
    }

    const isRemovedFromStore = !!wholeEntry;
    notifications.push(
      new PlacesVisitRemoved({
        url: uri.spec,
        pageGuid: guid,
        reason: PlacesVisitRemoved.REASON_EXPIRED,
        isRemovedFromStore,
        isPartialVisistsRemoval: !isRemovedFromStore && visitDate > 0,
      })
    );
  },

  _shuttingDown: false,

  _status: STATUS.UNKNOWN,

  set status(aNewStatus) {
    if (aNewStatus != this._status) {
      this._status = aNewStatus;
      this._newTimer();
      this.expireOnIdle = aNewStatus == STATUS.DIRTY;
    }
  },

  get status() {
    return this._status;
  },

  async getPagesLimit() {
    if (this._pagesLimit != null) {
      return this._pagesLimit;
    }
    // @ts-ignore - maxPages is dynamically added.
    if (this.maxPages >= 0) {
      // @ts-ignore - maxPages is dynamically added.
      return (this._pagesLimit = this.maxPages);
    }

    let optimalDatabaseSize = DATABASE_MAX_SIZE;

    let db;
    try {
      db = await lazy.PlacesUtils.promiseDBConnection();
      if (db) {
        let row = (
          await db.execute(`SELECT * FROM pragma_page_size(),
                                              pragma_page_count(),
                                              pragma_freelist_count(),
                                              (SELECT count(*) FROM moz_places)`)
        )[0];
        let pageSize = row.getResultByIndex(0);
        let pageCount = row.getResultByIndex(1);
        let freelistCount = row.getResultByIndex(2);
        let uriCount = row.getResultByIndex(3);
        let dbSize = (pageCount - freelistCount) * pageSize;
        let avgURISize = Math.ceil(dbSize / uriCount);
        if (avgURISize > URIENTRY_AVG_SIZE * 3) {
          avgURISize = URIENTRY_AVG_SIZE;
        }
        return (this._pagesLimit = Math.ceil(optimalDatabaseSize / avgURISize));
      }
    } catch (ex) {
    }
    return (this._pagesLimit = 100000);
  },

  _isIdleObserver: false,
  _expireOnIdle: false,

  set expireOnIdle(aExpireOnIdle) {
    if (!this._isIdleObserver && !this._shuttingDown) {
      this._idle.addIdleObserver(this, IDLE_TIMEOUT_SECONDS);
      this._isIdleObserver = true;
    } else if (this._isIdleObserver && this._shuttingDown) {
      this._idle.removeIdleObserver(this, IDLE_TIMEOUT_SECONDS);
      this._isIdleObserver = false;
    }

    if (this._debugLimit !== undefined) {
      this._expireOnIdle = false;
    } else {
      this._expireOnIdle = aExpireOnIdle;
    }
  },

  get expireOnIdle() {
    return this._expireOnIdle;
  },

  _telemetrySteps: 1,

  async _expire(aAction, aLimit) {
    if (this._shuttingDown && aAction != ACTION.SHUTDOWN_DIRTY) {
      return;
    }
    await this._dbInitializedPromise;

    try {
      let queriesToRun = [];
      for (let queryType in EXPIRATION_QUERIES) {
        let query = EXPIRATION_QUERIES[queryType];
        if (query.actions & aAction && !query.disabled) {
          let params = await this._getQueryParams(queryType, aLimit, aAction);
          queriesToRun.push({ query, params });
        }
      }

      let notifications = [];
      await lazy.PlacesUtils.withConnectionWrapper(
        "PlacesExpiration.sys.mjs: expire",
        async db => {
          await db.executeTransaction(async () => {
            for (let { query, params } of queriesToRun) {
              await db.executeCached(query.sql, params, row => {
                this._handleQueryResultAndAddNotification(row, notifications);
              });
            }
          });
        }
      );
      if (notifications.length) {
        PlacesObservers.notifyListeners(notifications);
      }
    } catch (ex) {
      console.error(ex);
      return;
    }

    if (this._mostRecentExpiredVisitDays) {
      delete this._mostRecentExpiredVisitDays;
    }

    if ("_expectedResultsCount" in this) {
      let oldStatus = this.status;
      this.status =
        this._expectedResultsCount == 0 ? STATUS.DIRTY : STATUS.CLEAN;

      if (this.status == STATUS.DIRTY) {
        this._telemetrySteps++;
      } else {
        if (oldStatus == STATUS.DIRTY) {
        }
        this._telemetrySteps = 1;
      }

      delete this._expectedResultsCount;
    }

    Services.obs.notifyObservers(
      null,
      lazy.PlacesUtils.TOPIC_EXPIRATION_FINISHED
    );
  },

  async _getQueryParams(aQueryType, aLimit, aAction) {
    let baseLimit;
    switch (aLimit) {
      case LIMIT.UNLIMITED:
        baseLimit = -1;
        break;
      case LIMIT.SMALL:
        baseLimit = EXPIRE_LIMIT_PER_STEP;
        break;
      case LIMIT.LARGE:
        baseLimit =
          EXPIRE_LIMIT_PER_STEP * EXPIRE_LIMIT_PER_LARGE_STEP_MULTIPLIER;
        break;
      case LIMIT.DEBUG:
        baseLimit = this._debugLimit;
        break;
    }
    if (
      this.status == STATUS.DIRTY &&
      aAction != ACTION.DEBUG &&
      baseLimit > 0
    ) {
      baseLimit *= EXPIRE_AGGRESSIVITY_MULTIPLIER;
    }

    switch (aQueryType) {
      case "QUERY_FIND_EXOTIC_VISITS_TO_EXPIRE":
        return {
          limit_visits:
            aLimit == LIMIT.DEBUG && baseLimit == -1 ? 0 : baseLimit,
        };
      case "QUERY_FIND_VISITS_TO_EXPIRE":
        return {
          max_uris: await this.getPagesLimit(),
          limit_visits:
            aLimit == LIMIT.DEBUG && baseLimit == -1 ? 0 : baseLimit,
        };
      case "QUERY_FIND_URIS_TO_EXPIRE":
        return {
          limit_uris: baseLimit,
        };
      case "QUERY_EXPIRE_ANNOS":
        return {
          limit_annos: baseLimit * EXPIRE_AGGRESSIVITY_MULTIPLIER,
        };
      case "QUERY_EXPIRE_INPUTHISTORY":
        return {
          limit_inputhistory: baseLimit,
        };
      case "QUERY_EXPIRE_INTERACTIONS":
        return {
          days_interactions: Services.prefs.getIntPref(
            "browser.places.interactions.expireDays",
            60
          ),
          limit_interactions:
            aLimit == LIMIT.DEBUG && baseLimit == -1 ? 0 : baseLimit,
        };
    }
    return undefined;
  },

  _newTimer() {
    if (this._timer) {
      this._timer.cancel();
    }
    if (this._shuttingDown) {
      return undefined;
    }

    if (!this._isIdleObserver) {
      // @ts-ignore - _idle is lazily instantiated.
      this._idle.addIdleObserver(this, IDLE_TIMEOUT_SECONDS);
      this._isIdleObserver = true;
    }

    // @ts-ignore - this.intervalSeconds is lazily instantiated.
    let seconds = this.intervalSeconds;
    let interval =
      this.status != STATUS.DIRTY
        ? seconds * EXPIRE_AGGRESSIVITY_MULTIPLIER
        : seconds;

    let timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
    timer.initWithCallback(
      this,
      interval * 1000,
      Ci.nsITimer.TYPE_REPEATING_SLACK_LOW_PRIORITY
    );
    if (this._testingMode) {
      Services.obs.notifyObservers(null, TOPIC_TEST_INTERVAL_CHANGED, interval);
    }
    return (this._timer = timer);
  },

  classID: Components.ID("705a423f-2f69-42f3-b9fe-1517e0dee56f"),

  QueryInterface: ChromeUtils.generateQI([
    "nsINamed",
    "nsIObserver",
    "nsISupportsWeakReference",
    "nsITimerCallback",
  ]),
};
