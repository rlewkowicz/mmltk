/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */



const FRECENCY_DEFAULT = 1000;

const NOTIFYRESULT_DELAY_MS = 16;

const SQL_BOOKMARK_TAGS_FRAGMENT = `
   ( SELECT dateAdded FROM moz_bookmarks WHERE fk = h.id ) AS bookmarkDate,
   ( SELECT title FROM moz_bookmarks WHERE fk = h.id AND title NOTNULL
     ORDER BY lastModified DESC LIMIT 1
   ) AS btitle,
   ( SELECT GROUP_CONCAT(t.title ORDER BY t.title)
     FROM moz_bookmarks b
     JOIN moz_bookmarks t ON t.id = +b.parent AND t.parent = :parent
     WHERE b.fk = h.id
   ) AS tags`;

function defaultQuery(conditions = "") {
  let query = `
     SELECT h.url, h.title, ${SQL_BOOKMARK_TAGS_FRAGMENT}, h.id, t.open_count,
            ${lazy.PAGES_FRECENCY_FIELD} AS frecency, t.userContextId,
            h.last_visit_date, NULLIF(t.groupId, '') groupId
     FROM moz_places h
     LEFT JOIN moz_openpages_temp t
            ON t.url = h.url
            AND (t.userContextId = :userContextId OR (t.userContextId <> -1 AND :userContextId IS NULL))
     WHERE (
        (:switchTabsEnabled AND t.open_count > 0) OR
        ${lazy.PAGES_FRECENCY_FIELD} <> 0
       )
       AND CASE WHEN bookmarkDate
         THEN
           AUTOCOMPLETE_MATCH(:searchString, h.url,
                              IFNULL(btitle, h.title), tags,
                              h.visit_count, h.typed,
                              1, t.open_count,
                              :matchBehavior, :searchBehavior, NULL)
         ELSE
           AUTOCOMPLETE_MATCH(:searchString, h.url,
                              h.title, '',
                              h.visit_count, h.typed,
                              0, t.open_count,
                              :matchBehavior, :searchBehavior, NULL)
         END
       ${conditions ? "AND" : ""} ${conditions}
     ORDER BY ${lazy.PAGES_FRECENCY_FIELD} DESC, h.id DESC
     LIMIT :maxResults`;
  return query;
}

const SQL_SWITCHTAB_QUERY = `
   SELECT t.url, t.url AS title, 0 AS bookmarkDate, NULL AS btitle,
           NULL AS tags, NULL AS id, t.open_count, NULL AS frecency,
           t.userContextId, NULL AS last_visit_date, NULLIF(t.groupId, '') groupId
   FROM moz_openpages_temp t
   LEFT JOIN moz_places h ON h.url_hash = hash(t.url) AND h.url = t.url
   WHERE h.id IS NULL
     AND (t.userContextId = :userContextId OR (t.userContextId <> -1 AND :userContextId IS NULL))
     AND AUTOCOMPLETE_MATCH(:searchString, t.url, t.url, NULL,
                            NULL, NULL, NULL, t.open_count,
                            :matchBehavior, :searchBehavior, NULL)
   ORDER BY t.ROWID DESC
   LIMIT :maxResults`;


import {
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  KeywordUtils: "resource://gre/modules/KeywordUtils.sys.mjs",
  ObjectUtils: "resource://gre/modules/ObjectUtils.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  Sqlite: "resource://gre/modules/Sqlite.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  UrlbarProviderOpenTabs:
    "moz-src:///browser/components/urlbar/UrlbarProviderOpenTabs.sys.mjs",
  ProvidersManager:
    "moz-src:///browser/components/urlbar/UrlbarProvidersManager.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarSearchUtils:
    "moz-src:///browser/components/urlbar/UrlbarSearchUtils.sys.mjs",
  UrlbarTokenizer:
    "moz-src:///browser/components/urlbar/UrlbarTokenizer.sys.mjs",
  PAGES_FRECENCY_FIELD: () => {
    return lazy.PlacesUtils.history.isAlternativeFrecencyEnabled
      ? "alt_frecency"
      : "frecency";
  },
  typeToBehaviorMap: () => {
    return  (
      new Map([
        [lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_HISTORY, "history"],
        [lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_BOOKMARK, "bookmark"],
        [lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_TAG, "tag"],
        [lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_OPENPAGE, "openpage"],
        [lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_SEARCH, "search"],
        [lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_TITLE, "title"],
        [lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_URL, "url"],
      ])
    );
  },
  sourceToBehaviorMap: () => {
    return  (
      new Map([
        [lazy.UrlbarShared.RESULT_SOURCE.HISTORY, "history"],
        [lazy.UrlbarShared.RESULT_SOURCE.BOOKMARKS, "bookmark"],
        [lazy.UrlbarShared.RESULT_SOURCE.TABS, "openpage"],
        [lazy.UrlbarShared.RESULT_SOURCE.SEARCH, "search"],
      ])
    );
  },
});

