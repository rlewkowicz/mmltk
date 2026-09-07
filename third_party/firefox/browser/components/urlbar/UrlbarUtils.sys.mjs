/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */



import { UrlbarShared } from "chrome://browser/content/urlbar/UrlbarShared.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  BrowserUIUtils: "resource:///modules/BrowserUIUtils.sys.mjs",
  ContextualIdentityService:
    "moz-src:///toolkit/components/contextualidentity/ContextualIdentityService.sys.mjs",
  FaviconUtils: "moz-src:///toolkit/modules/FaviconUtils.sys.mjs",
  KeywordUtils: "resource://gre/modules/KeywordUtils.sys.mjs",
  PlacesUIUtils: "moz-src:///browser/components/places/PlacesUIUtils.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  SearchEngine: "moz-src:///toolkit/components/search/SearchEngine.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarProviderOpenTabs:
    "moz-src:///browser/components/urlbar/UrlbarProviderOpenTabs.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarSearchUtils:
    "moz-src:///browser/components/urlbar/UrlbarSearchUtils.sys.mjs",
  UrlUtils: "resource://gre/modules/UrlUtils.sys.mjs",
  clearTimeout: "resource://gre/modules/Timer.sys.mjs",
  setTimeout: "resource://gre/modules/Timer.sys.mjs",

  historyEnabled: {
    pref: "places.history.enabled",
    default: true,
  },
});


function parseOriginParts(url) {
  let parsed = URL.parse(url);
  if (!parsed) {
    return null;
  }
  return { prefix: parsed.protocol + "//", host: parsed.host };
}

