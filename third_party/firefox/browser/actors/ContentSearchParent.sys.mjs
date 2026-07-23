/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AboutNewTab: "resource:///modules/AboutNewTab.sys.mjs",
  BrowserSearchTelemetry:
    "moz-src:///browser/components/search/BrowserSearchTelemetry.sys.mjs",
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  ConfigSearchEngine:
    "moz-src:///toolkit/components/search/ConfigSearchEngine.sys.mjs",
  DEFAULT_FORM_HISTORY_PARAM:
    "moz-src:///toolkit/components/search/SearchSuggestionController.sys.mjs",
  FormHistory: "resource://gre/modules/FormHistory.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  SearchSuggestionController:
    "moz-src:///toolkit/components/search/SearchSuggestionController.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
});


const MAX_LOCAL_SUGGESTIONS = 3;
const MAX_SUGGESTIONS = 6;
const SEARCH_ENGINE_PLACEHOLDER_ICON =
  "chrome://browser/skin/search-engine-placeholder.png";

let gContentSearchActors = new Set();


export let ContentSearch = {
  initialized: false,

  _eventQueue: [],
  _currentEventPromise: null,

  _suggestionMap: new WeakMap(),

  _destroyedPromise: null,

  _currentSuggestion: null,

  init() {
    if (!this.initialized) {
      Services.obs.addObserver(this, "browser-search-engine-modified");
      Services.obs.addObserver(this, "shutdown-leaks-before-check");
      lazy.UrlbarPrefs.addObserver(this);

      this.initialized = true;
    }
  },

  get searchSuggestionUIStrings() {
    if (this._searchSuggestionUIStrings) {
      return this._searchSuggestionUIStrings;
    }
    this._searchSuggestionUIStrings = {};
    let searchBundle = Services.strings.createBundle(
      "chrome://browser/locale/search.properties"
    );
    let stringNames = [
      "searchHeader",
      "searchForSomethingWith2",
      "searchWithHeader",
      "searchSettings",
    ];

    for (let name of stringNames) {
      this._searchSuggestionUIStrings[name] =
        searchBundle.GetStringFromName(name);
    }
    return this._searchSuggestionUIStrings;
  },

  destroy() {
    if (!this.initialized) {
      return new Promise();
    }

    if (this._destroyedPromise) {
      return this._destroyedPromise;
    }

    Services.obs.removeObserver(this, "browser-search-engine-modified");
    Services.obs.removeObserver(this, "shutdown-leaks-before-check");

    this._eventQueue.length = 0;
    this._destroyedPromise = Promise.resolve(this._currentEventPromise);
    return this._destroyedPromise;
  },

  observe(subj, topic, data) {
    switch (topic) {
      case "browser-search-engine-modified":
        this._eventQueue.push({
          type: "Observe",
          data,
        });
        this._processEventQueue();
        break;
      case "shutdown-leaks-before-check":
        subj.wrappedJSObject.client.addBlocker(
          "ContentSearch: Wait until the service is destroyed",
          () => this.destroy()
        );
        break;
    }
  },

  onPrefChanged(pref) {
    if (lazy.UrlbarPrefs.shouldHandOffToSearchModePrefs.includes(pref)) {
      this._eventQueue.push({
        type: "Observe",
        data: "shouldHandOffToSearchMode",
      });
      this._processEventQueue();
    }
  },

  removeFormHistoryEntry(browser, entry) {
    let browserData = this._suggestionDataForBrowser(browser);
    if (browserData?.previousFormHistoryResults) {
      let result = browserData.previousFormHistoryResults.find(
        e => e.text == entry
      );
      lazy.FormHistory.update({
        op: "remove",
        fieldname: lazy.DEFAULT_FORM_HISTORY_PARAM,
        value: entry,
        guid: result.guid,
      }).catch(err =>
        console.error("Error removing form history entry: ", err)
      );
    }
  },

  performSearch(actor, browser, data) {
    this._ensureDataHasProperties(data, [
      "engineName",
      "searchString",
      "healthReportKey",
    ]);
    let engine = lazy.SearchService.getEngineByName(data.engineName);
    let submission = engine.getSubmission(data.searchString, "");
    let win = browser.documentGlobal;
    if (!win) {
      return;
    }
    let where = lazy.BrowserUtils.whereToOpenLink(data.originalEvent);

    if (where === "current") {
      this._reply(actor, "Blur");
      browser.loadURI(submission.uri, {
        postData: submission.postData,
        triggeringPrincipal: Services.scriptSecurityManager.createNullPrincipal(
          {
            userContextId:
              win.gBrowser.selectedBrowser.getAttribute("userContextId"),
          }
        ),
      });
    } else {
      let params = {
        postData: submission.postData,
        inBackground: Services.prefs.getBoolPref(
          "browser.tabs.loadInBackground"
        ),
      };
      win.openTrustedLinkIn(submission.uri.spec, where, params);
    }
  },

  async getSuggestions(engineName, searchString, browser) {
    let engine = lazy.SearchService.getEngineByName(engineName);
    if (!engine) {
      throw new Error("Unknown engine name: " + engineName);
    }

    let browserData = this._suggestionDataForBrowser(browser, true);
    let { controller } = browserData;
    let ok = lazy.SearchSuggestionController.engineOffersSuggestions(engine);
    let maxLocalResults = ok ? MAX_LOCAL_SUGGESTIONS : MAX_SUGGESTIONS;
    let maxRemoteResults = ok ? MAX_SUGGESTIONS : 0;
    this._currentSuggestion = { controller, browser };
    let suggestions = await controller.fetch({
      searchString,
      inPrivateBrowsing: lazy.PrivateBrowsingUtils.isBrowserPrivate(browser),
      engine,
      maxLocalResults,
      maxRemoteResults,
    });

    suggestions.local = suggestions.local.map(e => e.value);
    let nonTailEntries = suggestions.remote.filter(
      e => !e.matchPrefix && !e.tail
    );
    suggestions.remote = nonTailEntries.map(e => e.value);

    this._currentSuggestion = null;

    let result = {};
    if (!suggestions) {
      return result;
    }

    browserData.previousFormHistoryResults = suggestions.formHistoryResults;
    result = {
      engineName,
      term: suggestions.term,
      local: suggestions.local,
      remote: suggestions.remote,
    };
    return result;
  },

  async addFormHistoryEntry(browser, entry = null) {
    let isPrivate = false;
    try {
      isPrivate = lazy.PrivateBrowsingUtils.isBrowserPrivate(browser);
    } catch (err) {
      return false;
    }
    if (
      isPrivate ||
      !entry ||
      entry.value.length >
        lazy.SearchSuggestionController.SEARCH_HISTORY_MAX_VALUE_LENGTH
    ) {
      return false;
    }
    lazy.FormHistory.update({
      op: "bump",
      fieldname: lazy.DEFAULT_FORM_HISTORY_PARAM,
      value: entry.value,
      source: entry.engineName,
    }).catch(err => console.error("Error adding form history entry: ", err));
    return true;
  },

  async currentStateObj() {
    let state = {
      engines: [],
      currentEngine: await this._currentEngineObj(false),
      currentPrivateEngine: await this._currentEngineObj(true),
    };

    for (let engine of await lazy.SearchService.getVisibleEngines()) {
      state.engines.push({
        name: engine.name,
        iconData: await this._getEngineIconURL(engine),
        hidden: engine.hideOneOffButton,
        isConfigEngine: engine instanceof lazy.ConfigSearchEngine,
      });
    }

    return state;
  },

  _processEventQueue() {
    if (this._currentEventPromise || !this._eventQueue.length) {
      return;
    }

    let event = this._eventQueue.shift();

    this._currentEventPromise = (async () => {
      try {
        await this["_on" + event.type](event);
      } catch (err) {
        console.error(err);
      } finally {
        this._currentEventPromise = null;

        this._processEventQueue();
      }
    })();
  },

  _cancelSuggestions({ actor, browser }) {
    let cancelled = false;
    if (
      this._currentSuggestion &&
      this._currentSuggestion.browser === browser
    ) {
      this._currentSuggestion.controller.stop();
      cancelled = true;
    }
    for (let i = 0; i < this._eventQueue.length; i++) {
      let m = this._eventQueue[i];
      if (actor === m.actor && m.name === "GetSuggestions") {
        this._eventQueue.splice(i, 1);
        cancelled = true;
        i--;
      }
    }
    if (cancelled) {
      this._reply(actor, "SuggestionsCancelled");
    }
  },

  async _onMessage(eventItem) {
    let methodName = "_onMessage" + eventItem.name;
    if (methodName in this) {
      await this._initService();
      await this[methodName](eventItem);
      eventItem.browser.removeEventListener("SwapDocShells", eventItem, true);
    }
  },

  async _onMessageGetState({ actor }) {
    let state = await this.currentStateObj();
    return this._reply(actor, "State", state);
  },

  async _onMessageGetEngine({ actor }) {
    let state = await this.currentStateObj();
    let { usePrivateBrowsing } = actor.browsingContext;
    return this._reply(actor, "Engine", {
      isPrivateEngine: usePrivateBrowsing,
      engine: usePrivateBrowsing
        ? state.currentPrivateEngine
        : state.currentEngine,
    });
  },

  _onMessageGetHandoffSearchModePrefs({ actor }) {
    this._reply(
      actor,
      "HandoffSearchModePrefs",
      lazy.UrlbarPrefs.get("shouldHandOffToSearchMode")
    );
  },

  _onMessageGetStrings({ actor }) {
    this._reply(actor, "Strings", this.searchSuggestionUIStrings);
  },

  _onMessageSearch({ actor, browser, data }) {
    this.performSearch(actor, browser, data);
  },

  _onMessageSetCurrentEngine({ data }) {
    lazy.SearchService.setDefault(
      lazy.SearchService.getEngineByName(data),
      lazy.SearchService.CHANGE_REASON.USER_SEARCHBAR
    );
  },

  _onMessageManageEngines({ browser }) {
    browser.documentGlobal.openPreferences("paneSearch");
  },

  async _onMessageGetSuggestions({ actor, browser, data }) {
    this._ensureDataHasProperties(data, ["engineName", "searchString"]);
    let { engineName, searchString } = data;
    let suggestions = await this.getSuggestions(
      engineName,
      searchString,
      browser
    );

    this._reply(actor, "Suggestions", {
      engineName: data.engineName,
      searchString: suggestions.term,
      formHistory: suggestions.local,
      remote: suggestions.remote,
    });
  },

  async _onMessageAddFormHistoryEntry({ browser, data: entry }) {
    await this.addFormHistoryEntry(browser, entry);
  },

  _onMessageRemoveFormHistoryEntry({ browser, data: entry }) {
    this.removeFormHistoryEntry(browser, entry);
  },

  _onMessageSpeculativeConnect({ browser, data: engineName }) {
    let engine = lazy.SearchService.getEngineByName(engineName);
    if (!engine) {
      throw new Error("Unknown engine name: " + engineName);
    }
    if (browser.contentWindow) {
      engine.speculativeConnect({
        window: browser.contentWindow,
        originAttributes: browser.contentPrincipal.originAttributes,
      });
    }
  },

  _onMessageSearchHandoff({ browser, data, actor }) {
    let win = browser.documentGlobal;
    let text = data.text;
    let urlBar = win.gURLBar;
    let inPrivateBrowsing = lazy.PrivateBrowsingUtils.isBrowserPrivate(browser);
    let searchEngine = inPrivateBrowsing
      ? lazy.SearchService.defaultPrivateEngine
      : lazy.SearchService.defaultEngine;
    let isFirstChange = true;

    let newtabSessionId = null;
    let newtabActor =
      browser.browsingContext?.currentWindowGlobal?.getExistingActor(
        "AboutNewTab"
      );
    if (newtabActor) {
      const portID = newtabActor.getTabDetails()?.portID;
      if (portID) {
        newtabSessionId = lazy.AboutNewTab.activityStream.store.feeds
          .get("feeds.telemetry")
          ?.sessions.get(portID)?.session_id;
      }
    }

    if (!text) {
      urlBar.setHiddenFocus();
    } else {
      urlBar.handoff(text, searchEngine, newtabSessionId);
      isFirstChange = false;
    }

    let checkFirstChange = () => {
      if (isFirstChange) {
        isFirstChange = false;
        urlBar.removeHiddenFocus(true);
        urlBar.handoff("", searchEngine, newtabSessionId);
        actor.sendAsyncMessage("DisableSearch");
        urlBar.removeEventListener("compositionstart", checkFirstChange);
        urlBar.removeEventListener("paste", checkFirstChange);
      }
    };

    let onKeydown = ev => {
      if (ev.key.length === 1 && !ev.altKey && !ev.ctrlKey && !ev.metaKey) {
        checkFirstChange();
      }
      if (ev.key === "Escape") {
        onDone();
      }
    };

    let onDone = ev => {
      const forceSuppressFocusBorder = ev?.type === "mousedown";
      urlBar.removeHiddenFocus(forceSuppressFocusBorder);

      urlBar.inputField.removeEventListener("keydown", onKeydown);
      urlBar.inputField.removeEventListener("mousedown", onDone);
      urlBar.inputField.removeEventListener("blur", onDone);
      urlBar.inputField.removeEventListener(
        "compositionstart",
        checkFirstChange
      );
      urlBar.inputField.removeEventListener("paste", checkFirstChange);

      actor.sendAsyncMessage("ShowSearch");
    };

    urlBar.inputField.addEventListener("keydown", onKeydown);
    urlBar.inputField.addEventListener("mousedown", onDone);
    urlBar.inputField.addEventListener("blur", onDone);
    urlBar.inputField.addEventListener("compositionstart", checkFirstChange);
    urlBar.inputField.addEventListener("paste", checkFirstChange);
  },

  async _onObserve(eventItem) {
    let engine;
    switch (eventItem.data) {
      case "engine-default":
        engine = await this._currentEngineObj(false);
        this._broadcast("CurrentEngine", engine);
        break;
      case "engine-default-private":
        engine = await this._currentEngineObj(true);
        this._broadcast("CurrentPrivateEngine", engine);
        break;
      case "shouldHandOffToSearchMode":
        this._broadcast(
          "HandoffSearchModePrefs",
          lazy.UrlbarPrefs.get("shouldHandOffToSearchMode")
        );
        break;
      default: {
        let state = await this.currentStateObj();
        this._broadcast("CurrentState", state);
        break;
      }
    }
  },

  _suggestionDataForBrowser(browser, create = false) {
    let data = this._suggestionMap.get(browser);
    if (!data && create) {
      data = {
        controller: new lazy.SearchSuggestionController(),
      };
      this._suggestionMap.set(browser, data);
    }
    return data;
  },

  _reply(actor, type, data) {
    actor.sendAsyncMessage(type, data);
  },

  _broadcast(type, data) {
    for (let actor of gContentSearchActors) {
      actor.sendAsyncMessage(type, data);
    }
  },

  async _currentEngineObj(usePrivate) {
    let engine = usePrivate
      ? await lazy.SearchService.getDefaultPrivate()
      : await lazy.SearchService.getDefault();
    return {
      name: engine.name,
      iconData: await this._getEngineIconURL(engine),
      isConfigEngine: engine instanceof lazy.ConfigSearchEngine,
    };
  },


  async _getEngineIconURL(engine) {
    let url = await engine.getIconURL();
    if (!url) {
      return SEARCH_ENGINE_PLACEHOLDER_ICON;
    }

    if (!url.startsWith("data:") && !url.startsWith("blob:")) {
      return url;
    }

    try {
      const response = await fetch(url);
      const mimeType = response.headers.get("Content-Type") || "";
      const data = await response.arrayBuffer();
      return { icon: data, mimeType };
    } catch (err) {
      console.error("Fetch error: ", err);
      return SEARCH_ENGINE_PLACEHOLDER_ICON;
    }
  },

  _ensureDataHasProperties(data, requiredProperties) {
    for (let prop of requiredProperties) {
      if (!(prop in data)) {
        throw new Error("Message data missing required property: " + prop);
      }
    }
  },

  _initService() {
    if (!this._initServicePromise) {
      this._initServicePromise = lazy.SearchService.init();
    }
    return this._initServicePromise;
  },
};

export class ContentSearchParent extends JSWindowActorParent {
  constructor() {
    super();
    ContentSearch.init();
    gContentSearchActors.add(this);
  }

  didDestroy() {
    gContentSearchActors.delete(this);
  }

  receiveMessage(msg) {
    let browser = this.browsingContext.top.embedderElement;
    if (!browser) {
      return;
    }
    let eventItem = {
      type: "Message",
      name: msg.name,
      data: msg.data,
      browser,
      actor: this,
      handleEvent: event => {
        let browserData = ContentSearch._suggestionMap.get(eventItem.browser);
        if (browserData) {
          ContentSearch._suggestionMap.delete(eventItem.browser);
          ContentSearch._suggestionMap.set(event.detail, browserData);
        }
        browser.removeEventListener("SwapDocShells", eventItem, true);
        eventItem.browser = event.detail;
        eventItem.browser.addEventListener("SwapDocShells", eventItem, true);
      },
    };
    browser.addEventListener("SwapDocShells", eventItem, true);

    if (msg.name === "Search") {
      ContentSearch._cancelSuggestions(eventItem);
    }

    ContentSearch._eventQueue.push(eventItem);
    ContentSearch._processEventQueue();
  }
}