function setTimeout(callback, ms) {
  let timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
  timer.initWithCallback(callback, ms, timer.TYPE_ONE_SHOT);
  return timer;
}


function makeMapKeyForResult(url, match) {
  let action = lazy.PlacesUtils.parseActionUrl(match.value);
  return UrlbarUtils.tupleString(
    url,
    action?.type == "switchtab" &&
      lazy.UrlbarProviderOpenTabs.isNonPrivateUserContextId(match.userContextId)
      ? match.userContextId
      : undefined
  );
}

function makeKeyForMatch(match) {
  let key, prefix;
  let action = lazy.PlacesUtils.parseActionUrl(match.value);
  if (!action) {
    [key, prefix] = UrlbarUtils.stripPrefixAndTrim(match.value, {
      stripHttp: true,
      stripHttps: true,
      stripWww: true,
      trimSlash: true,
      trimEmptyQuery: true,
      trimEmptyHash: true,
    });
    return [makeMapKeyForResult(key, match), prefix, null];
  }

  switch (action.type) {
    case "searchengine":
      key = [
        action.type,
        action.params.engineName,
        (
          action.params.searchSuggestion || action.params.searchQuery
        ).toLocaleLowerCase(),
      ].join(",");
      break;
    default:
      [key, prefix] = UrlbarUtils.stripPrefixAndTrim(
        action.params.url || match.value,
        {
          stripHttp: true,
          stripHttps: true,
          stripWww: true,
          trimEmptyQuery: true,
          trimSlash: true,
        }
      );
      break;
  }
  let resKey = makeMapKeyForResult(key, match);
  return [resKey, prefix, action];
}

function makeActionUrl(type, params) {
  let encodedParams = {};
  for (let key in params) {
    if (params[key] === null || params[key] === undefined) {
      continue;
    }
    encodedParams[key] = encodeURIComponent(params[key]);
  }
  return `moz-action:${type},${JSON.stringify(encodedParams)}`;
}

function convertLegacyMatches(context, matches, urls) {
  let results = [];
  for (let match of matches) {
    let url = match.finalCompleteValue || match.value;
    if (urls.has(makeMapKeyForResult(url, match))) {
      continue;
    }
    urls.add(makeMapKeyForResult(url, match));
    let result = makeUrlbarResult(context, {
      url,
      icon: match.icon || undefined,
      style: match.style,
      title: match.comment,
      userContextId: match.userContextId,
      lastVisit: match.lastVisit,
      bookmarkDateMs: match.bookmarkDateMs,
      tabGroup: match.tabGroup,
      frecency: match.frecency,
    });
    if (!result) {
      continue;
    }

    results.push(result);
  }
  return results;
}