export var UrlbarUtils = {
  RESULT_GROUP: Object.freeze({
    ABOUT_PAGES: "aboutPages",
    GENERAL: "general",
    HEURISTIC_AUTOFILL: "heuristicAutofill",
    HEURISTIC_ENGINE_ALIAS: "heuristicEngineAlias",
    HEURISTIC_FALLBACK: "heuristicFallback",
    HEURISTIC_BOOKMARK_KEYWORD: "heuristicBookmarkKeyword",
    HEURISTIC_HISTORY_URL: "heuristicHistoryUrl",
    HEURISTIC_RESTRICT_KEYWORD_AUTOFILL: "heuristicRestrictKeywordAutofill",
    HEURISTIC_TEST: "heuristicTest",
    HEURISTIC_TOKEN_ALIAS_ENGINE: "heuristicTokenAliasEngine",
    RESTRICT_SEARCH_KEYWORD: "restrictSearchKeyword",
    SUGGESTED_INDEX: "suggestedIndex",
  }),

  PROVIDER_TYPE: Object.freeze({
    HEURISTIC: 1,
    PROFILE: 2,
    NETWORK: 3,
  }),

  ICON: {
    HISTORY: "chrome://browser/skin/history.svg",
    SEARCH_GLASS: "chrome://global/skin/icons/search-glass.svg",
    TIP: "chrome://global/skin/icons/lightbulb.svg",
    GLOBE: "chrome://global/skin/icons/defaultFavicon.svg",
  },

  PAGE_UP_DOWN_DELTA: 5,

  COMPOSITION: {
    NONE: 1,
    COMPOSING: 2,
    COMMIT: 3,
    CANCELED: 4,
  },

  MAX_TEXT_LENGTH: 255,

  HIGHLIGHT: Object.freeze({
    TYPED: 1,
    SUGGESTED: 2,
    ALL: 3,
  }),

  TITLE_TAGS_SEPARATOR: "\x1F",

  REGEXP_SINGLE_WORD: /^[^\s@:/?#]+(:\d+)?$/,

  SEARCH_MODE_ENTRY: new Set([
    "bookmarkmenu",
    "handoff",
    "keywordoffer",
    "messagingSystem",
    "oneoff",
    "historymenu",
    "other",
    "searchbutton",
    "shortcut",
    "tabmenu",
    "tabtosearch",
    "tabtosearch_onboard",
    "topsites_newtab",
    "topsites_urlbar",
    "touchbar",
    "typed",
  ]),

  PROTOCOLS_WITH_ICONS: ["about:", "http:", "https:", "file:"],

  PROTOCOLS_WITHOUT_AUTHORITY: [
    "about:",
    "data:",
    "file:",
    "javascript:",
    "view-source:",
  ],

  get LOCAL_SEARCH_MODES() {
    let modes =  ([
      {
        source: UrlbarShared.RESULT_SOURCE.BOOKMARKS,
        restrict: UrlbarShared.RESTRICT_TOKENS.BOOKMARK,
        icon: "chrome://browser/skin/bookmark.svg",
        pref: "shortcuts.bookmarks",
        telemetryLabel: "bookmarks",
        uiLabel: "urlbar-searchmode-bookmarks3",
      },
      {
        source: UrlbarShared.RESULT_SOURCE.TABS,
        restrict: UrlbarShared.RESTRICT_TOKENS.OPENPAGE,
        icon: "chrome://browser/skin/tabs.svg",
        pref: "shortcuts.tabs",
        telemetryLabel: "tabs",
        uiLabel: "urlbar-searchmode-tabs3",
      },
      {
        source: UrlbarShared.RESULT_SOURCE.HISTORY,
        restrict: UrlbarShared.RESTRICT_TOKENS.HISTORY,
        icon: "chrome://browser/skin/history.svg",
        pref: "shortcuts.history",
        telemetryLabel: "history",
        uiLabel: "urlbar-searchmode-history3",
      },
      {
        source: UrlbarShared.RESULT_SOURCE.ACTIONS,
        restrict: UrlbarShared.RESTRICT_TOKENS.ACTION,
        icon: "chrome://browser/skin/quickactions.svg",
        pref: "shortcuts.actions",
        telemetryLabel: "actions",
        uiLabel: "urlbar-searchmode-actions3",
      },
    ]);
    return AppConstants.MOZ_PLACES
      ? modes
      : modes.filter(mode => mode.source == UrlbarShared.RESULT_SOURCE.ACTIONS);
  },

  getPayloadSchema(type) {
    return this.RESULT_PAYLOAD_SCHEMA[type];
  },

  addToUrlbarHistory(url, window) {
    if (
      AppConstants.MOZ_PLACES &&
      !lazy.PrivateBrowsingUtils.isWindowPrivate(window) &&
      url &&
      !url.includes(" ") &&
      // eslint-disable-next-line no-control-regex
      !/[\x00-\x1F]/.test(url)
    ) {
      lazy.PlacesUIUtils.markPageAsTyped(url);
    }
  },

  async getShortcutOrURIAndPostData(url) {
    let mayInheritPrincipal = false;
    let postData = null;
    let [keyword, param = ""] = url.trim().split(/\s(.+)/, 2);

    if (!keyword) {
      return { url, postData, mayInheritPrincipal };
    }

    let engine = await lazy.SearchService.getEngineByAlias(keyword);
    if (engine) {
      let submission = engine.getSubmission(param, null);
      return {
        url: submission.uri.spec,
        postData: submission.postData,
        mayInheritPrincipal,
      };
    }

    if (!AppConstants.MOZ_PLACES) {
      return { url, postData, mayInheritPrincipal };
    }

    let entry = null;
    try {
      entry = await lazy.PlacesUtils.keywords.fetch(keyword);
    } catch (ex) {
      console.error(`Unable to fetch Places keyword "${keyword}":`, ex);
    }
    if (!entry || !entry.url) {
      return { url, postData, mayInheritPrincipal };
    }

    try {
      [url, postData] = await lazy.KeywordUtils.parseUrlAndPostData(
        entry.url.href,
        entry.postData,
        param
      );
      if (postData) {
        postData = this.getPostDataStream(postData);
      }

      mayInheritPrincipal = true;
    } catch (ex) {
    }

    return { url, postData, mayInheritPrincipal };
  },

  getPostDataStream(
    postDataString,
    type = "application/x-www-form-urlencoded"
  ) {
    let dataStream = Cc["@mozilla.org/io/string-input-stream;1"].createInstance(
      Ci.nsIStringInputStream
    );
    dataStream.setByteStringData(postDataString);

    let mimeStream = Cc[
      "@mozilla.org/network/mime-input-stream;1"
    ].createInstance(Ci.nsIMIMEInputStream);
    mimeStream.addHeader("Content-Type", type);
    mimeStream.setData(dataStream);
    return mimeStream.QueryInterface(Ci.nsIInputStream);
  },

  _compareIgnoringDiacritics: null,

  getTokenMatches(tokens, str, highlightType) {
    if (highlightType == this.HIGHLIGHT.ALL) {
      return [[0, str.length]];
    }

    if (!tokens?.length) {
      return [];
    }

    str = str.substring(0, this.MAX_TEXT_LENGTH).toLocaleLowerCase();
    let hits = new Array(str.length).fill(
      highlightType == this.HIGHLIGHT.SUGGESTED ? 1 : 0
    );
    let compareIgnoringDiacritics;
    for (let i = 0, totalTokensLength = 0; i < tokens.length; i++) {
      const { lowerCaseValue: needle } = tokens[i];

      if (!needle) {
        continue;
      }
      let index = 0;
      let found = false;
      for (;;) {
        index = str.indexOf(needle, index);
        if (index < 0) {
          break;
        }

        if (highlightType == this.HIGHLIGHT.SUGGESTED) {
          let previousSpaceIndex = str.lastIndexOf(" ", index) + 1;
          if (index != previousSpaceIndex) {
            index += needle.length;
            found = true;
            continue;
          }
        }

        hits.fill(
          highlightType == this.HIGHLIGHT.SUGGESTED ? 0 : 1,
          index,
          index + needle.length
        );
        index += needle.length;
        found = true;
      }
      if (!found) {
        if (!compareIgnoringDiacritics) {
          if (!this._compareIgnoringDiacritics) {
            this._compareIgnoringDiacritics = new Intl.Collator("en", {
              sensitivity: "base",
            }).compare;
          }
          compareIgnoringDiacritics = this._compareIgnoringDiacritics;
        }
        index = 0;
        while (index < str.length) {
          let hay = str.substr(index, needle.length);
          if (compareIgnoringDiacritics(needle, hay) === 0) {
            if (highlightType == this.HIGHLIGHT.SUGGESTED) {
              let previousSpaceIndex = str.lastIndexOf(" ", index) + 1;
              if (index != previousSpaceIndex) {
                index += needle.length;
                continue;
              }
            }
            hits.fill(
              highlightType == this.HIGHLIGHT.SUGGESTED ? 0 : 1,
              index,
              index + needle.length
            );
            index += needle.length;
          } else {
            index++;
          }
        }
      }

      totalTokensLength += needle.length;
      if (totalTokensLength > this.MAX_TEXT_LENGTH) {
        break;
      }
    }
    let ranges = [];
    for (let index = hits.indexOf(1); index >= 0 && index < hits.length; ) {
      let len = 0;
      // eslint-disable-next-line no-empty
      for (let j = index; j < hits.length && hits[j]; ++j, ++len) {}
      ranges.push([index, len]);
      index = hits.indexOf(1, index + len);
    }
    return ranges;
  },

  getResultGroup(result) {
    if (result.group) {
      return result.group;
    }

    if (result.hasSuggestedIndex && !result.isSuggestedIndexRelativeToGroup) {
      return this.RESULT_GROUP.SUGGESTED_INDEX;
    }
    if (result.heuristic) {
      switch (result.providerName) {
        case "UrlbarProviderAliasEngines":
          return this.RESULT_GROUP.HEURISTIC_ENGINE_ALIAS;
        case "UrlbarProviderAutofill":
          return this.RESULT_GROUP.HEURISTIC_AUTOFILL;
        case "UrlbarProviderBookmarkKeywords":
          return this.RESULT_GROUP.HEURISTIC_BOOKMARK_KEYWORD;
        case "UrlbarProviderHeuristicFallback":
          return this.RESULT_GROUP.HEURISTIC_FALLBACK;
        case "UrlbarProviderHistoryUrlHeuristic":
          return this.RESULT_GROUP.HEURISTIC_HISTORY_URL;
        case "UrlbarProviderRestrictKeywordsAutofill":
          return this.RESULT_GROUP.HEURISTIC_RESTRICT_KEYWORD_AUTOFILL;
        case "UrlbarProviderTokenAliasEngines":
          return this.RESULT_GROUP.HEURISTIC_TOKEN_ALIAS_ENGINE;
        default:
          if (result.providerName.startsWith("TestProvider")) {
            return this.RESULT_GROUP.HEURISTIC_TEST;
          }
          break;
      }
      console.error(
        "Returning HEURISTIC_FALLBACK for unrecognized heuristic result: ",
        result
      );
      return this.RESULT_GROUP.HEURISTIC_FALLBACK;
    }

    switch (result.providerName) {
      case "UrlbarProviderAboutPages":
        return this.RESULT_GROUP.ABOUT_PAGES;
      default:
        break;
    }

    switch (result.type) {
      case UrlbarShared.RESULT_TYPE.RESTRICT:
        return this.RESULT_GROUP.RESTRICT_SEARCH_KEYWORD;
    }
    return this.RESULT_GROUP.GENERAL;
  },

  getUrlFromResult(result, { element = null } = {}) {
    if (
      result.payload.engine &&
      (result.type == UrlbarShared.RESULT_TYPE.SEARCH ||
        result.type == UrlbarShared.RESULT_TYPE.DYNAMIC)
    ) {
      let query = element?.dataset.query || result.payload.query;
      if (query) {
        const engine = lazy.SearchService.getEngineByName(
          result.payload.engine
        );
        let [url, postData] = this.getSearchQueryUrl(engine, query);
        return { url, postData };
      }
    }

    return {
      url: result.payload.url ?? null,
      postData: result.payload.postData
        ? this.getPostDataStream(result.payload.postData)
        : null,
    };
  },

  getSearchQueryUrl(engine, query) {
    let submission = engine.getSubmission(query);
    return [submission.uri.spec, submission.postData];
  },

  getPrefixRank(prefix) {
    return ["http://www.", "http://", "https://www.", "https://"].indexOf(
      prefix
    );
  },

  getSpanForResult(result) {
    if (result.resultSpan) {
      return result.resultSpan;
    }

    switch (result.type) {
      case UrlbarShared.RESULT_TYPE.TIP:
        return 3;
    }
    return 1;
  },

  getIconForUrl(url) {
    if (typeof url == "string") {
      return this.PROTOCOLS_WITH_ICONS.some(p => url.startsWith(p))
        ? "page-icon:" + url
        : this.ICON.DEFAULT;
    }
    if (
      URL.isInstance(url) &&
      this.PROTOCOLS_WITH_ICONS.includes(url.protocol)
    ) {
      return "page-icon:" + url.href;
    }
    return this.ICON.DEFAULT;
  },

  getRemoteIconUrl(iconUrl, size, win) {
    let url = URL.parse(iconUrl);
    if (!url) {
      return null;
    }
    if (!lazy.FaviconUtils.TRUSTED_FAVICON_SCHEMES.includes(url.protocol)) {
      return lazy.FaviconUtils.getMozRemoteImageURL(iconUrl, {
        colorScheme: win.matchMedia("(prefers-color-scheme: dark)").matches
          ? "dark"
          : "light",
      });
    }
    return iconUrl;
  },

  setupSpeculativeConnection(urlOrEngine, window) {
    if (!lazy.UrlbarPrefs.get("speculativeConnect.enabled")) {
      return;
    }
    if (urlOrEngine instanceof lazy.SearchEngine) {
      try {
        urlOrEngine.speculativeConnect({
          window,
          originAttributes: window.gBrowser.contentPrincipal.originAttributes,
        });
      } catch (ex) {
      }
      return;
    }

    if (URL.isInstance(urlOrEngine)) {
      urlOrEngine = urlOrEngine.href;
    }

    try {
      let uri =
        urlOrEngine instanceof Ci.nsIURI
          ? urlOrEngine
          : Services.io.newURI(urlOrEngine);
      Services.io.speculativeConnect(
        uri,
        window.gBrowser.contentPrincipal,
        window.docShell.QueryInterface(Ci.nsIInterfaceRequestor),
        false
      );
    } catch (ex) {
    }
  },

  extractRefFromUrl(url) {
    let uri = URL.parse(url)?.URI;
    if (uri) {
      return { base: uri.specIgnoringRef, ref: uri.ref };
    }
    return { base: url };
  },

  stripPrefixAndTrim(spec, options = {}) {
    let prefix = "";
    let suffix = "";
    if (options.stripHttp && spec.startsWith("http://")) {
      spec = spec.slice(7);
      prefix = "http://";
    } else if (options.stripHttps && spec.startsWith("https://")) {
      spec = spec.slice(8);
      prefix = "https://";
    }
    if (options.stripWww && spec.startsWith("www.")) {
      spec = spec.slice(4);
      prefix += "www.";
    }
    if (options.trimEmptyHash && spec.endsWith("#")) {
      spec = spec.slice(0, -1);
      suffix = "#" + suffix;
    }
    if (options.trimEmptyQuery && spec.endsWith("?")) {
      spec = spec.slice(0, -1);
      suffix = "?" + suffix;
    }
    if (options.trimSlash && spec.endsWith("/")) {
      spec = spec.slice(0, -1);
      suffix = "/" + suffix;
    }
    if (options.trimTrailingDot && spec.endsWith(".")) {
      spec = spec.slice(0, -1);
      suffix = "." + suffix;
    }
    return [spec, prefix, suffix];
  },

  stripPublicSuffixFromHost(host) {
    try {
      return host.substring(
        0,
        host.length - Services.eTLD.getKnownPublicSuffixFromHost(host).length
      );
    } catch (ex) {
      if (ex.result != Cr.NS_ERROR_HOST_IS_IP_ADDRESS) {
        throw ex;
      }
    }
    return host;
  },

  sanitizeTextFromClipboard(clipboardData) {
    let fixedURI, keywordAsSent;
    try {
      ({ fixedURI, keywordAsSent } = Services.uriFixup.getFixupURIInfo(
        clipboardData,
        Ci.nsIURIFixup.FIXUP_FLAG_FIX_SCHEME_TYPOS |
          Ci.nsIURIFixup.FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP
      ));
    } catch (e) {}

    let pasteData;
    if (keywordAsSent) {
      if (clipboardData.length < 500) {
        pasteData = clipboardData.replace(/\s/g, " ");
      } else {
        pasteData = clipboardData;
      }
    } else if (
      fixedURI?.scheme == "data" &&
      !fixedURI.spec.match(/^data:.+;base64,/)
    ) {
      pasteData = clipboardData.replace(/[\r\n]/g, " ");
    } else {
      pasteData = clipboardData.replace(/[\r\n]/g, "");
    }

    return this.stripUnsafeProtocolOnPaste(pasteData);
  },

  stripUnsafeProtocolOnPaste(pasteData) {
    for (;;) {
      let scheme = "";
      try {
        scheme = Services.io.extractScheme(pasteData);
      } catch (ex) {
      }
      if (scheme != "javascript") {
        break;
      }

      pasteData = pasteData.substring(pasteData.indexOf(":") + 1);
    }
    return pasteData;
  },

  async addToInputHistory(url, input) {
    if (!AppConstants.MOZ_PLACES || !lazy.historyEnabled) {
      return false;
    }
    let rows = await lazy.PlacesUtils.withConnectionWrapper(
      "addToInputHistory",
      db => {
        return db.executeCached(
          `
          INSERT OR REPLACE INTO moz_inputhistory
          SELECT h.id, IFNULL(i.input, :input), IFNULL(i.use_count, 0) * .9 + 1
          FROM moz_places h
          LEFT JOIN moz_inputhistory i ON i.place_id = h.id AND i.input = :input
          WHERE url_hash = hash(:url) AND url = :url
          RETURNING place_id
          `,
          { url, input: input.toLowerCase() }
        );
      }
    );
    return !!rows.length;
  },

  async addToInputHistoryWhenReady(url, input) {
    if (!AppConstants.MOZ_PLACES || !lazy.historyEnabled) {
      return;
    }
    let { promise: visitedPromise, resolve: visitedResolve } =
      Promise.withResolvers();
    let listener = events => {
      for (let event of events) {
        if (event.type == "page-visited" && event.url == url) {
          PlacesObservers.removeListener(["page-visited"], listener);
          visitedResolve(true);
          return;
        }
      }
    };
    PlacesObservers.addListener(["page-visited"], listener);

    let timeoutId = lazy.setTimeout(() => {
      PlacesObservers.removeListener(["page-visited"], listener);
      visitedResolve(false);
    }, 1000);

    if (await this.addToInputHistory(url, input)) {
      PlacesObservers.removeListener(["page-visited"], listener);
      lazy.clearTimeout(timeoutId);
      return;
    }

    let visited = await visitedPromise;
    lazy.clearTimeout(timeoutId);
    if (visited) {
      await this.addToInputHistory(url, input);
    }
  },

  async removeInputHistory(url, input) {
    if (!AppConstants.MOZ_PLACES) {
      return;
    }

    await lazy.PlacesUtils.withConnectionWrapper("removeInputHistory", db => {
      return db.executeCached(
        `
        DELETE FROM moz_inputhistory
        WHERE input BETWEEN :input AND :input || X'FFFF'
          AND place_id =
            (SELECT id FROM moz_places WHERE url_hash = hash(:url) AND url = :url)
        `,
        { url, input: input.toLowerCase() }
      );
    });
  },

  async blockAutofill(url, blockUntilMs) {
    if (this.isOriginUrl(url)) {
      await this.blockOriginAutofill(url, blockUntilMs);
    } else {
      await this.blockOriginPageAutofill(url, blockUntilMs);
    }
  },

  async blockOriginAutofill(url, blockUntilMs) {
    if (!AppConstants.MOZ_PLACES) {
      return;
    }

    let origin = parseOriginParts(url);
    if (!origin) {
      return;
    }

    let baseHost = origin.host.replace(/^www\./, "");
    let wwwHost = "www." + baseHost;

    await lazy.PlacesUtils.withConnectionWrapper("blockOriginAutofill", db => {
      return db.executeCached(
        `
      UPDATE moz_origins SET block_until_ms = :blockUntilMs
      WHERE host IN (:baseHost, :wwwHost)
        AND prefix IN ('http://', 'https://')
      `,
        { blockUntilMs, baseHost, wwwHost }
      );
    });
  },

  async blockOriginPageAutofill(url, blockPagesUntilMs) {
    if (!AppConstants.MOZ_PLACES) {
      return;
    }

    let origin = parseOriginParts(url);
    if (!origin) {
      return;
    }

    let baseHost = origin.host.replace(/^www\./, "");
    let wwwHost = "www." + baseHost;

    await lazy.PlacesUtils.withConnectionWrapper(
      "blockOriginPageAutofill",
      db => {
        return db.executeCached(
          `
        UPDATE moz_origins SET block_pages_until_ms = :blockPagesUntilMs
        WHERE host IN (:baseHost, :wwwHost)
          AND prefix IN ('http://', 'https://')
        `,
          { blockPagesUntilMs, baseHost, wwwHost }
        );
      }
    );
  },

  async clearOriginAutofillBlock(url) {
    if (!AppConstants.MOZ_PLACES) {
      return false;
    }

    let origin = parseOriginParts(url);
    if (!origin) {
      return false;
    }

    let baseHost = origin.host.replace(/^www\./, "");
    let wwwHost = "www." + baseHost;

    let rows = await lazy.PlacesUtils.withConnectionWrapper(
      "clearOriginAutofillBlock",
      db => {
        return db.executeCached(
          `
        UPDATE moz_origins SET block_until_ms = NULL
        WHERE host IN (:baseHost, :wwwHost)
          AND prefix IN ('http://', 'https://')
          AND block_until_ms IS NOT NULL
        RETURNING id
        `,
          { baseHost, wwwHost }
        );
      }
    );
    return !!rows.length;
  },

  async clearOriginPageAutofillBlock(url) {
    if (!AppConstants.MOZ_PLACES) {
      return false;
    }

    let origin = parseOriginParts(url);
    if (!origin) {
      return false;
    }

    let baseHost = origin.host.replace(/^www\./, "");
    let wwwHost = "www." + baseHost;

    let rows = await lazy.PlacesUtils.withConnectionWrapper(
      "clearOriginPageAutofillBlock",
      db => {
        return db.executeCached(
          `
        UPDATE moz_origins SET block_pages_until_ms = NULL
        WHERE host IN (:baseHost, :wwwHost)
          AND prefix IN ('http://', 'https://')
          AND block_pages_until_ms IS NOT NULL
        RETURNING id
        `,
          { baseHost, wwwHost }
        );
      }
    );
    return !!rows.length;
  },


  _backspaceBlocks: new Map(),

  _lastRecordAutofillBackspacePromise: Promise.resolve(),

  _BACKSPACE_BLOCK_MAX_AGE_HOURS: 24,

  _BACKSPACE_BLOCKS_MAX: 512,

  _backspaceBlockKey(url) {
    let origin = parseOriginParts(url);
    if (!origin) {
      return null;
    }
    let basehost = origin.host.replace(/^www\./, "");
    let scope =  (
      this.isOriginUrl(url) ? "origin" : "page"
    );
    return `${scope}:${basehost}`;
  },

  async recordAutofillBackspace(url) {
    let key = this._backspaceBlockKey(url);
    if (!key) {
      return;
    }

    let entry = this._backspaceBlocks.get(key) ?? {
      count: 0,
      blockedAt: null,
    };
    if (entry.blockedAt) {
      delete entry.blockedAt;
    }
    let newCount = (entry.count ?? 0) + 1;

    if (newCount >= lazy.UrlbarPrefs.get("autoFill.backspaceThreshold")) {
      delete entry.count;
      entry.blockedAt = Date.now();
      await this.blockAutofill(
        url,
        Date.now() + lazy.UrlbarPrefs.get("autoFill.backspaceBlockDurationMs")
      ).catch(console.error);
    } else {
      entry.count = newCount;
    }

    this._backspaceBlocks.delete(key);
    this._backspaceBlocks.set(key, entry);

    if (this._backspaceBlocks.size > this._BACKSPACE_BLOCKS_MAX) {
      let oldestKey = this._backspaceBlocks.keys().next().value;
      this._backspaceBlocks.delete(oldestKey);
    }
  },

  getBackspaceBlock(url) {
    let key = this._backspaceBlockKey(url);
    if (!key) {
      return null;
    }

    let entry = this._backspaceBlocks.get(key);
    if (!entry?.blockedAt) {
      return null;
    }

    this._backspaceBlocks.delete(key);

    let ageHours = (Date.now() - entry.blockedAt) / (60 * 60 * 1000);
    if (ageHours > this._BACKSPACE_BLOCK_MAX_AGE_HOURS) {
      return null;
    }
    let level = this.isOriginUrl(url) ? "origin" : "url";
    return { blockedAt: entry.blockedAt, level };
  },

  clearAutofillBackspaceEntryForUrl(url) {
    let key = this._backspaceBlockKey(url);
    if (key) {
      this._backspaceBlocks.delete(key);
    }
  },

  isOriginUrl(url) {
    let parsed = URL.parse(url);
    return (
      !!parsed && parsed.pathname === "/" && !parsed.search && !parsed.hash
    );
  },

  isPasteEvent(event) {
    return (
      event.inputType &&
      (event.inputType.startsWith("insertFromPaste") ||
        event.inputType == "insertFromYank")
    );
  },

  looksLikeSingleWordHost(value) {
    let str = value.trim();
    return this.REGEXP_SINGLE_WORD.test(str);
  },

  substringAt(sourceStr, targetStr) {
    let index = sourceStr.indexOf(targetStr);
    return index < 0 ? "" : sourceStr.substr(index);
  },

  substringAfter(sourceStr, targetStr) {
    let index = sourceStr.indexOf(targetStr);
    return index < 0 ? "" : sourceStr.substr(index + targetStr.length);
  },

  stripURLPrefix(str) {
    let match = lazy.UrlUtils.REGEXP_PREFIX.exec(str);
    if (!match) {
      return ["", str];
    }
    let prefix = match[0];
    if (prefix.length < str.length && str[prefix.length] == " ") {
      return ["", str];
    }
    if (
      prefix.endsWith(":") &&
      !this.PROTOCOLS_WITHOUT_AUTHORITY.includes(prefix.toLowerCase())
    ) {
      return ["", str];
    }
    return [prefix, str.substring(prefix.length)];
  },

  async getHeuristicResultFor(searchString, urlbarInput) {
    if (!searchString) {
      throw new Error("Must pass a non-null search string");
    }

    let gBrowser = urlbarInput.window.gBrowser;
    let options = {
      allowAutofill: false,
      isPrivate: urlbarInput.isPrivate,
      sapName: urlbarInput.sapName,
      maxResults: 1,
      searchString,
      userContextId: parseInt(
        gBrowser.selectedBrowser.getAttribute("usercontextid") || 0
      ),
      tabGroup: gBrowser.selectedTab.group?.id ?? null,
      providers: [
        "UrlbarProviderAliasEngines",
        "UrlbarProviderBookmarkKeywords",
        "UrlbarProviderHeuristicFallback",
      ],
    };
    if (urlbarInput.searchMode) {
      let searchMode = urlbarInput.searchMode;
      options.searchMode = searchMode;
      if (searchMode.source) {
        options.sources = [searchMode.source];
      }
    }
    let context = new UrlbarQueryContext(options);
    let heuristicResult =
      await urlbarInput.controller.getHeuristicResult(context);
    if (!heuristicResult) {
      throw new Error("There should always be an heuristic result");
    }
    return heuristicResult;
  },

  getResultSourceName(source) {
    if (!this._resultSourceNamesBySource) {
      this._resultSourceNamesBySource = new Map();
      for (let [name, src] of Object.entries(UrlbarShared.RESULT_SOURCE)) {
        this._resultSourceNamesBySource.set(src, name.toLowerCase());
      }
    }
    return this._resultSourceNamesBySource.get(source);
  },

  canAutofillURL(urlString, candidateString, checkFragmentOnly = false) {
    if (
      !checkFragmentOnly &&
      (urlString.length <= candidateString.length ||
        !urlString
          .toLocaleLowerCase()
          .startsWith(candidateString.toLocaleLowerCase()))
    ) {
      return false;
    }

    if (!lazy.UrlUtils.REGEXP_PREFIX.test(urlString)) {
      urlString = "http://" + urlString;
    }
    if (!lazy.UrlUtils.REGEXP_PREFIX.test(candidateString)) {
      candidateString = "http://" + candidateString;
    }

    let url = URL.parse(urlString);
    let candidate = URL.parse(candidateString);
    if (!url || !candidate) {
      return false;
    }

    if (checkFragmentOnly) {
      return url.hash.startsWith(candidate.hash);
    }

    if (!candidate.href.endsWith("/")) {
      let nextSlashIndex = url.pathname.indexOf("/", candidate.pathname.length);
      if (nextSlashIndex >= 0 && nextSlashIndex != url.pathname.length - 1) {
        return false;
      }
    } else if (url.pathname.length > candidate.pathname.length || url.hash) {
      return false;
    }

    return url.hash.startsWith(candidate.hash);
  },

  unEscapeURIForUI(uri) {
    return uri.length > this.MAX_TEXT_LENGTH
      ? uri
      : Services.textToSubURI.unEscapeURIForUI(uri);
  },

  isTextDirectionRTL(value, window) {
    let directionality = window.windowUtils.getDirectionFromText(value);
    return directionality == window.windowUtils.DIRECTION_RTL;
  },

  prepareUrlForDisplay(url, { trimURL = true, schemeless = false } = {}) {
    let displayString;
    if (typeof url == "string") {
      try {
        displayString = new URL(url).URI.displaySpec;
      } catch {
        displayString = url;
      }
    } else {
      displayString = url.URI.displaySpec;
    }

    if (displayString) {
      if (schemeless) {
        displayString = this.stripPrefixAndTrim(displayString, {
          stripHttp: true,
          stripHttps: true,
        })[0];
      } else if (trimURL && lazy.UrlbarPrefs.get("trimURLs")) {
        displayString =
          lazy.BrowserUIUtils.removeSingleTrailingSlashFromURL(displayString);
        if (displayString.startsWith("https://")) {
          displayString = displayString.substring(8);
          if (displayString.startsWith("www.")) {
            displayString = displayString.substring(4);
          }
        }
      }
    }

    return this.unEscapeURIForUI(displayString);
  },

  resultType(result) {
    if (!result) {
      return "input_field";
    }

    function checkForSubType(type, res) {
      if (
        lazy.UrlbarSearchUtils.resultIsSERP(res, [
          UrlbarShared.RESULT_SOURCE.BOOKMARKS,
          UrlbarShared.RESULT_SOURCE.HISTORY,
          UrlbarShared.RESULT_SOURCE.TABS,
        ])
      ) {
        type += "_serp";
      }
      return type;
    }

    switch (result.type) {
      case UrlbarShared.RESULT_TYPE.DYNAMIC:
        switch (result.providerName) {
          case "UrlbarProviderGlobalActions":
          case "UrlbarProviderActionsSearchMode":
            return "action";
        }
        break;
      case UrlbarShared.RESULT_TYPE.KEYWORD:
        return "keyword";
      case UrlbarShared.RESULT_TYPE.SEARCH:
        if (result.source == UrlbarShared.RESULT_SOURCE.HISTORY) {
          return "search_history";
        }
        return "search_engine";
      case UrlbarShared.RESULT_TYPE.TAB_SWITCH:
        return checkForSubType("tab", result);
      case UrlbarShared.RESULT_TYPE.TIP:
        return "tip";
      case UrlbarShared.RESULT_TYPE.URL:
        if (
          result.source === UrlbarShared.RESULT_SOURCE.OTHER_LOCAL &&
          result.heuristic
        ) {
          return "url";
        }
        if (result.autofill) {
          return `autofill_${result.autofill.type ?? "unknown"}`;
        }
        if (result.payload.isAutofillFallback) {
          return "history_autofill_fallback_origin";
        }
        if (result.source === UrlbarShared.RESULT_SOURCE.BOOKMARKS) {
          return checkForSubType("bookmark", result);
        }
        return checkForSubType("history", result);
      case UrlbarShared.RESULT_TYPE.RESTRICT:
        if (result.payload.keyword === UrlbarShared.RESTRICT_TOKENS.BOOKMARK) {
          return "restrict_keyword_bookmarks";
        }
        if (result.payload.keyword === UrlbarShared.RESTRICT_TOKENS.OPENPAGE) {
          return "restrict_keyword_tabs";
        }
        if (result.payload.keyword === UrlbarShared.RESTRICT_TOKENS.HISTORY) {
          return "restrict_keyword_history";
        }
        if (result.payload.keyword === UrlbarShared.RESTRICT_TOKENS.ACTION) {
          return "restrict_keyword_actions";
        }
        break;
    }

    return "unknown";
  },

  tupleString(...tokens) {
    return tokens.filter(t => t).join("|");
  },

  copySnakeKeysToCamel(obj, overwrite = true) {
    for (let [key, value] of Object.entries(obj)) {
      let match = key.match(/^_+/);
      if (match) {
        key = key.substring(match[0].length);
      }
      let camelKey = key.replace(/_([^_])/g, (m, p1) => p1.toUpperCase());
      if (match) {
        camelKey = match[0] + camelKey;
      }
      if (!overwrite && camelKey != key && obj.hasOwnProperty(camelKey)) {
        throw new Error(
          `Can't copy snake_case key '${key}' to camelCase key ` +
            `'${camelKey}' because '${camelKey}' is already defined`
        );
      }
      obj[camelKey] = value;
      if (value && typeof value == "object") {
        this.copySnakeKeysToCamel(value);
      }
    }
    return obj;
  },

  createTabSwitchSecondaryAction(userContextId) {
    let action = { key: "tabswitch" };
    let identity =
      lazy.ContextualIdentityService.getPublicIdentityFromId(userContextId);

    if (identity) {
      let label =
        lazy.ContextualIdentityService.getUserContextLabel(
          userContextId
        ).toLowerCase();
      action.l10nId = "urlbar-result-action-switch-tab-with-container";
      action.l10nArgs = {
        container: label,
      };
      action.classList = [
        "urlbarView-userContext",
        `identity-color-${identity.color}`,
      ];
    } else {
      action.l10nId = "urlbar-result-action-switch-tab";
    }

    return action;
  },

  addTextContentWithHighlights(parentNode, textContent, highlights) {
    parentNode.textContent = "";
    if (!textContent) {
      return;
    }

    highlights = (highlights || []).concat([[textContent.length, 0]]);
    let index = 0;
    for (let [highlightIndex, highlightLength] of highlights) {
      if (highlightIndex - index > 0) {
        parentNode.appendChild(
          parentNode.ownerDocument.createTextNode(
            textContent.substring(index, highlightIndex)
          )
        );
      }
      if (highlightLength > 0) {
        let strong = parentNode.ownerDocument.createElement("strong");
        strong.textContent = textContent.substring(
          highlightIndex,
          highlightIndex + highlightLength
        );
        parentNode.appendChild(strong);
      }
      index = highlightIndex + highlightLength;
    }
  },

  getURLBarForFocus(window) {
    return window.gURLBar;
  },

  formatDate(
    date,
    {
      forceAbsoluteDate = false,
      capitalizeRelativeDate = false,
      includeTimeZone = false,
    } = {}
  ) {
    let parseDateResult = this.parseDate(date);
    let { zonedNow, zonedDate, daysUntil, isFuture } = parseDateResult;

    let formattedDate;
    let isRelative = false;
    if (Math.abs(daysUntil) <= 1) {
      isRelative = true;
      formattedDate = new Intl.RelativeTimeFormat(undefined, {
        numeric: "auto",
      }).format(daysUntil, "day");
      if (capitalizeRelativeDate) {
        formattedDate =
          formattedDate[0].toLocaleUpperCase() + formattedDate.substring(1);
      }
    } else {
      let opts = {
        timeZone: zonedNow.timeZoneId,
      };
      if (!forceAbsoluteDate && 0 < daysUntil && daysUntil < 7) {
        opts.weekday = "short";
      } else {
        opts.month = "short";
        opts.day = "numeric";
        if (zonedDate.year != zonedNow.year) {
          opts.year = "numeric";
        }
      }
      formattedDate = new Intl.DateTimeFormat(undefined, opts).format(date);
    }

    let formattedTime;
    if (isFuture) {
      formattedTime = new Intl.DateTimeFormat(undefined, {
        hour: "numeric",
        minute: "numeric",
        timeZoneName: includeTimeZone ? "short" : undefined,
        timeZone: zonedNow.timeZoneId,
      }).format(date);
    }

    return {
      isRelative,
      formattedDate,
      formattedTime,
      parseDateResult,
    };
  },

  parseDate(date) {
    let zonedNow = this._zonedDateTimeISO();
    let zonedDate = date.toTemporalInstant().toZonedDateTimeISO(zonedNow);
    let isFuture = Temporal.ZonedDateTime.compare(zonedNow, zonedDate) < 0;

    let today = zonedNow.startOfDay();
    let dateDay = zonedDate.startOfDay();

    let duration = today.until(dateDay).round("days");
    let daysUntil = duration.days;

    return {
      zonedNow,
      zonedDate,
      isFuture,
      daysUntil,
    };
  },

  _zonedDateTimeISO() {
    return Temporal.Now.zonedDateTimeISO();
  },
};

ChromeUtils.defineLazyGetter(UrlbarUtils.ICON, "DEFAULT", () => {
  return AppConstants.MOZ_PLACES
    ? lazy.PlacesUtils.favicons.defaultFavicon.spec
    : UrlbarUtils.ICON.GLOBE;
});

ChromeUtils.defineLazyGetter(UrlbarUtils, "strings", () => {
  return Services.strings.createBundle(
    "chrome://global/locale/autocomplete.properties"
  );
});

const L10N_SCHEMA = {
  type: "object",
  required: ["id"],
  properties: {
    id: {
      type: "string",
    },
    args: {
      type: "object",
      additionalProperties: true,
    },
    argsHighlights: {
      type: "object",
      additionalProperties: true,
    },
    parseMarkup: {
      type: "boolean",
    },
  },
};

UrlbarUtils.RESULT_PAYLOAD_SCHEMA = {
  [UrlbarShared.RESULT_TYPE.TAB_SWITCH]: {
    type: "object",
    required: ["url"],
    properties: {
      action: {
        type: "object",
        properties: {
          classList: {
            type: "array",
            items: {
              type: "string",
            },
          },
          l10nArgs: {
            type: "object",
            additionalProperties: true,
          },
          l10nId: {
            type: "string",
          },
          key: {
            type: "string",
          },
        },
      },
      bookmarkDateMs: {
        type: "number",
      },
      frecency: {
        type: "number",
      },
      icon: {
        type: "string",
      },
      isPinned: {
        type: "boolean",
      },
      lastVisit: {
        type: "number",
      },
      tabGroup: {
        type: "string",
      },
      title: {
        type: "string",
      },
      url: {
        type: "string",
      },
      userContextId: {
        type: "number",
      },
    },
  },
  [UrlbarShared.RESULT_TYPE.SEARCH]: {
    type: "object",
    properties: {
      blockL10n: L10N_SCHEMA,
      description: {
        type: "string",
      },
      descriptionL10n: L10N_SCHEMA,
      engine: {
        type: "string",
      },
      helpUrl: {
        type: "string",
      },
      icon: {
        type: "string",
      },
      inPrivateWindow: {
        type: "boolean",
      },
      isBlockable: {
        type: "boolean",
      },
      isPinned: {
        type: "boolean",
      },
      isPrivateEngine: {
        type: "boolean",
      },
      isGeneralPurposeEngine: {
        type: "boolean",
      },
      keyword: {
        type: "string",
      },
      keywords: {
        type: "string",
      },
      providesSearchMode: {
        type: "boolean",
      },
      query: {
        type: "string",
      },
      satisfiesAutofillThreshold: {
        type: "boolean",
      },
      searchUrlDomainWithoutSuffix: {
        type: "string",
      },
      title: {
        type: "string",
      },
      url: {
        type: "string",
      },
    },
  },
  [UrlbarShared.RESULT_TYPE.URL]: {
    type: "object",
    required: ["url"],
    properties: {
      blockL10n: L10N_SCHEMA,
      bookmarkDateMs: {
        type: "number",
      },
      bottomTextL10n: L10N_SCHEMA,
      description: {
        type: "string",
      },
      descriptionL10n: L10N_SCHEMA,
      dismissalKey: {
        type: "string",
      },
      dupedHeuristic: {
        type: "boolean",
      },
      frecency: {
        type: "number",
      },
      helpL10n: L10N_SCHEMA,
      helpUrl: {
        type: "string",
      },
      icon: {
        type: "string",
      },
      iconBlob: {
        type: "object",
      },
      isAutofillFallback: {
        type: "boolean",
      },
      lastVisit: {
        type: "number",
      },
      provider: {
        type: "string",
      },
      source: {
        type: "string",
      },
      subtype: {
        type: "string",
      },
      tags: {
        type: "array",
        items: {
          type: "string",
        },
      },
      title: {
        type: "string",
      },
      titleL10n: L10N_SCHEMA,
      subtitle: {
        type: "string",
      },
      subtitleL10n: L10N_SCHEMA,
      url: {
        type: "string",
      },
      urlTimestampIndex: {
        type: "number",
      },
    },
  },
  [UrlbarShared.RESULT_TYPE.KEYWORD]: {
    type: "object",
    required: ["keyword", "url"],
    properties: {
      icon: {
        type: "string",
      },
      input: {
        type: "string",
      },
      keyword: {
        type: "string",
      },
      postData: {
        type: "string",
      },
      title: {
        type: "string",
      },
      url: {
        type: "string",
      },
    },
  },
  [UrlbarShared.RESULT_TYPE.TIP]: {
    type: "object",
    required: ["type"],
    properties: {
      buttons: {
        type: "array",
        items: {
          type: "object",
          required: ["l10n"],
          properties: {
            l10n: L10N_SCHEMA,
            url: {
              type: "string",
            },
            command: {
              type: "string",
            },
            input: {
              type: "string",
            },
            attributes: {
              type: "object",
              properties: {
                primary: {
                  type: "string",
                },
              },
            },
            menu: {
              type: "array",
              items: {
                type: "object",
                properties: {
                  l10n: L10N_SCHEMA,
                  name: {
                    type: "string",
                  },
                },
              },
            },
          },
        },
      },
      helpL10n: L10N_SCHEMA,
      helpUrl: {
        type: "string",
      },
      icon: {
        type: "string",
      },
      titleL10n: L10N_SCHEMA,
      descriptionL10n: L10N_SCHEMA,
      type: {
        type: "string",
        enum: [
          "dismissalAcknowledgment",
          "intervention_clear",
          "intervention_refresh",
          "intervention_update_ask",
          "intervention_update_refresh",
          "intervention_update_restart",
          "intervention_update_web",
          "realtime_opt_in",
          "searchTip_onboard",
          "searchTip_redirect",
          "test", 
        ],
      },
    },
  },
  [UrlbarShared.RESULT_TYPE.DYNAMIC]: {
    type: "object",
    required: ["dynamicType"],
    properties: {
      dynamicType: {
        type: "string",
      },
    },
  },
  [UrlbarShared.RESULT_TYPE.RESTRICT]: {
    type: "object",
    properties: {
      icon: {
        type: "string",
      },
      keyword: {
        type: "string",
      },
      l10nRestrictKeywords: {
        type: "array",
        items: {
          type: "string",
        },
      },
      autofillKeyword: {
        type: "string",
      },
      providesSearchMode: {
        type: "boolean",
      },
    },
  },
};


export class UrlbarQueryContext {
  constructor(options) {
    options = structuredClone(options);

    this._checkRequiredOptions(options, [
      "allowAutofill",
      "isPrivate",
      "maxResults",
      "sapName",
      "searchString",
    ]);

    if (isNaN(options.maxResults)) {
      throw new Error(
        `Invalid maxResults property provided to UrlbarQueryContext`
      );
    }

    const optionalProperties = [
      ["currentPage", v => typeof v == "string" && !!v.length],
      ["providers", v => Array.isArray(v) && !!v.length],
      ["searchMode", v => v && typeof v == "object"],
      ["sources", v => Array.isArray(v) && !!v.length],
    ];

    for (let [prop, checkFn, defaultValue] of optionalProperties) {
      if (prop in options) {
        if (!checkFn(options[prop])) {
          throw new Error(`Invalid value for option "${prop}"`);
        }
        this[prop] = options[prop];
      } else if (defaultValue !== undefined) {
        this[prop] = defaultValue;
      }
    }

    this.lastResultCount = 0;
    this.pendingHeuristicProviders = new Set();
    this.deferUserSelectionProviders = new Set();
    this.trimmedSearchString = this.searchString.trim();
    this.lowerCaseSearchString = this.searchString.toLowerCase();
    this.trimmedLowerCaseSearchString = this.trimmedSearchString.toLowerCase();
    this.userContextId =
      lazy.UrlbarProviderOpenTabs.getUserContextIdForOpenPagesTable(
        options.userContextId,
        this.isPrivate
      ) || Ci.nsIScriptSecurityManager.DEFAULT_USER_CONTEXT_ID;
    this.tabGroup = options.tabGroup || null;

  }

  allowAutofill;

  canceled = false;

  currentPage;

  firstResult;

  firstResultChanged = false;

  heuristicResult;

  isPrivate;

  maxResults;

  muxer;

  providers;

  restrictSource;

  restrictToken;

  results;

  sapName;

  searchMode;

  restrictInSearchMode() {
    if (lazy.UrlbarPrefs.get("unifiedSearchButton.historyInSearchMode")) {
      return !!this.searchMode && !this.searchMode.engineName;
    }
    return !!this.searchMode;
  }

  searchString;

  sources;

  tokens;

  _checkRequiredOptions(options, optionNames) {
    for (let optionName of optionNames) {
      if (!(optionName in options)) {
        throw new Error(
          `Missing or empty ${optionName} provided to UrlbarQueryContext`
        );
      }
      this[optionName] = options[optionName];
    }
  }

  get fixupInfo() {
    if (!this._fixupError && !this._fixupInfo && this.trimmedSearchString) {
      let flags =
        Ci.nsIURIFixup.FIXUP_FLAG_FIX_SCHEME_TYPOS |
        Ci.nsIURIFixup.FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP;
      if (this.isPrivate) {
        flags |= Ci.nsIURIFixup.FIXUP_FLAG_PRIVATE_CONTEXT;
      }

      try {
        let info = Services.uriFixup.getFixupURIInfo(this.searchString, flags);

        this._fixupInfo = {
          href: info.fixedURI.spec,
          isSearch: !!info.keywordAsSent,
          scheme: info.fixedURI.scheme,
        };
      } catch (ex) {
        this._fixupError = ex.result;
      }
    }

    return this._fixupInfo || null;
  }

  get fixupError() {
    if (!this.fixupInfo) {
      return this._fixupError;
    }

    return null;
  }

  toWire() {
    return {
      ...this,
      results: this.results?.map(result => result.toWire()),
      heuristicResult: this.heuristicResult?.toWire(),
    };
  }

  static fromWire(wire) {
    Object.setPrototypeOf(wire, UrlbarQueryContext.prototype);
    wire.results = wire.results?.map(lazy.UrlbarResult.fromWire) ?? [];
    if (wire.heuristicResult) {
      wire.heuristicResult = lazy.UrlbarResult.fromWire(wire.heuristicResult);
    }
    return wire;
  }
}

export class UrlbarMuxer {
  get name() {
    return "UrlbarMuxerBase";
  }

  sort(_queryContext, _unsortedResults) {
    throw new Error("Trying to access the base class, must be overridden");
  }
}

export class UrlbarProvider {
  #lazy = XPCOMUtils.declareLazy({
    logger: () => UrlbarShared.getLogger({ prefix: `Provider.${this.name}` }),
  });

  get logger() {
    return this.#lazy.logger;
  }

  get name() {
    return this.constructor.name;
  }

  get type() {
    throw new Error("Trying to access the base class, must be overridden");
  }

  queryInstance;

  tryMethod(methodName, ...args) {
    try {
      return this[methodName](...args);
    } catch (ex) {
      console.error(ex);
    }
    return undefined;
  }

  async isActive(_queryContext, _controller) {
    throw new Error("Trying to access the base class, must be overridden");
  }

  getPriority(_queryContext) {
    return 0;
  }

  startQuery(_queryContext, _addCallback, _controller) {
    throw new Error("Trying to access the base class, must be overridden");
  }

  cancelQuery(_queryContext) {
  }





  onBeforeSelection(_result, _element) {}

  onSelection(_result, _element) {}



  getViewTemplate(_result) {
    return null;
  }

  getViewUpdate(_result, _idsByName) {
    return null;
  }

  getResultCommands(_result, _isPrivate) {
    return null;
  }

  get deferUserSelection() {
    return false;
  }
}

