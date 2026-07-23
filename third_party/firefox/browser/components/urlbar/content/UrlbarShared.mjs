/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
});

export const UrlbarShared = {
  NOTIFICATIONS: Object.freeze({
    QUERY_STARTED: "onQueryStarted",
    QUERY_RESULTS: "onQueryResults",
    QUERY_RESULT_REMOVED: "onQueryResultRemoved",
    QUERY_CANCELLED: "onQueryCancelled",
    QUERY_FINISHED: "onQueryFinished",
    VIEW_OPEN: "onViewOpen",
    VIEW_CLOSE: "onViewClose",
  }),

  TOKEN_TYPE: Object.freeze({
    TEXT: 1,
    POSSIBLE_ORIGIN: 2,
    POSSIBLE_URL: 3, 
    RESTRICT_HISTORY: 4,
    RESTRICT_BOOKMARK: 5,
    RESTRICT_TAG: 6,
    RESTRICT_OPENPAGE: 7,
    RESTRICT_SEARCH: 8,
    RESTRICT_TITLE: 9,
    RESTRICT_URL: 10,
    RESTRICT_ACTION: 11,
    POSSIBLE_ORIGIN_BUT_SEARCH_ALLOWED: 12,
  }),

  RESTRICT_TOKENS: Object.freeze({
    HISTORY: "^",
    BOOKMARK: "*",
    TAG: "+",
    OPENPAGE: "%",
    SEARCH: "?",
    TITLE: "#",
    URL: "$",
    ACTION: ">",
  }),

  RESULT_TYPE: Object.freeze({
    TAB_SWITCH: 1,
    SEARCH: 2,
    URL: 3,
    KEYWORD: 4,
    TIP: 7,
    DYNAMIC: 8,
    RESTRICT: 9,

  }),

  RESULT_SOURCE: Object.freeze({
    BOOKMARKS: 1,
    HISTORY: 2,
    SEARCH: 3,
    TABS: 4,
    OTHER_LOCAL: 5,
    OTHER_NETWORK: 6,
    ADDON: 7,
    ACTIONS: 8,
  }),

  get SEARCH_MODE_RESTRICT() {
    const keys = [
      this.RESTRICT_TOKENS.HISTORY,
      this.RESTRICT_TOKENS.BOOKMARK,
      this.RESTRICT_TOKENS.OPENPAGE,
      this.RESTRICT_TOKENS.SEARCH,
    ];
    keys.push(this.RESTRICT_TOKENS.ACTION);
    return new Set(keys);
  },

  getLogger({ prefix = "" } = {}) {
    let logger = loggers.get(prefix);
    if (logger) {
      return logger;
    }

    let fullPrefix = `URLBar${prefix ? " - " + prefix : ""}`;
    if (console.createInstance) {
      logger = createLoggerChrome(fullPrefix);
    } else {
      logger = createLoggerContent(fullPrefix);
    }
    loggers.set(prefix, logger);
    return logger;
  },
};

function createLoggerChrome(prefix) {
  let logger = console.createInstance({
    prefix,
    maxLogLevelPref: "browser.urlbar.loglevel",
  });
  return  ( (logger));
}

function createLoggerContent(prefix) {
  let tag = `[${prefix}]`;
  const LEVEL_NUMBERS = {
    all: 0,
    trace: 1,
    debug: 2,
    log: 3,
    info: 3,
    warn: 4,
    error: 5,
    off: Infinity,
  };
  const LEVELS = ["debug", "log", "info", "trace", "warn", "error"];

  let shouldLog = level => {
    let maxLevel =
      LEVEL_NUMBERS[lazy.UrlbarPrefs.get("loglevel").toLowerCase()] ??
      LEVEL_NUMBERS.warn;
    return maxLevel <= LEVEL_NUMBERS[level];
  };

  return new Proxy(console, {
    get(target, prop) {
      if (typeof prop == "string" && LEVELS.includes(prop)) {
        return (...args) => shouldLog(prop) && target[prop](tag, ...args);
      }

      let value = target[prop];
      return typeof value == "function" ? value.bind(target) : value;
    },
  });
}

const loggers = new Map();