function makeUrlbarResult(queryContext, info) {
  let action = lazy.PlacesUtils.parseActionUrl(info.url);
  if (action) {
    switch (action.type) {
      case "searchengine":
        return null;
      case "switchtab": {
        return new lazy.UrlbarResult({
          type: lazy.UrlbarShared.RESULT_TYPE.TAB_SWITCH,
          source: lazy.UrlbarShared.RESULT_SOURCE.TABS,
          payload: {
            url: action.params.url,
            title: info.title,
            icon: info.icon,
            userContextId: info.userContextId,
            lastVisit: info.lastVisit,
            bookmarkDateMs: info.bookmarkDateMs,
            tabGroup: info.tabGroup,
            frecency: info.frecency,
            action: lazy.UrlbarPrefs.get("secondaryActions.switchToTab")
              ? UrlbarUtils.createTabSwitchSecondaryAction(info.userContextId)
              : undefined,
          },
          highlights: {
            url: UrlbarUtils.HIGHLIGHT.TYPED,
            title: UrlbarUtils.HIGHLIGHT.TYPED,
          },
        });
      }
      default:
        console.error(`Unexpected action type: ${action.type}`);
        return null;
    }
  }

  let source;
  let tags = [];
  let title = info.title;
  let isBlockable;
  let blockL10n;
  let helpUrl;

  if (info.style.includes("bookmark")) {
    source = lazy.UrlbarShared.RESULT_SOURCE.BOOKMARKS;
  } else {
    source = lazy.UrlbarShared.RESULT_SOURCE.HISTORY;
    isBlockable = true;
    blockL10n = { id: "urlbar-result-menu-remove-from-history2" };
    helpUrl =
      Services.urlFormatter.formatURLPref("app.support.baseURL") +
      "awesome-bar-result-menu";
  }

  if (info.style.includes("tag")) {
    let titleTags;
    [title, titleTags] = info.title.split(UrlbarUtils.TITLE_TAGS_SEPARATOR);

    if (source != lazy.UrlbarShared.RESULT_SOURCE.BOOKMARKS) {
      titleTags = "";
    }

    tags = titleTags.split(",").filter(tag => {
      let lowerCaseTag = tag.toLocaleLowerCase();
      return queryContext.tokens.some(token =>
        lowerCaseTag.includes(token.lowerCaseValue)
      );
    });
  }

  return new lazy.UrlbarResult({
    type: lazy.UrlbarShared.RESULT_TYPE.URL,
    source,
    payload: {
      url: info.url,
      icon: info.icon,
      title,
      tags,
      isBlockable,
      blockL10n,
      helpUrl,
      lastVisit: info.lastVisit,
      bookmarkDateMs: info.bookmarkDateMs,
      frecency: info.frecency,
    },
    highlights: {
      url: UrlbarUtils.HIGHLIGHT.TYPED,
      title: UrlbarUtils.HIGHLIGHT.TYPED,
      tags: UrlbarUtils.HIGHLIGHT.TYPED,
    },
  });
}

const MATCH_TYPE = Object.freeze({
  HEURISTIC: "heuristic",
  GENERAL: "general",
});