export class SkippableTimer {
  done = false;

  constructor({
    name = "<anonymous timer>",
    callback = null,
    time = 0,
    reportErrorOnTimeout = false,
    logger = null,
  } = {}) {
    this.name = name;
    this.logger = logger;

    let timerPromise = new Promise(resolve => {
      this._timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
      this._timer.initWithCallback(
        () => {
          this._log(`Timed out!`, reportErrorOnTimeout);
          this.done = true;
          this._timer = null;
          resolve();
        },
        time,
        Ci.nsITimer.TYPE_ONE_SHOT
      );
      this._log(`Started`);
    });

    let firePromise = new Promise(resolve => {
      this.fire = async () => {
        this.done = true;
        if (this._timer) {
          if (!this._canceled) {
            this._log(`Skipped`);
          }
          this._timer.cancel();
          this._timer = null;
          resolve();
        }
        await this.promise;
      };
    });

    this.promise = Promise.race([timerPromise, firePromise]).then(() => {
      if (callback && !this._canceled) {
        callback();
      }
    });
  }

  async cancel() {
    if (this._timer) {
      this._log(`Canceling`);
      this._canceled = true;
    }
    await this.fire();
  }

  _log(msg, isError = false) {
    let line = `SkippableTimer :: ${this.name} :: ${msg}`;
    if (this.logger) {
      this.logger.debug(line);
    }
    if (isError) {
      console.error(line);
    }
  }
}

export class TaskQueue {
  get emptyPromise() {
    return this.#emptyPromise;
  }

  queue(callback) {
    return new Promise((resolve, reject) => {
      this.#queue.push({ callback, resolve, reject });
      if (this.#queue.length == 1) {
        this.#emptyDeferred = Promise.withResolvers();
        this.#emptyPromise = this.#emptyDeferred.promise;
        this.#doNextTask();
      }
    });
  }

  queueIdleCallback(callback) {
    return this.queue(async () => {
      await new Promise((resolve, reject) => {
        ChromeUtils.idleDispatch(async () => {
          try {
            let value = await callback();
            resolve(value);
          } catch (error) {
            console.error(error);
            reject(error);
          }
        });
      });
    });
  }

  async #doNextTask() {
    if (!this.#queue.length) {
      this.#emptyDeferred.resolve();
      this.#emptyDeferred = null;
      return;
    }

    let { callback, resolve, reject } = this.#queue[0];
    try {
      let value = await callback();
      resolve(value);
    } catch (error) {
      console.error(error);
      reject(error);
    }
    this.#queue.shift();
    this.#doNextTask();
  }

  #queue = [];
  #emptyDeferred = null;
  #emptyPromise = Promise.resolve();
}