class Search {
  constructor(queryContext, listener, provider) {
    this.#originalSearchString = queryContext.searchString;
    this.#trimmedOriginalSearchString = queryContext.trimmedSearchString;
    let unescapedSearchString = UrlbarUtils.unEscapeURIForUI(
      this.#trimmedOriginalSearchString
    );
    let prefix, suffix;
    if (unescapedSearchString.startsWith("about:")) {
      prefix = "";
      suffix = unescapedSearchString;
    } else {
      [prefix, suffix] = UrlbarUtils.stripURLPrefix(unescapedSearchString);
    }
    this.#searchString = suffix;

    this.#behavior = this.#searchString
      ? lazy.UrlbarPrefs.get("defaultBehavior")
      : this.#emptySearchDefaultBehavior;

    this.#inPrivateWindow = queryContext.isPrivate;
    this.#maxResults = Math.round(queryContext.maxResults * 1.5);
    this.#userContextId = queryContext.userContextId;
    this.#currentPage = queryContext.currentPage;
    this.#searchModeEngine = queryContext.searchMode?.engineName;
    if (this.#searchModeEngine && queryContext.restrictInSearchMode()) {
      let engine = lazy.SearchService.getEngineByName(this.#searchModeEngine);
      this.#filterOnHost = engine.searchUrlDomain;
    }

    let tokens = lazy.UrlbarTokenizer.tokenize({
      searchString: unescapedSearchString,
      trimmedSearchString: unescapedSearchString.trim(),
    });

    this.#leadingRestrictionToken = null;
    if (tokens.length) {
      if (
        lazy.UrlbarTokenizer.isRestrictionToken(tokens[0]) &&
        (tokens.length > 1 ||
          tokens[0].type == lazy.UrlbarShared.TOKEN_TYPE.RESTRICT_SEARCH)
      ) {
        this.#leadingRestrictionToken = tokens[0].value;
      }

      if (
        prefix &&
        prefix != "about:" &&
        tokens[0].value.length > prefix.length
      ) {
        tokens[0].value = tokens[0].value.substring(prefix.length);
      }
    }

    this.#searchTokens =
      !queryContext || queryContext.restrictToken
        ? this.filterTokens(tokens)
        : tokens;

    if (
      queryContext &&
      queryContext.restrictSource &&
      lazy.sourceToBehaviorMap.has(queryContext.restrictSource)
    ) {
      this.#behavior = 0;
      this.setBehavior("restrict");
      let behavior = lazy.sourceToBehaviorMap.get(queryContext.restrictSource);
      this.setBehavior(behavior);

      this.#heuristicToken = null;
    } else {
      let firstToken =
        !!this.#searchTokens.length && this.#searchTokens[0].value;
      this.#heuristicToken =
        firstToken && this.#trimmedOriginalSearchString.startsWith(firstToken)
          ? firstToken
          : null;
    }

    if (!lazy.UrlbarPrefs.get("filter.javascript")) {
      this.setBehavior("javascript");
    }

    this.#listener = listener;
    this.#provider = provider;
    this.#queryContext = queryContext;
  }

  setBehavior(type) {
    type = type.toUpperCase();
    this.#behavior |= Ci.mozIPlacesAutoComplete["BEHAVIOR_" + type];
  }

  hasBehavior(type) {
    let behavior = Ci.mozIPlacesAutoComplete["BEHAVIOR_" + type.toUpperCase()];
    return !!(this.#behavior & behavior);
  }

  filterTokens(tokens) {
    let foundToken = false;
    let filtered = [];
    for (let token of tokens) {
      if (!lazy.UrlbarTokenizer.isRestrictionToken(token)) {
        filtered.push(token);
        continue;
      }
      let behavior = lazy.typeToBehaviorMap.get(token.type);
      if (!behavior) {
        throw new Error(`Unknown token type ${token.type}`);
      }
      if (!foundToken) {
        foundToken = true;
        this.#behavior = 0;
        this.setBehavior("restrict");
      }
      this.setBehavior(behavior);
      if (behavior == "tag") {
        this.setBehavior("bookmark");
      }
    }
    return filtered;
  }

  stop() {
    if (!this.pending) {
      return;
    }
    if (this.#notifyTimer) {
      this.#notifyTimer.cancel();
    }
    this.#notifyDelaysCount = 0;
    if (typeof this.#interrupt == "function") {
      this.#interrupt();
    }
    this.pending = false;
  }

  pending = true;

  async execute(conn) {
    if (!this.pending) {
      return;
    }

    this.#interrupt = () => {
      if (!lazy.ProvidersManager.interruptLevel) {
        conn.interrupt();
      }
    };


    let tokenAliasEngines = await lazy.UrlbarSearchUtils.tokenAliasEngines();
    if (this.#trimmedOriginalSearchString == "@" && tokenAliasEngines.length) {
      this.#provider.finishSearch(true);
      return;
    }

    this.#firstTokenIsKeyword =
      this.#firstTokenIsKeyword || (await this.#checkIfFirstTokenIsKeyword());
    if (!this.pending) {
      return;
    }

    if (this.#trimmedOriginalSearchString) {
      let emptySearchRestriction =
        this.#trimmedOriginalSearchString.length <= 3 &&
        this.#leadingRestrictionToken ==
          lazy.UrlbarShared.RESTRICT_TOKENS.SEARCH &&
        /\s*\S?$/.test(this.#trimmedOriginalSearchString);
      if (
        emptySearchRestriction ||
        (tokenAliasEngines.length &&
          this.#trimmedOriginalSearchString.startsWith("@")) ||
        (this.hasBehavior("search") && this.hasBehavior("restrict"))
      ) {
        this.#provider.finishSearch(true);
        return;
      }
    }

    let queries = [];
    if (this.hasBehavior("openpage")) {
      await lazy.UrlbarProviderOpenTabs.promiseDBPopulated;
      if (!this.pending) {
        return;
      }
      queries.push(this.#switchToTabQuery);
    }
    queries.push(this.#searchQuery);
    for (let [query, params] of queries) {
      await conn.executeCached(query, params, this.#onResultRow.bind(this));
      if (!this.pending) {
        return;
      }
    }

    let count = this.#counts[MATCH_TYPE.GENERAL];
    if (count < this.#maxResults) {
      this.#matchBehavior = Ci.mozIPlacesAutoComplete.MATCH_ANYWHERE;
      queries = [this.#searchQuery];
      if (this.hasBehavior("openpage")) {
        queries.unshift(this.#switchToTabQuery);
      }
      for (let [query, params] of queries) {
        await conn.executeCached(query, params, this.#onResultRow.bind(this));
        if (!this.pending) {
          return;
        }
      }
    }
  }

  #counts = Object.values(MATCH_TYPE).reduce((o, p) => {
    o[p] = 0;
    return o;
  },  ({}));

  #behavior;
  #matchBehavior = Ci.mozIPlacesAutoComplete.MATCH_BOUNDARY;

  #maxResults;

  #originalSearchString;
  #searchString;
  #trimmedOriginalSearchString;

  #currentPage;
  #filterOnHost;
  #firstTokenIsKeyword;
  #groups;
  #heuristicToken;
  #inPrivateWindow;
  #interrupt;
  #leadingRestrictionToken;
  #listener;
  #matches = [];
  #provider;
  #searchModeEngine;
  #searchTokens;
  #userContextId;
  #queryContext;

  #usedURLs = [];

  #usedPlaceIds = new Set();

  async #checkIfFirstTokenIsKeyword() {
    if (!this.#heuristicToken) {
      return false;
    }

    let aliasEngine = await lazy.UrlbarSearchUtils.engineForAlias(
      this.#heuristicToken,
      this.#originalSearchString
    );

    if (aliasEngine) {
      return true;
    }

    let { entry } = await lazy.KeywordUtils.getBindableKeyword(
      this.#heuristicToken,
      this.#originalSearchString
    );
    if (entry) {
      this.#filterOnHost = entry.url.host;
      return true;
    }

    return false;
  }

  #onResultRow(row, cancel) {
    this.#addFilteredQueryMatch(row);

    let count = this.#counts[MATCH_TYPE.GENERAL];
    if (!this.pending || count >= this.#maxResults) {
      cancel();
    }
  }

  #maybeRestyleSearchMatch(match) {
    let historyUrl = match.value;
    let parseResult = lazy.SearchService.parseSubmissionURL(historyUrl);
    if (!parseResult?.engine) {
      return false;
    }

    let terms = parseResult.terms.toLowerCase();
    if (
      this.#searchTokens.length &&
      this.#searchTokens.every(token => !terms.includes(token.value))
    ) {
      return false;
    }

    let [generatedSuggestionUrl] = UrlbarUtils.getSearchQueryUrl(
      parseResult.engine,
      this.#searchTokens.map(t => t.value).join(" ")
    );

    if (
      !lazy.UrlbarSearchUtils.serpsAreEquivalent(
        historyUrl,
        generatedSuggestionUrl,
        [parseResult.termsParameterName]
      )
    ) {
      return false;
    }

    match.value = makeActionUrl("searchengine", {
      engineName: parseResult.engine.name,
      input: parseResult.terms,
      searchSuggestion: parseResult.terms,
      searchQuery: parseResult.terms,
      isSearchHistory: true,
    });
    match.comment = parseResult.engine.name;
    match.icon = match.icon || match.iconUrl;
    match.style = "action searchengine favicon suggestion";
    return true;
  }

  #addMatch(match) {
    if (typeof match.frecency != "number") {
      throw new Error("Frecency not provided");
    }

    if (typeof match.type != "string") {
      match.type = MATCH_TYPE.GENERAL;
    }

    if (!this.pending) {
      return;
    }

    match.style = match.style || "favicon";

    if (
      match.style == "favicon" &&
      (lazy.UrlbarPrefs.get("restyleSearches") || this.#searchModeEngine)
    ) {
      this.#maybeRestyleSearchMatch(match);
    }

    match.icon = match.icon || "";
    match.finalCompleteValue = match.finalCompleteValue || "";

    let { index, replace } = this.#getInsertIndexForMatch(match);
    if (index == -1) {
      return;
    }
    if (replace) {
      this.#matches.splice(index, 1);
    }
    this.#matches.splice(index, 0, match);
    this.#counts[match.type]++;

    this.notifyResult(true);
  }


  #getInsertIndexForMatch(match) {
    let [urlMapKey, prefix, action] = makeKeyForMatch(match);
    if (
      (match.placeId &&
        this.#usedPlaceIds.has(makeMapKeyForResult(match.placeId, match))) ||
      this.#usedURLs.some(e => lazy.ObjectUtils.deepEqual(e.key, urlMapKey))
    ) {
      let isDupe = true;
      if (action && ["switchtab", "remotetab"].includes(action.type)) {
        for (let i = 0; i < this.#usedURLs.length; ++i) {
          let { key: matchKey, action: matchAction } = this.#usedURLs[i];
          if (lazy.ObjectUtils.deepEqual(matchKey, urlMapKey)) {
            isDupe = true;
            if (!matchAction || action.type == "switchtab") {
              this.#usedURLs[i] = {
                key: urlMapKey,
                action,
                type: match.type,
                prefix,
                comment: match.comment,
              };
              return { index: i, replace: true };
            }
            break; 
          }
        }
      } else {
        let prefixRank = UrlbarUtils.getPrefixRank(prefix);
        for (let i = 0; i < this.#usedURLs.length; ++i) {
          if (!this.#usedURLs[i]) {
            continue;
          }

          let { key: existingKey, prefix: existingPrefix } = this.#usedURLs[i];

          let existingPrefixRank = UrlbarUtils.getPrefixRank(existingPrefix);
          if (lazy.ObjectUtils.deepEqual(existingKey, urlMapKey)) {
            isDupe = true;

            if (prefix == existingPrefix) {
              break;
            }

            if (prefix.endsWith("www.") == existingPrefix.endsWith("www.")) {
              if (prefixRank <= existingPrefixRank) {
                break; 
              } else {
                this.#usedURLs[i] = {
                  key: urlMapKey,
                  action,
                  type: match.type,
                  prefix,
                  comment: match.comment,
                };
                return { index: i, replace: true };
              }
            } else {
              isDupe = false;
              continue;
            }
          }
        }
      }

      if (isDupe) {
        return { index: -1, replace: false };
      }
    }

    if (match.placeId) {
      this.#usedPlaceIds.add(makeMapKeyForResult(match.placeId, match));
    }

    let index = 0;
    if (!this.#groups) {
      this.#groups = [];
      this.#makeGroups(
        lazy.UrlbarPrefs.getResultGroups({ context: this.#queryContext }),
        this.#maxResults
      );
    }

    let replace = false;
    for (let group of this.#groups) {
      if (match.type != group.type || !group.available) {
        index += group.count;
        continue;
      }

      index += group.insertIndex;
      group.available--;
      if (group.insertIndex < group.count) {
        replace = true;
      } else {
        group.count++;
      }
      group.insertIndex++;
      break;
    }
    this.#usedURLs[index] = {
      key: urlMapKey,
      action,
      type: match.type,
      prefix,
      comment: match.comment || "",
    };
    return { index, replace };
  }

  #makeGroups(resultGroup, maxResultCount) {
    if (!resultGroup.children) {
      let type;
      switch (resultGroup.group) {
        case UrlbarUtils.RESULT_GROUP.HEURISTIC_AUTOFILL:
        case UrlbarUtils.RESULT_GROUP.HEURISTIC_FALLBACK:
        case UrlbarUtils.RESULT_GROUP.HEURISTIC_TEST:
        case UrlbarUtils.RESULT_GROUP.HEURISTIC_TOKEN_ALIAS_ENGINE:
          type = MATCH_TYPE.HEURISTIC;
          break;
        default:
          type = MATCH_TYPE.GENERAL;
          break;
      }
      if (this.#groups.length) {
        let last = this.#groups[this.#groups.length - 1];
        if (last.type == type) {
          return;
        }
      }
      this.#groups.push({
        type,
        available: maxResultCount,
        insertIndex: 0,
        count: 0,
      });
      return;
    }

    let initialMaxResultCount;
    if (typeof resultGroup.maxResultCount == "number") {
      initialMaxResultCount = resultGroup.maxResultCount;
    } else if (typeof resultGroup.availableSpan == "number") {
      initialMaxResultCount = resultGroup.availableSpan;
    } else {
      initialMaxResultCount = this.#maxResults;
    }
    let childMaxResultCount = Math.min(initialMaxResultCount, maxResultCount);
    for (let child of resultGroup.children) {
      this.#makeGroups(child, childMaxResultCount);
    }
  }

  #addFilteredQueryMatch(row) {
    let placeId = row.getResultByName("id");
    let url = row.getResultByName("url");
    let openPageCount = row.getResultByName("open_count") || 0;
    let historyTitle = row.getResultByName("title") || "";

    let bookmarkDatePRTime = row.getResultByName("bookmarkDate");
    let bookmarkTitle = bookmarkDatePRTime
      ? row.getResultByName("btitle")
      : null;
    let bookmarkDateMs = bookmarkDatePRTime
      ? lazy.PlacesUtils.toDate(bookmarkDatePRTime).getTime()
      : undefined;
    let tags = row.getResultByName("tags") || "";
    let frecency = row.getResultByName("frecency");
    let userContextId = row.getResultByName("userContextId");
    let lastVisitPRTime = row.getResultByName("last_visit_date");
    let lastVisit = lastVisitPRTime
      ? lazy.PlacesUtils.toDate(lastVisitPRTime).getTime()
      : undefined;
    let tabGroup = row.getResultByName("groupId");

    let match = {
      placeId,
      value: url,
      comment: bookmarkTitle || historyTitle,
      icon: UrlbarUtils.getIconForUrl(url),
      frecency: frecency || FRECENCY_DEFAULT,
      userContextId,
      lastVisit,
      tabGroup,
      bookmarkDateMs,
    };
    if (openPageCount > 0 && this.hasBehavior("openpage")) {
      if (
        this.#currentPage == match.value &&
        this.#userContextId == match.userContextId
      ) {
        return;
      }
      match.value = makeActionUrl("switchtab", { url: match.value });
      match.style = "action switchtab";
    } else if (
      this.hasBehavior("history") &&
      !this.hasBehavior("bookmark") &&
      !tags
    ) {
      match.style = "favicon";
    } else if (tags) {
      match.comment += UrlbarUtils.TITLE_TAGS_SEPARATOR + tags;
      match.style = this.hasBehavior("bookmark") ? "bookmark-tag" : "tag";
    } else if (bookmarkDateMs) {
      match.style = "bookmark";
    }

    this.#addMatch(match);
  }

  get #suggestionPrefQuery() {
    let conditions = [];
    if (this.#filterOnHost) {
      conditions.push("h.rev_host = get_unreversed_host(:host || '.') || '.'");

      if (lazy.UrlbarPrefs.get("restyleSearches") || this.#searchModeEngine) {
        conditions.push(`NOT EXISTS (
          WITH visits(type) AS (
            SELECT visit_type
            FROM moz_historyvisits
            WHERE place_id = h.id
            ORDER BY visit_date DESC
            LIMIT 10 /* limit to the last 10 visits */
          )
          SELECT 1 FROM visits
          WHERE type IN (5,6)
        )`);
      } else {
        conditions.push(`NOT EXISTS (
          WITH visits(id) AS (
            SELECT id
            FROM moz_historyvisits
            WHERE place_id = h.id
            ORDER BY visit_date DESC
            LIMIT 10 /* limit to the last 10 visits */
            )
           SELECT 1
           FROM visits src
           JOIN moz_historyvisits dest ON src.id = dest.from_visit
           WHERE dest.visit_type IN (5,6)
        )`);
        conditions.push("(h.foreign_count > 0 OR h.title NOTNULL)");
      }
    }

    if (
      this.hasBehavior("restrict") ||
      (!this.hasBehavior("openpage") &&
        (!this.hasBehavior("history") || !this.hasBehavior("bookmark")))
    ) {
      if (this.hasBehavior("history")) {
        conditions.push("+h.visit_count > 0");
      }
      if (this.hasBehavior("bookmark")) {
        conditions.push("bookmarkDate");
      }
      if (this.hasBehavior("tag")) {
        conditions.push("tags NOTNULL");
      }
    }

    return defaultQuery(conditions.join(" AND "));
  }

  get #emptySearchDefaultBehavior() {
    let val = Ci.mozIPlacesAutoComplete.BEHAVIOR_RESTRICT;
    if (lazy.UrlbarPrefs.get("suggest.history")) {
      val |= Ci.mozIPlacesAutoComplete.BEHAVIOR_HISTORY;
    } else if (lazy.UrlbarPrefs.get("suggest.bookmark")) {
      val |= Ci.mozIPlacesAutoComplete.BEHAVIOR_BOOKMARK;
    } else {
      val |= Ci.mozIPlacesAutoComplete.BEHAVIOR_OPENPAGE;
    }
    return val;
  }

  get #keywordFilteredSearchString() {
    let tokens = this.#searchTokens.map(t => t.value);
    if (this.#firstTokenIsKeyword) {
      tokens = tokens.slice(1);
    }
    return tokens.join(" ");
  }

  get #searchQuery() {
    let params = {
      parent: lazy.PlacesUtils.tagsFolderId,
      matchBehavior: this.#matchBehavior,
      searchBehavior: this.#behavior,
      searchString: this.#keywordFilteredSearchString,
      maxResults: this.#maxResults,
      switchTabsEnabled: this.hasBehavior("openpage"),
    };
    params.userContextId =
      lazy.UrlbarProviderOpenTabs.getUserContextIdForOpenPagesTable(
        null,
        this.#inPrivateWindow
      );

    if (this.#filterOnHost) {
      params.host = this.#filterOnHost;
    }
    return [this.#suggestionPrefQuery, params];
  }

  get #switchToTabQuery() {
    return [
      SQL_SWITCHTAB_QUERY,
      {
        matchBehavior: this.#matchBehavior,
        searchBehavior: this.#behavior,
        searchString: this.#keywordFilteredSearchString,
        userContextId:
          lazy.UrlbarProviderOpenTabs.getUserContextIdForOpenPagesTable(
            null,
            this.#inPrivateWindow
          ),
        maxResults: this.#maxResults,
      },
    ];
  }

  #notifyTimer = null;

  #notifyDelaysCount = 0;

  notifyResult(searchOngoing) {
    let notify = () => {
      if (!this.pending) {
        return;
      }
      this.#notifyDelaysCount = 0;
      this.#listener(this.#matches, searchOngoing);
      if (!searchOngoing) {
        this.#listener = null;
        this.#provider = null;
        this.stop();
      }
    };
    if (this.#notifyTimer) {
      this.#notifyTimer.cancel();
    }
    if (this.#notifyDelaysCount > 3) {
      notify();
    } else {
      this.#notifyDelaysCount++;
      this.#notifyTimer = setTimeout(notify, NOTIFYRESULT_DELAY_MS);
    }
  }
}

let _promiseDatabase = null;

export class UrlbarProviderPlaces extends UrlbarProvider {
  #deferred = null;
  #currentSearch = null;

  get type() {
    return UrlbarUtils.PROVIDER_TYPE.PROFILE;
  }

  getDatabaseHandle() {
    if (!_promiseDatabase) {
      _promiseDatabase = (async () => {
        let conn = await lazy.PlacesUtils.promiseLargeCacheDBConnection();

        lazy.Sqlite.shutdown.addBlocker("UrlbarProviderPlaces closing", () => {
          this.#currentSearch = null;
        });

        return conn;
      })().catch(ex => {
        dump("Couldn't get database handle: " + ex + "\n");
        this.logger.error(ex);
      });
    }
    return _promiseDatabase;
  }

  async isActive(queryContext) {
    if (
      !queryContext.trimmedSearchString &&
      queryContext.searchMode?.engineName
    ) {
      return false;
    }
    return true;
  }

  startQuery(queryContext, addCallback) {
    let instance = this.queryInstance;
    let urls = new Set();
    this.#startLegacyQuery(queryContext, matches => {
      if (instance != this.queryInstance) {
        return;
      }
      let results = convertLegacyMatches(queryContext, matches, urls);
      for (let result of results) {
        addCallback(this, result);
      }
    });
    return this.#deferred.promise;
  }

  cancelQuery() {
    if (this.#currentSearch) {
      this.#currentSearch.stop();
    }
    if (this.#deferred) {
      this.#deferred.resolve();
    }
    this.finishSearch();
  }

  finishSearch(notify = false) {
    let search = this.#currentSearch;
    if (!search) {
      return;
    }

    if (!notify || !search.pending) {
      return;
    }

    search.notifyResult(false);
  }

  onEngagement(queryContext, controller, details) {
    let { result } = details;
    if (details.selType == "dismiss") {
      switch (result.type) {
        case lazy.UrlbarShared.RESULT_TYPE.SEARCH: {
          let { url } = UrlbarUtils.getUrlFromResult(result);
          lazy.PlacesUtils.history.remove(url).catch(console.error);
          controller.removeResult(result);
          break;
        }
        case lazy.UrlbarShared.RESULT_TYPE.URL:
          lazy.PlacesUtils.history
            .remove(result.payload.url)
            .catch(console.error);
          controller.removeResult(result);
          break;
      }
    }
  }

  #startLegacyQuery(queryContext, callback) {
    let deferred = Promise.withResolvers();
    let listener = (matches, searchOngoing) => {
      callback(matches);
      if (!searchOngoing) {
        deferred.resolve();
      }
    };
    this.#startSearch(queryContext.searchString, listener, queryContext);
    this.#deferred = deferred;
  }

  #startSearch(searchString, listener, queryContext) {
    if (this.#currentSearch) {
      this.cancelQuery();
    }

    let search = (this.#currentSearch = new Search(
      queryContext,
      listener,
      this
    ));
    this.getDatabaseHandle()
      .then(conn => search.execute(conn))
      .catch(ex => {
        dump(`Query failed: ${ex}\n`);
        this.logger.error(ex);
      })
      .then(() => {
        if (search == this.#currentSearch) {
          this.finishSearch(true);
        }
      });
  }
}
