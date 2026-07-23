/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const FORMAT_VERSION = 1;

const PERSIST_SESSIONS = Services.prefs.getBoolPref(
  "browser.sessionstore.persist_closed_tabs_between_sessions"
);
const TAB_CUSTOM_VALUES = new WeakMap();
const TAB_LAZY_STATES = new WeakMap();
const TAB_STATE_NEEDS_RESTORE = 1;
const TAB_STATE_RESTORING = 2;
const TAB_STATE_FOR_BROWSER = new WeakMap();
const WINDOW_RESTORE_IDS = new WeakMap();
const WINDOW_RESTORE_ZINDICES = new WeakMap();
const WINDOW_SHOWING_PROMISES = new Map();
const WINDOW_FLUSHING_PROMISES = new Map();

const NOTIFY_SINGLE_WINDOW_RESTORED = "sessionstore-single-window-restored";
const NOTIFY_WINDOWS_RESTORED = "sessionstore-windows-restored";
const NOTIFY_BROWSER_STATE_RESTORED = "sessionstore-browser-state-restored";
const NOTIFY_LAST_SESSION_CLEARED = "sessionstore-last-session-cleared";
const NOTIFY_LAST_SESSION_RE_ENABLED = "sessionstore-last-session-re-enable";
const NOTIFY_RESTORING_ON_STARTUP = "sessionstore-restoring-on-startup";
const NOTIFY_INITIATING_MANUAL_RESTORE =
  "sessionstore-initiating-manual-restore";
const NOTIFY_CLOSED_OBJECTS_CHANGED = "sessionstore-closed-objects-changed";
const NOTIFY_SAVED_TAB_GROUPS_CHANGED = "sessionstore-saved-tab-groups-changed";

const NOTIFY_TAB_RESTORED = "sessionstore-debug-tab-restored"; 
const NOTIFY_DOMWINDOWCLOSED_HANDLED =
  "sessionstore-debug-domwindowclosed-handled"; 

const NOTIFY_BROWSER_SHUTDOWN_FLUSH = "sessionstore-browser-shutdown-flush";

const MAX_CONCURRENT_TAB_RESTORES = 3;

const MIN_SCREEN_EDGE_SLOP = 8;

const OBSERVING = [
  "browser-window-before-show",
  "domwindowclosed",
  "quit-application-granted",
  "browser-lastwindow-close-granted",
  "quit-application",
  "browser:purge-session-history",
  "browser:purge-session-history-for-domain",
  "idle-daily",
  "clear-origin-attributes-data",
  "browsing-context-did-set-embedder",
  "browsing-context-discarded",
  "browser-shutdown-tabstate-updated",
];

const WINDOW_ATTRIBUTES = ["width", "height", "screenX", "screenY", "sizemode"];

const CHROME_FLAGS_MAP = [
  [Ci.nsIWebBrowserChrome.CHROME_TITLEBAR, "titlebar"],
  [Ci.nsIWebBrowserChrome.CHROME_WINDOW_CLOSE, "close"],
  [Ci.nsIWebBrowserChrome.CHROME_TOOLBAR, "toolbar"],
  [Ci.nsIWebBrowserChrome.CHROME_LOCATIONBAR, "location"],
  [Ci.nsIWebBrowserChrome.CHROME_PERSONAL_TOOLBAR, "personalbar"],
  [Ci.nsIWebBrowserChrome.CHROME_STATUSBAR, "status"],
  [Ci.nsIWebBrowserChrome.CHROME_MENUBAR, "menubar"],
  [Ci.nsIWebBrowserChrome.CHROME_WINDOW_RESIZE, "resizable"],
  [Ci.nsIWebBrowserChrome.CHROME_WINDOW_MINIMIZE, "minimizable"],
  [Ci.nsIWebBrowserChrome.CHROME_SCROLLBARS, "", "scrollbars=0"],
  [Ci.nsIWebBrowserChrome.CHROME_PRIVATE_WINDOW, "private"],
  [Ci.nsIWebBrowserChrome.CHROME_NON_PRIVATE_WINDOW, "non-private"],
  [Ci.nsIWebBrowserChrome.CHROME_ALWAYS_ON_TOP, "alwaysontop"],
  [Ci.nsIWebBrowserChrome.CHROME_EXTRA, "extrachrome"],
  [Ci.nsIWebBrowserChrome.CHROME_CENTER_SCREEN, "centerscreen"],
  [Ci.nsIWebBrowserChrome.CHROME_DEPENDENT, "dependent"],
  [Ci.nsIWebBrowserChrome.CHROME_MODAL, "modal"],
  [Ci.nsIWebBrowserChrome.CHROME_OPENAS_DIALOG, "dialog", "dialog=0"],
];

const WINDOW_HIDEABLE_FEATURES = [
  "menubar",
  "toolbar",
  "locationbar",
  "personalbar",
  "statusbar",
  "scrollbars",
];

const WINDOW_OPEN_FEATURES_MAP = {
  locationbar: "location",
  statusbar: "status",
};

const TAB_EVENTS = [
  "TabOpen",
  "TabBrowserInserted",
  "TabClose",
  "TabSelect",
  "TabShow",
  "TabHide",
  "TabPinned",
  "TabUnpinned",
  "TabGroupCreate",
  "TabGroupRemoveRequested",
  "TabGroupRemoved",
  "TabGrouped",
  "TabUngrouped",
  "TabGroupCollapse",
  "TabGroupExpand",
  "TabSplitViewActivate",
  "SplitViewRemoved",
  "SplitViewCreated",
];

const XUL_NS = "http://www.mozilla.org/keymaster/gatekeeper/there.is.only.xul";

const RESTORE_TAB_CONTENT_REASON = {
  SET_STATE: 0,
  NAVIGATE_AND_RESTORE: 1,
};

const BROWSER_STARTUP_RESUME_SESSION = 3;

const kNoIndex = Number.MAX_SAFE_INTEGER;
const kLastIndex = Number.MAX_SAFE_INTEGER - 1;

import { PrivateBrowsingUtils } from "resource://gre/modules/PrivateBrowsingUtils.sys.mjs";

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { GlobalState } from "resource:///modules/sessionstore/GlobalState.sys.mjs";

const lazy = {};

XPCOMUtils.defineLazyServiceGetters(lazy, {
  gScreenManager: ["@mozilla.org/gfx/screenmanager;1", Ci.nsIScreenManager],
});

ChromeUtils.defineESModuleGetters(lazy, {

  AsyncShutdown: "resource://gre/modules/AsyncShutdown.sys.mjs",
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  E10SUtils: "resource://gre/modules/E10SUtils.sys.mjs",
  HomePage: "resource:///modules/HomePage.sys.mjs",
  JsonSchema: "resource://gre/modules/JsonSchema.sys.mjs",
  PrivacyFilter: "resource://gre/modules/sessionstore/PrivacyFilter.sys.mjs",
  sessionStoreLogger: "resource:///modules/sessionstore/SessionLogger.sys.mjs",
  RunState: "resource:///modules/sessionstore/RunState.sys.mjs",
  SessionCookies: "resource:///modules/sessionstore/SessionCookies.sys.mjs",
  SessionFile: "resource:///modules/sessionstore/SessionFile.sys.mjs",
  SessionHistory: "resource://gre/modules/sessionstore/SessionHistory.sys.mjs",
  SessionSaver: "resource:///modules/sessionstore/SessionSaver.sys.mjs",
  SessionStartup: "resource:///modules/sessionstore/SessionStartup.sys.mjs",
  SessionStoreHelper:
    "resource://gre/modules/sessionstore/SessionStoreHelper.sys.mjs",
  TabAttributes: "resource:///modules/sessionstore/TabAttributes.sys.mjs",
  TabGroupState: "resource:///modules/sessionstore/TabGroupState.sys.mjs",
  TabState: "resource:///modules/sessionstore/TabState.sys.mjs",
  TabStateCache: "resource:///modules/sessionstore/TabStateCache.sys.mjs",
  TabStateFlusher: "resource:///modules/sessionstore/TabStateFlusher.sys.mjs",
  setTimeout: "resource://gre/modules/Timer.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "blankURI", () => {
  return Services.io.newURI("about:blank");
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "gRestoreWindowsToVirtualDesktop",
  "browser.sessionstore.restore_windows_to_virtual_desktop"
);

var gDebuggingEnabled = false;

export var SessionStore = {
  get logger() {
    return SessionStoreInternal._log;
  },
  get promiseInitialized() {
    return SessionStoreInternal.promiseInitialized;
  },

  get promiseAllWindowsRestored() {
    return SessionStoreInternal.promiseAllWindowsRestored;
  },

  get canRestoreLastSession() {
    return SessionStoreInternal.canRestoreLastSession;
  },

  set canRestoreLastSession(val) {
    SessionStoreInternal.canRestoreLastSession = val;
  },

  get lastClosedObjectType() {
    return SessionStoreInternal.lastClosedObjectType;
  },

  get lastClosedActions() {
    return [...SessionStoreInternal._lastClosedActions];
  },

  get LAST_ACTION_CLOSED_TAB() {
    return SessionStoreInternal._LAST_ACTION_CLOSED_TAB;
  },

  get LAST_ACTION_CLOSED_WINDOW() {
    return SessionStoreInternal._LAST_ACTION_CLOSED_WINDOW;
  },

  get savedGroups() {
    return SessionStoreInternal._savedGroups;
  },

  get willAutoRestore() {
    return SessionStoreInternal.willAutoRestore;
  },

  get shouldRestoreLastSession() {
    return SessionStoreInternal._shouldRestoreLastSession;
  },

  init: function ss_init() {
    SessionStoreInternal.init();
  },

  getWindows(aWindowOrOptions) {
    return SessionStoreInternal.getWindows(aWindowOrOptions);
  },

  getWindowForTabClosedId(aClosedId, aIncludePrivate) {
    return SessionStoreInternal.getWindowForTabClosedId(
      aClosedId,
      aIncludePrivate
    );
  },

  getWindowId(aWindow) {
    return aWindow.__SSi ?? null;
  },

  getBrowserState: function ss_getBrowserState() {
    return SessionStoreInternal.getBrowserState();
  },

  setBrowserState: function ss_setBrowserState(aState) {
    SessionStoreInternal.setBrowserState(aState);
  },

  getWindowState: function ss_getWindowState(aWindow) {
    return SessionStoreInternal.getWindowState(aWindow);
  },

  setWindowState: function ss_setWindowState(aWindow, aState, aOverwrite) {
    SessionStoreInternal.setWindowState(aWindow, aState, aOverwrite);
  },

  getTabState: function ss_getTabState(aTab) {
    return SessionStoreInternal.getTabState(aTab);
  },

  setTabState: function ss_setTabState(aTab, aState) {
    SessionStoreInternal.setTabState(aTab, aState);
  },

  isTabRestoring(aTab) {
    return TAB_STATE_FOR_BROWSER.has(aTab.linkedBrowser);
  },

  getInternalObjectState(obj) {
    return SessionStoreInternal.getInternalObjectState(obj);
  },

  duplicateTab: function ss_duplicateTab(
    aWindow,
    aTab,
    aDelta = 0,
    aRestoreImmediately = true,
    aOptions = {}
  ) {
    return SessionStoreInternal.duplicateTab(
      aWindow,
      aTab,
      aDelta,
      aRestoreImmediately,
      aOptions
    );
  },

  getLastClosedTabCount(aWindow) {
    return SessionStoreInternal.getLastClosedTabCount(aWindow);
  },

  resetLastClosedTabCount(aWindow) {
    SessionStoreInternal.resetLastClosedTabCount(aWindow);
  },

  getClosedTabCountForWindow: function ss_getClosedTabCountForWindow(aWindow) {
    return SessionStoreInternal.getClosedTabCountForWindow(aWindow);
  },

  getClosedTabCount: function ss_getClosedTabCount(aOptions) {
    return SessionStoreInternal.getClosedTabCount(aOptions);
  },

  getClosedTabCountFromClosedWindows:
    function ss_getClosedTabCountFromClosedWindows() {
      return SessionStoreInternal.getClosedTabCountFromClosedWindows();
    },

  getClosedTabDataForWindow: function ss_getClosedTabDataForWindow(aWindow) {
    return SessionStoreInternal.getClosedTabDataForWindow(aWindow);
  },

  getClosedTabData: function ss_getClosedTabData(aOptions) {
    return SessionStoreInternal.getClosedTabData(aOptions);
  },

  getClosedTabDataFromClosedWindows:
    function ss_getClosedTabDataFromClosedWindows() {
      return SessionStoreInternal.getClosedTabDataFromClosedWindows();
    },

  getClosedTabGroups: function ss_getClosedTabGroups(aOptions) {
    return SessionStoreInternal.getClosedTabGroups(aOptions);
  },

  getLastClosedTabGroupId(window) {
    return SessionStoreInternal.getLastClosedTabGroupId(window);
  },

  undoCloseTab: function ss_undoCloseTab(aSource, aIndex, aTargetWindow) {
    return SessionStoreInternal.undoCloseTab(aSource, aIndex, aTargetWindow);
  },

  undoClosedTabFromClosedWindow: function ss_undoClosedTabFromClosedWindow(
    aSource,
    aClosedId,
    aTargetWindow
  ) {
    return SessionStoreInternal.undoClosedTabFromClosedWindow(
      aSource,
      aClosedId,
      aTargetWindow
    );
  },

  forgetClosedTab: function ss_forgetClosedTab(aSource, aIndex) {
    return SessionStoreInternal.forgetClosedTab(aSource, aIndex);
  },

  forgetClosedTabGroup: function ss_forgetClosedTabGroup(aSource, tabGroupId) {
    return SessionStoreInternal.forgetClosedTabGroup(aSource, tabGroupId);
  },

  forgetClosedTabById: function ss_forgetClosedTabById(
    aClosedId,
    aSourceOptions
  ) {
    SessionStoreInternal.forgetClosedTabById(aClosedId, aSourceOptions);
  },

  forgetClosedWindowById: function ss_forgetClosedWindowById(aClosedId) {
    SessionStoreInternal.forgetClosedWindowById(aClosedId);
  },

  getObjectTypeForClosedId(aClosedId) {
    return SessionStoreInternal.getObjectTypeForClosedId(aClosedId);
  },

  getWindowById: function ss_getWindowById(aSessionStoreId) {
    return SessionStoreInternal.getWindowById(aSessionStoreId);
  },

  getClosedWindowCount: function ss_getClosedWindowCount() {
    return SessionStoreInternal.getClosedWindowCount();
  },

  popLastClosedAction: function ss_popLastClosedAction() {
    return SessionStoreInternal._lastClosedActions.pop();
  },

  resetLastClosedActions: function ss_resetLastClosedActions() {
    SessionStoreInternal._lastClosedActions = [];
  },

  getClosedWindowData: function ss_getClosedWindowData() {
    return SessionStoreInternal.getClosedWindowData();
  },

  maybeDontRestoreTabs(aWindow) {
    SessionStoreInternal.maybeDontRestoreTabs(aWindow);
  },

  undoCloseWindow: function ss_undoCloseWindow(aIndex) {
    return SessionStoreInternal.undoCloseWindow(aIndex);
  },

  forgetClosedWindow: function ss_forgetClosedWindow(aIndex) {
    return SessionStoreInternal.forgetClosedWindow(aIndex);
  },

  getCustomWindowValue(aWindow, aKey) {
    return SessionStoreInternal.getCustomWindowValue(aWindow, aKey);
  },

  setCustomWindowValue(aWindow, aKey, aStringValue) {
    SessionStoreInternal.setCustomWindowValue(aWindow, aKey, aStringValue);
  },

  deleteCustomWindowValue(aWindow, aKey) {
    SessionStoreInternal.deleteCustomWindowValue(aWindow, aKey);
  },

  getCustomTabValue(aTab, aKey) {
    return SessionStoreInternal.getCustomTabValue(aTab, aKey);
  },

  setCustomTabValue(aTab, aKey, aStringValue) {
    SessionStoreInternal.setCustomTabValue(aTab, aKey, aStringValue);
  },

  deleteCustomTabValue(aTab, aKey) {
    SessionStoreInternal.deleteCustomTabValue(aTab, aKey);
  },

  getLazyTabValue(aTab, aKey) {
    return SessionStoreInternal.getLazyTabValue(aTab, aKey);
  },

  getCustomGlobalValue(aKey) {
    return SessionStoreInternal.getCustomGlobalValue(aKey);
  },

  setCustomGlobalValue(aKey, aStringValue) {
    SessionStoreInternal.setCustomGlobalValue(aKey, aStringValue);
  },

  deleteCustomGlobalValue(aKey) {
    SessionStoreInternal.deleteCustomGlobalValue(aKey);
  },

  restoreLastSession: function ss_restoreLastSession() {
    SessionStoreInternal.restoreLastSession();
  },

  speculativeConnectOnTabHover(tab) {
    SessionStoreInternal.speculativeConnectOnTabHover(tab);
  },

  getCurrentState(aUpdateAll) {
    return SessionStoreInternal.getCurrentState(aUpdateAll);
  },

  reviveCrashedTab(aTab) {
    return SessionStoreInternal.reviveCrashedTab(aTab);
  },

  reviveAllCrashedTabs() {
    return SessionStoreInternal.reviveAllCrashedTabs();
  },

  updateSessionStoreFromTablistener(
    aBrowser,
    aBrowsingContext,
    aPermanentKey,
    aData,
    aForStorage
  ) {
    return SessionStoreInternal.updateSessionStoreFromTablistener(
      aBrowser,
      aBrowsingContext,
      aPermanentKey,
      aData,
      aForStorage
    );
  },

  getSessionHistory(tab, updatedCallback) {
    return SessionStoreInternal.getSessionHistory(tab, updatedCallback);
  },

  undoCloseById(aClosedId, aIncludePrivate, aTargetWindow) {
    return SessionStoreInternal.undoCloseById(
      aClosedId,
      aIncludePrivate,
      aTargetWindow
    );
  },

  resetBrowserToLazyState(tab) {
    return SessionStoreInternal.resetBrowserToLazyState(tab);
  },

  isBrowserInCrashedSet(browser) {
    return SessionStoreInternal.isBrowserInCrashedSet(browser);
  },

  getNextSplitViewId() {
    if (SessionStoreInternal._maxSplitViewId >= Number.MAX_SAFE_INTEGER) {
      throw new Error("Maximum _maxSplitViewId exceeded");
    }
    return ++SessionStoreInternal._maxSplitViewId;
  },

  resetNextClosedId() {
    SessionStoreInternal._nextClosedId = 0;
  },

  ensureInitialized(window) {
    if (SessionStoreInternal._sessionInitialized && !window.__SSi) {
      SessionStoreInternal.onLoad(window);
    }
  },

  getCurrentEpoch(browser) {
    return SessionStoreInternal.getCurrentEpoch(browser.permanentKey);
  },

  isFormatVersionCompatible(version) {
    if (!version) {
      return false;
    }
    if (!Array.isArray(version)) {
      return false;
    }
    if (version[0] != "sessionrestore") {
      return false;
    }
    let number = Number.parseFloat(version[1]);
    if (Number.isNaN(number)) {
      return false;
    }
    return number <= FORMAT_VERSION;
  },

  keepOnlyWorthSavingTabs(aState) {
    let closedWindowShouldRestore = null;
    for (let i = aState.windows.length - 1; i >= 0; i--) {
      let win = aState.windows[i];
      for (let j = win.tabs.length - 1; j >= 0; j--) {
        let tab = win.tabs[j];
        if (!SessionStoreInternal._shouldSaveTab(tab)) {
          win.tabs.splice(j, 1);
          if (win.selected > j) {
            win.selected--;
          }
        }
      }

      if (
        !win.tabs.length &&
        (aState.windows.length > 1 ||
          closedWindowShouldRestore ||
          (closedWindowShouldRestore == null &&
            (closedWindowShouldRestore = aState._closedWindows.some(
              w => w._shouldRestore
            ))))
      ) {
        aState.windows.splice(i, 1);
        if (aState.selectedWindow > i) {
          aState.selectedWindow--;
        }
      }
    }
  },

  purgeDataForPrivateWindow(win) {
    return SessionStoreInternal.purgeDataForPrivateWindow(win);
  },

  addSavedTabGroup(tabGroup) {
    return SessionStoreInternal.addSavedTabGroup(tabGroup);
  },

  addTabsToSavedGroup(tabGroupId, tabs) {
    return SessionStoreInternal.addTabsToSavedGroup(tabGroupId, tabs);
  },

  getSavedTabGroup(tabGroupId) {
    return SessionStoreInternal.getSavedTabGroup(tabGroupId);
  },

  getSavedTabGroups() {
    return SessionStoreInternal.getSavedTabGroups();
  },

  forgetSavedTabGroup(tabGroupId) {
    return SessionStoreInternal.forgetSavedTabGroup(tabGroupId);
  },

  undoCloseTabGroup(source, tabGroupId, targetWindow) {
    return SessionStoreInternal.undoCloseTabGroup(
      source,
      tabGroupId,
      targetWindow
    );
  },

  openSavedTabGroup(tabGroupId, targetWindow) {
    return SessionStoreInternal.openSavedTabGroup(tabGroupId, targetWindow);
  },

  shouldSaveTabsToGroup(tabs) {
    return SessionStoreInternal.shouldSaveTabsToGroup(tabs);
  },

  formatTabStateForSavedGroup(tab) {
    return SessionStoreInternal._formatTabStateForSavedGroup(tab);
  },

  validateState(state) {
    return SessionStoreInternal.validateState(state);
  },
};

Object.freeze(SessionStore);

var SessionStoreInternal = {
  QueryInterface: ChromeUtils.generateQI([
    "nsIObserver",
    "nsISupportsWeakReference",
  ]),

  _globalState: new GlobalState(),

  _nextClosedId: 0,

  _restoreCount: -1,

  _browserSHistoryListener: new WeakMap(),

  _restoreListeners: new WeakMap(),

  _tabStateRestorePromises: new WeakMap(),

  _tabStateToRestore: new WeakMap(),

  _browserEpochs: new WeakMap(),

  _crashedBrowsers: new WeakSet(),

  _closingTabMap: new WeakMap(),

  _tabClosingByWindowMap: new WeakMap(),

  _saveableClosedWindowData: new WeakSet(),

  _browserSetState: false,

  _sessionStartTime: Date.now(),

  _windows: {},

  _nextWindowID: 0,

  _maxSplitViewId: 0,

  _closedWindows: [],

  _savedGroups: [],

  _statesToRestore: {},

  _recentCrashes: 0,

  _restoreLastWindow: false,

  _shouldRestoreLastSession: false,

  _restoreWithoutRestart: false,

  _tabsRestoringCount: 0,


  _lastClosedActions: [],

  _removeClosedAction(closedAction, closedId) {
    let closedActionIndex = this._lastClosedActions.findIndex(
      obj => obj.type == closedAction && obj.closedId == closedId
    );

    if (closedActionIndex > -1) {
      this._lastClosedActions.splice(closedActionIndex, 1);
    }
  },

  _addClosedAction(closedAction, closedId) {
    this._lastClosedActions.push({
      type: closedAction,
      closedId,
    });
    let maxLength = this._max_tabs_undo * this._max_windows_undo;

    if (this._lastClosedActions.length > maxLength) {
      this._lastClosedActions = this._lastClosedActions.slice(-maxLength);
    }
  },

  _LAST_ACTION_CLOSED_TAB: "tab",

  _LAST_ACTION_CLOSED_WINDOW: "window",

  _log: null,

  _deferredInitialState: null,

  _closedObjectsChanged: false,

  _deferredInitialized: Promise.withResolvers(),

  _sessionInitialized: false,

  _deferredAllWindowsRestored: Promise.withResolvers(),

  get promiseAllWindowsRestored() {
    return this._deferredAllWindowsRestored.promise;
  },

  _promiseReadyForInitialization: null,

  _windowBusyStates: new WeakMap(),

  get promiseInitialized() {
    return this._deferredInitialized.promise;
  },

  get canRestoreLastSession() {
    return LastSession.canRestore;
  },

  set canRestoreLastSession(val) {
    if (!val) {
      LastSession.clear();
    }
  },

  get lastClosedObjectType() {
    if (this._closedWindows.length) {
      let tabTimestamps = [];
      for (let window of Services.wm.getEnumerator("navigator:browser")) {
        let windowState = this._windows[window.__SSi];
        if (windowState && windowState._closedTabs[0]) {
          tabTimestamps.push(windowState._closedTabs[0].closedAt);
        }
      }
      if (
        !tabTimestamps.length ||
        tabTimestamps.sort((a, b) => b - a)[0] < this._closedWindows[0].closedAt
      ) {
        return this._LAST_ACTION_CLOSED_WINDOW;
      }
    }
    return this._LAST_ACTION_CLOSED_TAB;
  },

  get willAutoRestore() {
    return (
      !PrivateBrowsingUtils.permanentPrivateBrowsing &&
      (Services.prefs.getBoolPref("browser.sessionstore.resume_session_once") ||
        Services.prefs.getIntPref("browser.startup.page") ==
          BROWSER_STARTUP_RESUME_SESSION)
    );
  },

  init() {
    if (this._initialized) {
      throw new Error("SessionStore.init() must only be called once!");
    }

    OBSERVING.forEach(function (aTopic) {
      Services.obs.addObserver(this, aTopic, true);
    }, this);

    this._initPrefs();
    this._initialized = true;

    this.promiseAllWindowsRestored.finally(() => () => {
      this._log.debug("promiseAllWindowsRestored finalized");
    });
  },

  initSession() {
    let state;
    let ss = lazy.SessionStartup;
    let willRestore = ss.willRestore();
    if (willRestore || ss.sessionType == ss.DEFER_SESSION) {
      state = ss.state;
    }
    this._log.debug(
      `initSession willRestore: ${willRestore}, SessionStartup.sessionType: ${ss.sessionType}`
    );

    if (state) {
      this._initSplitViewIds(state);

      try {
        if (ss.sessionType == ss.DEFER_SESSION) {
          let [iniState, remainingState] =
            this._prepDataForDeferredRestore(state);
          if (iniState.windows.length || iniState.savedGroups) {
            state = iniState;
          } else {
            state = null;
          }
          this._log.debug(
            `initSession deferred restore with ${iniState.windows.length} initial windows, ${remainingState.windows.length} remaining windows`
          );

          if (remainingState.windows.length) {
            LastSession.setState(remainingState);
          }
        } else {
          LastSession.setState(state.lastSessionState);

          let restoreAsCrashed = ss.willRestoreAsCrashed();
          if (restoreAsCrashed) {
            this._recentCrashes =
              ((state.session && state.session.recentCrashes) || 0) + 1;
            this._log.debug(
              `initSession, restoreAsCrashed, crashes: ${this._recentCrashes}`
            );

            if (this._needsRestorePage(state, this._recentCrashes)) {
              let url = "about:sessionrestore";
              let formdata = { id: { sessionData: state }, url };
              let entry = {
                url,
                triggeringPrincipal_base64:
                  lazy.E10SUtils.SERIALIZED_SYSTEMPRINCIPAL,
              };
              state = { windows: [{ tabs: [{ entries: [entry], formdata }] }] };
              this._log.debug("initSession, will show about:sessionrestore");
            } else if (
              this._hasSingleTabWithURL(state.windows, "about:welcomeback")
            ) {
              this._log.debug("initSession, will show about:welcomeback");
              state.windows[0].tabs[0].entries[0].url = "about:sessionrestore";
              state.windows[0].tabs[0].entries[0].triggeringPrincipal_base64 =
                lazy.E10SUtils.SERIALIZED_SYSTEMPRINCIPAL;
            } else {
              restoreAsCrashed = false;
            }
          }

          if (!restoreAsCrashed) {
            this._log.debug("initSession, will autorestore");
            this._removeExplicitlyClosedTabs(state);
          }

          this._updateSessionStartTime(state);

          if (state.windows.length) {
            delete state.windows[0].hidden;
            delete state.windows[0].isPopup;
            if (state.windows[0].sizemode == "minimized") {
              state.windows[0].sizemode = "normal";
            }
          }

          state.windows.forEach(function (aWindow) {
            delete aWindow.__lastSessionWindowID;
          });
        }

        state?.windows?.forEach(win => delete win._maybeDontRestoreTabs);
        state?._closedWindows?.forEach(win => delete win._maybeDontRestoreTabs);

        this._savedGroups = state?.savedGroups ?? [];
      } catch (ex) {
        this._log.error("The session file is invalid: ", ex);
      }
    }

    if (
      !lazy.RunState.isQuitting &&
      this._prefBranch.getBoolPref("sessionstore.resume_session_once")
    ) {
      this._prefBranch.setBoolPref("sessionstore.resume_session_once", false);
    }

    return state;
  },

  _removeExplicitlyClosedTabs(state) {
    for (let i = 0; i < state.windows.length; ) {
      const winData = state.windows[i];
      if (winData._maybeDontRestoreTabs) {
        if (state.windows.length == 1) {
          let j = 0;
          winData._lastClosedTabGroupCount = -1;
          while (winData.tabs.length) {
            const tabState = winData.tabs.pop();

            let activeIndex = (tabState.index || tabState.entries.length) - 1;
            activeIndex = Math.min(activeIndex, tabState.entries.length - 1);
            activeIndex = Math.max(activeIndex, 0);

            let title = "";
            if (activeIndex in tabState.entries) {
              title =
                tabState.entries[activeIndex].title ||
                tabState.entries[activeIndex].url;
            }

            const tabData = {
              state: tabState,
              title,
              image: tabState.image,
              pos: j++,
              closedAt: Date.now(),
              closedInGroup: true,
            };
            if (this._shouldSaveTabState(tabState)) {
              this.saveClosedTabData(winData, winData._closedTabs, tabData);
            }
          }
        } else {
          if (winData.tabs.some(this._shouldSaveTabState)) {
            winData.closedAt = Date.now();
            state._closedWindows.unshift(winData);
          }
          state.windows.splice(i, 1);
          continue; 
        }
      }
      i++;
    }
  },

  _initPrefs() {
    this._prefBranch = Services.prefs.getBranch("browser.");

    gDebuggingEnabled = this._prefBranch.getBoolPref("sessionstore.debug");

    Services.prefs.addObserver("browser.sessionstore.debug", () => {
      gDebuggingEnabled = this._prefBranch.getBoolPref("sessionstore.debug");
    });

    this._log = lazy.sessionStoreLogger;

    this._max_tabs_undo = this._prefBranch.getIntPref(
      "sessionstore.max_tabs_undo"
    );
    this._prefBranch.addObserver("sessionstore.max_tabs_undo", this, true);

    this._closedTabsFromAllWindowsEnabled = this._prefBranch.getBoolPref(
      "sessionstore.closedTabsFromAllWindows"
    );
    this._prefBranch.addObserver(
      "sessionstore.closedTabsFromAllWindows",
      this,
      true
    );

    this._closedTabsFromClosedWindowsEnabled = this._prefBranch.getBoolPref(
      "sessionstore.closedTabsFromClosedWindows"
    );
    this._prefBranch.addObserver(
      "sessionstore.closedTabsFromClosedWindows",
      this,
      true
    );

    this._max_windows_undo = this._prefBranch.getIntPref(
      "sessionstore.max_windows_undo"
    );
    this._prefBranch.addObserver("sessionstore.max_windows_undo", this, true);

    this._restore_on_demand = this._prefBranch.getBoolPref(
      "sessionstore.restore_on_demand"
    );
    this._prefBranch.addObserver("sessionstore.restore_on_demand", this, true);
  },

  _uninit: function ssi_uninit() {
    if (!this._initialized) {
      throw new Error("SessionStore is not initialized.");
    }

    lazy.RunState.setClosing();

    if (this._sessionInitialized) {
      lazy.SessionSaver.run();
    }

    TabRestoreQueue.reset();

    lazy.SessionSaver.cancel();
  },

  observe: function ssi_observe(aSubject, aTopic, aData) {
    switch (aTopic) {
      case "browser-window-before-show": 
        this.onBeforeBrowserWindowShown(aSubject);
        break;
      case "domwindowclosed": 
        this.onClose(aSubject).then(() => {
          this._notifyOfClosedObjectsChange();
        });
        if (gDebuggingEnabled) {
          Services.obs.notifyObservers(null, NOTIFY_DOMWINDOWCLOSED_HANDLED);
        }
        break;
      case "quit-application-granted": {
        let syncShutdown = aData == "syncShutdown";
        this.onQuitApplicationGranted(syncShutdown);
        break;
      }
      case "browser-lastwindow-close-granted":
        this.onLastWindowCloseGranted();
        break;
      case "quit-application":
        this.onQuitApplication(aData);
        break;
      case "browser:purge-session-history": 
        this.onPurgeSessionHistory();
        this._notifyOfClosedObjectsChange();
        break;
      case "browser:purge-session-history-for-domain":
        this.onPurgeDomainData(aData);
        this._notifyOfClosedObjectsChange();
        break;
      case "nsPref:changed": 
        this.onPrefChange(aData);
        this._notifyOfClosedObjectsChange();
        break;
      case "idle-daily":
        this.onIdleDaily();
        this._notifyOfClosedObjectsChange();
        break;
      case "clear-origin-attributes-data": {
        let userContextId = 0;
        try {
          userContextId = JSON.parse(aData).userContextId;
        } catch (e) {}
        if (userContextId) {
          this._forgetTabsWithUserContextId(userContextId);
        }
        break;
      }
      case "browsing-context-did-set-embedder":
        if (aSubject === aSubject.top && aSubject.isContent) {
          const permanentKey = aSubject.embedderElement?.permanentKey;
          if (permanentKey) {
            this.maybeRecreateSHistoryListener(permanentKey, aSubject);
          }
        }
        break;
      case "browsing-context-discarded": {
        let permanentKey = aSubject?.embedderElement?.permanentKey;
        if (permanentKey) {
          this._browserSHistoryListener.get(permanentKey)?.unregister();
        }
        break;
      }
      case "browser-shutdown-tabstate-updated":
        this.onFinalTabStateUpdateComplete(aSubject);
        this._notifyOfClosedObjectsChange();
        break;
    }
  },

  getOrCreateSHistoryListener(permanentKey, browsingContext) {
    if (!permanentKey || browsingContext !== browsingContext.top) {
      return null;
    }

    const listener = this._browserSHistoryListener.get(permanentKey);
    if (listener) {
      return listener;
    }

    return this.createSHistoryListener(permanentKey, browsingContext, false);
  },

  maybeRecreateSHistoryListener(permanentKey, browsingContext) {
    const listener = this._browserSHistoryListener.get(permanentKey);
    if (!listener || listener._browserId != browsingContext.browserId) {
      listener?.unregister(permanentKey);
      this.createSHistoryListener(permanentKey, browsingContext, true);
    }
  },

  createSHistoryListener(permanentKey, browsingContext, collectImmediately) {
    class SHistoryListener {
      constructor() {
        this.QueryInterface = ChromeUtils.generateQI([
          "nsISHistoryListener",
          "nsISupportsWeakReference",
        ]);

        this._browserId = browsingContext.browserId;
        this._fromIndex = kNoIndex;
      }

      unregister() {
        let bc = BrowsingContext.getCurrentTopByBrowserId(this._browserId);
        bc?.sessionHistory?.removeSHistoryListener(this);
        SessionStoreInternal._browserSHistoryListener.delete(permanentKey);
      }

      collect(
        permanentKey, // eslint-disable-line no-shadow
        browsingContext, // eslint-disable-line no-shadow
        { collectFull = true, writeToCache = false }
      ) {
        if (!collectFull && this._fromIndex === kNoIndex) {
          return null;
        }

        let fromIndex = collectFull ? -1 : this._fromIndex;
        this._fromIndex = kNoIndex;

        let historychange = lazy.SessionHistory.collectFromParent(
          browsingContext.currentURI?.spec,
          true, 
          browsingContext.sessionHistory,
          fromIndex
        );

        if (writeToCache) {
          let win =
            browsingContext.embedderElement?.documentGlobal ||
            browsingContext.currentWindowGlobal?.browsingContext?.window;

          SessionStoreInternal.onTabStateUpdate(permanentKey, win, {
            data: { historychange },
          });
        }


        return historychange;
      }

      collectFrom(index) {
        if (this._fromIndex <= index) {
          return;
        }

        let bc = BrowsingContext.getCurrentTopByBrowserId(this._browserId);
        if (bc?.embedderElement?.frameLoader) {
          this._fromIndex = index;

          bc.embedderElement.frameLoader.requestSHistoryUpdate();
        }
      }

      OnHistoryNewEntry(newURI, oldIndex) {
        this.collectFrom(oldIndex == -1 ? oldIndex : oldIndex - 1);
      }
      OnHistoryGotoIndex() {
        this.collectFrom(kLastIndex);
      }
      OnHistoryPurge() {
        this.collectFrom(-1);
      }
      OnHistoryReload() {
        this.collectFrom(-1);
        return true;
      }
      OnHistoryReplaceEntry() {
        this.collectFrom(-1);
      }
    }

    let sessionHistory = browsingContext.sessionHistory;
    if (!sessionHistory) {
      return null;
    }

    const listener = new SHistoryListener();
    sessionHistory.addSHistoryListener(listener);
    this._browserSHistoryListener.set(permanentKey, listener);

    let isAboutBlank = browsingContext.currentURI?.spec === "about:blank";

    if (collectImmediately && (!isAboutBlank || sessionHistory.count !== 0)) {
      listener.collect(permanentKey, browsingContext, { writeToCache: true });
    }

    return listener;
  },

  onTabStateUpdate(permanentKey, win, update) {
    if (this._crashedBrowsers.has(permanentKey)) {
      return;
    }

    lazy.TabState.update(permanentKey, update);
    this.saveStateDelayed(win);

    let closedTab = this._closingTabMap.get(permanentKey);
    if (closedTab) {
      lazy.TabState.copyFromCache(permanentKey, closedTab.tabData.state);
    }
  },

  onFinalTabStateUpdateComplete(browser) {
    let permanentKey = browser.permanentKey;
    if (
      this._closingTabMap.has(permanentKey) &&
      !this._crashedBrowsers.has(permanentKey)
    ) {
      let { winData, closedTabs, tabData } =
        this._closingTabMap.get(permanentKey);

      this._closingTabMap.delete(permanentKey);

      delete tabData.permanentKey;

      let shouldSave = this._shouldSaveTabState(tabData.state);
      let index = closedTabs.indexOf(tabData);

      if (shouldSave && index == -1) {
        this.saveClosedTabData(winData, closedTabs, tabData);
      } else if (!shouldSave && index > -1) {
        this.removeClosedTabData(winData, closedTabs, index);
      }

      this._cleanupOrphanedClosedGroups(winData);
    }

    lazy.TabStateFlusher.resolveAll(browser);

    this._browserSHistoryListener.get(permanentKey)?.unregister();
    this._restoreListeners.get(permanentKey)?.unregister();

    Services.obs.notifyObservers(browser, NOTIFY_BROWSER_SHUTDOWN_FLUSH);
  },

  updateSessionStoreFromTablistener(
    browser,
    browsingContext,
    permanentKey,
    update,
    forStorage = false
  ) {
    permanentKey = browser?.permanentKey ?? permanentKey;
    if (!permanentKey) {
      return;
    }

    if (!this.isCurrentEpoch(permanentKey, update.epoch)) {
      return;
    }

    if (browsingContext.isReplaced) {
      return;
    }

    let listener = this.getOrCreateSHistoryListener(
      permanentKey,
      browsingContext
    );

    if (listener) {
      let historychange =
        (forStorage &&
          lazy.SessionHistory.collectNonWebControlledLoadingSession(
            browsingContext
          )) ||
        listener.collect(permanentKey, browsingContext, {
          collectFull: !!update.sHistoryNeeded,
          writeToCache: false,
        });

      if (historychange) {
        update.data.historychange = historychange;
      }
    }

    let win =
      browser?.documentGlobal ??
      browsingContext.currentWindowGlobal?.browsingContext?.window;

    this.onTabStateUpdate(permanentKey, win, update);
  },


  handleEvent: function ssi_handleEvent(aEvent) {
    let win = aEvent.currentTarget.documentGlobal;
    let target = aEvent.originalTarget;
    switch (aEvent.type) {
      case "TabOpen":
        this.onTabAdd(win);
        if (aEvent.detail.adoptedTab) {
          this.moveCustomTabValue(aEvent.detail.adoptedTab, target);
        }
        break;
      case "TabBrowserInserted":
        this.onTabBrowserInserted(win, target);
        break;
      case "TabClose":
        if (aEvent.detail.adoptedBy) {
          this.moveCustomTabValue(target, aEvent.detail.adoptedBy);
          this.onMoveToNewWindow(
            target.linkedBrowser,
            aEvent.detail.adoptedBy.linkedBrowser
          );
        } else if (!aEvent.detail.skipSessionStore) {
          this.onTabClose(win, target);
        }
        this.onTabRemove(win, target);
        this._notifyOfClosedObjectsChange();
        break;
      case "TabSelect":
        this.onTabSelect(win);
        break;
      case "TabShow":
        this.onTabShow(win, target);
        break;
      case "TabHide":
        this.onTabHide(win, target);
        break;
      case "TabPinned":
      case "TabUnpinned":
      case "SwapDocShells":
        this.saveStateDelayed(win);
        break;
      case "TabGroupCreate":
      case "TabGroupRemoved":
      case "TabGrouped":
      case "TabUngrouped":
      case "TabGroupCollapse":
      case "TabGroupExpand":
      case "SplitViewRemoved":
      case "SplitViewCreated":
        this.saveStateDelayed(win);
        break;
      case "TabGroupRemoveRequested":
        if (!aEvent.detail?.skipSessionStore) {
          this.onTabGroupRemoveRequested(win, target);
          this._notifyOfClosedObjectsChange();
        }
        break;
      case "TabSplitViewActivate":
        for (const tab of aEvent.detail.tabs) {
          this.maybeRestoreTabContent(tab);
        }
        this.saveStateDelayed(win);
        break;
      case "oop-browser-crashed":
      case "oop-browser-buildid-mismatch":
        if (aEvent.isTopFrame) {
          this.onBrowserCrashed(target);
        }
        break;
      case "XULFrameLoaderCreated":
        if (
          target.namespaceURI == XUL_NS &&
          target.localName == "browser" &&
          target.frameLoader &&
          target.permanentKey
        ) {
          this.resetEpoch(target.permanentKey, target.frameLoader);
        }
        break;
      default:
        throw new Error(`unhandled event ${aEvent.type}?`);
    }
    this._clearRestoringWindows();
  },

  _generateWindowID: function ssi_generateWindowID() {
    return "window" + this._nextWindowID++;
  },

  onLoad(aWindow) {
    if (aWindow && aWindow.__SSi && this._windows[aWindow.__SSi]) {
      return;
    }

    if (lazy.RunState.isQuitting) {
      return;
    }

    aWindow.__SSi = this._generateWindowID();

    this._windows[aWindow.__SSi] = {
      tabs: [],
      groups: [],
      closedGroups: [],
      selected: 0,
      _closedTabs: [],
      _lastClosedTabGroupCount: -1,
      lastClosedTabGroupId: null,
      busy: false,
      chromeFlags: aWindow.docShell.treeOwner
        .QueryInterface(Ci.nsIInterfaceRequestor)
        .getInterface(Ci.nsIAppWindow).chromeFlags,
    };

    if (PrivateBrowsingUtils.isWindowPrivate(aWindow)) {
      this._windows[aWindow.__SSi].isPrivate = true;
    }
    if (!this._isWindowLoaded(aWindow)) {
      this._windows[aWindow.__SSi]._restoring = true;
    }
    if (!aWindow.toolbar.visible) {
      this._windows[aWindow.__SSi].isPopup = true;
    }

    if (aWindow.document.documentElement.hasAttribute("taskbartab")) {
      this._windows[aWindow.__SSi].isTaskbarTab = true;
    }

    let tabbrowser = aWindow.gBrowser;

    for (let i = 0; i < tabbrowser.tabs.length; i++) {
      this.onTabBrowserInserted(aWindow, tabbrowser.tabs[i]);
    }
    TAB_EVENTS.forEach(function (aEvent) {
      tabbrowser.tabContainer.addEventListener(aEvent, this, true);
    }, this);

    aWindow.gBrowser.addEventListener("XULFrameLoaderCreated", this);
  },

  initializeWindow(aWindow, aInitialState = null) {
    let isPrivateWindow = PrivateBrowsingUtils.isWindowPrivate(aWindow);
    let isTaskbarTab = this._windows[aWindow.__SSi].isTaskbarTab;
    let isRegularWindow =
      !isPrivateWindow && !isTaskbarTab && aWindow.toolbar.visible;

    if (lazy.RunState.isStopped) {
      lazy.RunState.setRunning();

      if (aInitialState) {
        lazy.SessionSaver.updateLastSaveTime();

        if (isPrivateWindow || isTaskbarTab) {
          this._log.debug(
            "initializeWindow, the window is private or a web app. Saving SessionStartup.state for possibly restoring later"
          );
          this._deferredInitialState = lazy.SessionStartup.state;

          Services.obs.notifyObservers(null, NOTIFY_WINDOWS_RESTORED);
          Services.obs.notifyObservers(
            null,
            "sessionstore-one-or-no-tab-restored"
          );
          this._deferredAllWindowsRestored.resolve();
        } else {
          this._restoreCount = aInitialState.windows
            ? aInitialState.windows.length
            : 0;

          this._globalState.setFromState(aInitialState);

          lazy.SessionCookies.restore(aInitialState.cookies || []);

          let overwrite = this._isCmdLineEmpty(aWindow, aInitialState);
          let options = { firstWindow: true, overwriteTabs: overwrite };
          this.restoreWindows(aWindow, aInitialState, options);
        }
      } else {
        Services.obs.notifyObservers(null, NOTIFY_WINDOWS_RESTORED);
        Services.obs.notifyObservers(
          null,
          "sessionstore-one-or-no-tab-restored"
        );
        this._deferredAllWindowsRestored.resolve();
      }
    } else if (!this._isWindowLoaded(aWindow)) {
      return;
    } else if (this._deferredInitialState && isRegularWindow) {
      if (lazy.SessionStartup.willRestore()) {
        this._globalState.setFromState(this._deferredInitialState);
        this._restoreCount = this._deferredInitialState.windows
          ? this._deferredInitialState.windows.length
          : 0;
        this.restoreWindows(aWindow, this._deferredInitialState, {
          firstWindow: true,
        });
      }
      this._deferredInitialState = null;
    } else if (
      this._restoreLastWindow &&
      aWindow.toolbar.visible &&
      this._closedWindows.length &&
      !isPrivateWindow
    ) {
      let closedWindowState = null;
      let closedWindowIndex;
      for (let i = 0; i < this._closedWindows.length; i++) {
        if (!this._closedWindows[i].isPopup) {
          closedWindowState = this._closedWindows[i];
          closedWindowIndex = i;
          break;
        }
      }

      if (closedWindowState) {
        let newWindowState;
        if (
          AppConstants.platform == "macosx" ||
          !lazy.SessionStartup.willRestore()
        ) {
          let [appTabsState, normalTabsState] =
            this._prepDataForDeferredRestore({
              windows: [closedWindowState],
            });

          if (appTabsState.windows.length) {
            newWindowState = appTabsState.windows[0];
            delete newWindowState.__lastSessionWindowID;
          }

          if (!normalTabsState.windows.length) {
            this._removeClosedWindow(closedWindowIndex);
          } else {
            delete normalTabsState.windows[0].__lastSessionWindowID;
            this._closedWindows[closedWindowIndex] = normalTabsState.windows[0];
          }
        } else {
          this._removeClosedWindow(closedWindowIndex);
          newWindowState = closedWindowState;
          delete newWindowState.hidden;
        }

        if (newWindowState) {
          this._restoreCount = 1;
          let state = { windows: [newWindowState] };
          let options = { overwriteTabs: this._isCmdLineEmpty(aWindow, state) };
          this.restoreWindow(aWindow, newWindowState, options);
        }
      }
      this._prefBranch.setBoolPref("sessionstore.resume_session_once", false);
    }
    else if (
      Services.prefs.getBoolPref("browser.taskbarTabs.enabled", false) &&
      this._shouldRestoreLastSession &&
      isRegularWindow
    ) {
      let lastSessionState = LastSession.getState();
      this._globalState.setFromState(lastSessionState);
      lazy.SessionCookies.restore(lastSessionState.cookies || []);
      this.restoreWindows(aWindow, lastSessionState, {
        firstWindow: true,
      });
      this._shouldRestoreLastSession = false;
    }

    if (this._restoreLastWindow && aWindow.toolbar.visible) {
      this._restoreLastWindow = false;
    }
  },

  onBeforeBrowserWindowShown(aWindow) {
    if (aWindow.browsingContext.isDocumentPiP) {
      return;
    }

    this.onLoad(aWindow);

    let deferred = WINDOW_SHOWING_PROMISES.get(aWindow);
    if (deferred) {
      deferred.resolve(aWindow);
      WINDOW_SHOWING_PROMISES.delete(aWindow);
    }

    if (this._sessionInitialized) {
      this._log.debug(
        "onBeforeBrowserWindowShown, session already initialized, initializing window"
      );
      this.initializeWindow(aWindow);
      return;
    }

    if (!this._promiseReadyForInitialization) {
      let promise = new Promise(resolve => {
        Services.obs.addObserver(function obs(subject, topic) {
          if (aWindow == subject) {
            Services.obs.removeObserver(obs, topic);
            resolve();
          }
        }, "browser-delayed-startup-finished");
      });

      this._promiseReadyForInitialization = Promise.all([
        promise,
        lazy.SessionStartup.onceInitialized,
      ]);
    }

    this._promiseReadyForInitialization
      .then(() => {
        if (aWindow.closed) {
          this._log.debug(
            "When _promiseReadyForInitialization resolved, the window was closed"
          );
          return;
        }

        if (this._sessionInitialized) {
          this.initializeWindow(aWindow);
        } else {
          let initialState = this.initSession();
          this._sessionInitialized = true;

          if (initialState) {
            Services.obs.notifyObservers(null, NOTIFY_RESTORING_ON_STARTUP);
          }
          this.initializeWindow(aWindow, initialState);

          this._deferredInitialized.resolve();
        }
      })
      .catch(ex => {
        this._log.error(
          "Exception when handling _promiseReadyForInitialization resolution:",
          ex
        );
      });
  },

  onClose: function ssi_onClose(aWindow) {
    let completionPromise = Promise.resolve();
    let isFullyLoaded = this._isWindowLoaded(aWindow);
    if (!isFullyLoaded) {
      if (!aWindow.__SSi) {
        aWindow.__SSi = this._generateWindowID();
      }

      let restoreID = WINDOW_RESTORE_IDS.get(aWindow);
      this._windows[aWindow.__SSi] =
        this._statesToRestore[restoreID].windows[0];
      delete this._statesToRestore[restoreID];
      WINDOW_RESTORE_IDS.delete(aWindow);
    }

    if (!aWindow.__SSi || !this._windows[aWindow.__SSi]) {
      return completionPromise;
    }

    let event = aWindow.document.createEvent("Events");
    event.initEvent("SSWindowClosing", true, false);
    aWindow.dispatchEvent(event);

    if (this.windowToFocus && this.windowToFocus == aWindow) {
      delete this.windowToFocus;
    }

    var tabbrowser = aWindow.gBrowser;

    let browsers = Array.from(tabbrowser.browsers);

    TAB_EVENTS.forEach(function (aEvent) {
      tabbrowser.tabContainer.removeEventListener(aEvent, this, true);
    }, this);

    aWindow.gBrowser.removeEventListener("XULFrameLoaderCreated", this);

    let winData = this._windows[aWindow.__SSi];

    if (lazy.RunState.isRunning) {
      let tabMap = this._collectWindowData(aWindow);

      for (let [tab, tabData] of tabMap) {
        let permanentKey = tab.linkedBrowser.permanentKey;
        this._tabClosingByWindowMap.set(permanentKey, tabData);
      }

      if (isFullyLoaded && !winData.title) {
        winData.title =
          tabbrowser.selectedBrowser.contentTitle ||
          tabbrowser.selectedTab.label;
      }

      if (AppConstants.platform != "macosx") {
        winData._shouldRestore = true;
      }

      winData.closedAt = Date.now();

      delete winData.busy;

      let isLastWindow = this.isLastRestorableWindow();

      let isLastRegularWindow =
        Object.values(this._windows).filter(
          wData => !wData.isPrivate && !wData.isTaskbarTab
        ).length == 1;
      this._log.debug(
        `onClose, closing window isLastRegularWindow? ${isLastRegularWindow}`
      );

      let taskbarTabsRemains = Object.values(this._windows).some(
        wData => wData.isTaskbarTab
      );

      if (
        Services.prefs.getBoolPref("browser.taskbarTabs.enabled", false) &&
        isLastRegularWindow &&
        !winData.isTaskbarTab &&
        !winData.isPrivate &&
        taskbarTabsRemains
      ) {
        if (this.willAutoRestore) {
          this._shouldRestoreLastSession = true;
        } else {
          Services.obs.notifyObservers(null, NOTIFY_LAST_SESSION_RE_ENABLED);
        }

        let savedState = this.getCurrentState(true);
        lazy.PrivacyFilter.filterPrivateWindowsAndTabs(savedState);
        LastSession.setState(savedState);
        this._restoreWithoutRestart = true;
      }

      delete this._windows[aWindow.__SSi];

      this._saveableClosedWindowData.add(winData);

      if (!winData.isPrivate && !winData.isTaskbarTab) {
        this.maybeSaveClosedWindow(winData, isLastWindow);
      }

      completionPromise = lazy.TabStateFlusher.flushWindow(aWindow).then(() => {

        WINDOW_FLUSHING_PROMISES.delete(aWindow);

        for (let browser of browsers) {
          if (this._tabClosingByWindowMap.has(browser.permanentKey)) {
            let tabData = this._tabClosingByWindowMap.get(browser.permanentKey);
            lazy.TabState.copyFromCache(browser.permanentKey, tabData);
            this._tabClosingByWindowMap.delete(browser.permanentKey);
          }
        }

        if (!winData.isPrivate && !winData.isTaskbarTab) {
          this.maybeSaveClosedWindow(winData, isLastWindow);

          if (!isLastWindow && winData.closedId > -1) {
            this._addClosedAction(
              this._LAST_ACTION_CLOSED_WINDOW,
              winData.closedId
            );
          }
        }

        this.cleanUpWindow(aWindow, winData, browsers);

        this.saveStateDelayed();
      });

      WINDOW_FLUSHING_PROMISES.set(aWindow, completionPromise);
    } else {
      this.cleanUpWindow(aWindow, winData, browsers);
    }

    for (let i = 0; i < tabbrowser.tabs.length; i++) {
      this.onTabRemove(aWindow, tabbrowser.tabs[i], true);
    }

    return completionPromise;
  },

  cleanUpWindow(aWindow, winData, browsers) {
    for (let browser of browsers) {
      lazy.TabStateFlusher.resolveAll(browser);
    }

    DyingWindowCache.set(aWindow, winData);

    this._saveableClosedWindowData.delete(winData);
    delete aWindow.__SSi;
  },

  maybeSaveClosedWindow(winData, isLastWindow) {
    if (
      lazy.RunState.isRunning &&
      this._saveableClosedWindowData.has(winData)
    ) {
      let hasSaveableTabs = winData.tabs.some(this._shouldSaveTabState);

      let winIndex = this._closedWindows.indexOf(winData);
      let alreadyStored = winIndex != -1;
      let shouldStore = hasSaveableTabs || isLastWindow;

      if (shouldStore && !alreadyStored) {
        let index = this._closedWindows.findIndex(win => {
          return win.closedAt < winData.closedAt;
        });

        if (index == -1) {
          index = this._closedWindows.length;
        }

        winData.closedId = this._nextClosedId++;

        this._closedWindows.splice(index, 0, winData);
        this._capClosedWindows();
        this._saveOpenTabGroupsOnClose(winData);
        this._closedObjectsChanged = true;
        this._log.debug(
          `Saved closed window:${winData.closedId} with ${winData.tabs.length} open tabs, ${winData._closedTabs.length} closed tabs`
        );

        if (
          AppConstants.platform == "macosx" &&
          this._closedWindows.length == 1
        ) {
          let window = Services.appShell.hiddenDOMWindow;
          let historyMenu = window.document.getElementById("history-menu");
          let evt = new window.CustomEvent("popupshowing", { bubbles: true });
          historyMenu.menupopup.dispatchEvent(evt);
        }
      } else if (!shouldStore) {
        if (
          winData._closedTabs.length &&
          this._closedTabsFromAllWindowsEnabled
        ) {
          this._closedObjectsChanged = true;
        }
        if (alreadyStored) {
          this._removeClosedWindow(winIndex);
          return;
        }
        this._log.warn(
          `Discarding window:${winData.closedId} with 0 saveable tabs and ${winData._closedTabs.length} closed tabs`
        );
      }
    }
  },

  _saveOpenTabGroupsOnClose(closedWinData) {
    let newlySavedTabGroups = new Map();
    closedWinData.groups = closedWinData.groups.map(tabGroupState =>
      lazy.TabGroupState.savedInClosedWindow(
        tabGroupState,
        closedWinData.closedId
      )
    );
    for (let tabGroupState of closedWinData.groups) {
      if (!tabGroupState.saveOnWindowClose) {
        continue;
      }
      newlySavedTabGroups.set(tabGroupState.id, tabGroupState);
    }
    for (let tIndex = 0; tIndex < closedWinData.tabs.length; tIndex++) {
      let tabState = closedWinData.tabs[tIndex];
      if (!tabState.groupId) {
        continue;
      }
      if (!newlySavedTabGroups.has(tabState.groupId)) {
        continue;
      }

      if (this._shouldSaveTabState(tabState)) {
        let tabData = this._formatTabStateForSavedGroup(tabState);
        if (!tabData) {
          continue;
        }
        newlySavedTabGroups.get(tabState.groupId).tabs.push(tabData);
      }
    }

    for (let tabGroupToSave of newlySavedTabGroups.values()) {
      this._recordSavedTabGroupState(tabGroupToSave);
    }
  },

  _formatTabStateForSavedGroup(tabState) {
    let activeIndex = tabState.index;
    activeIndex = Math.min(activeIndex, tabState.entries.length - 1);
    activeIndex = Math.max(activeIndex, 0);
    if (!(activeIndex in tabState.entries)) {
      return {};
    }
    let title =
      tabState.entries[activeIndex].title || tabState.entries[activeIndex].url;
    return {
      state: tabState,
      title,
      image: tabState.image,
      pos: tabState.pos,
      closedAt: Date.now(),
      closedId: this._nextClosedId++,
    };
  },

  onQuitApplicationGranted: function ssi_onQuitApplicationGranted(
    syncShutdown = false
  ) {
    let index = 0;
    for (let window of this._orderedBrowserWindows) {
      this._collectWindowData(window);
      this._windows[window.__SSi].zIndex = ++index;
    }
    this._log.debug(
      `onQuitApplicationGranted, shutdown of ${index} windows will be sync? ${syncShutdown}`
    );
    this._log.debug(
      `Last session save attempt: ${Date.now() - lazy.SessionSaver.lastSaveTime}ms ago`
    );


    let progress = { total: -1, current: -1 };

    lazy.RunState.setQuitting();

    if (!syncShutdown) {


      let promises = [this.flushAllWindowsAsync(progress)];

      const observeTopic = topic => {
        let deferred = Promise.withResolvers();
        const observer = subject => {
          subject.QueryInterface(Ci.nsIPropertyBag2);
          switch (topic) {
            case "ipc:content-shutdown":
              if (subject.get("abnormal")) {
                this._log.debug(
                  "Observed abnormal ipc:content-shutdown during shutdown"
                );
                deferred.resolve();
              }
              break;
            case "oop-frameloader-crashed":
              this._log.debug(`Observed topic: ${topic} during shutdown`);
              deferred.resolve();
              break;
          }
        };
        const cleanup = () => {
          try {
            Services.obs.removeObserver(observer, topic);
          } catch (ex) {
            this._log.error("Exception whilst flushing all windows", ex);
          }
        };
        Services.obs.addObserver(observer, topic);
        deferred.promise.then(cleanup, cleanup);
        return deferred;
      };

      let waitTimeMaxMs = Math.max(
        0,
        lazy.AsyncShutdown.DELAY_CRASH_MS - 10000
      );
      let defers = [
        this.looseTimer(waitTimeMaxMs),

        observeTopic("oop-frameloader-crashed"),
        observeTopic("ipc:content-shutdown"),
      ];
      promises.push(...defers.map(deferred => deferred.promise));

      let isDone = false;
      Promise.race(promises)
        .then(() => {
          defers.forEach(deferred => deferred.reject());
        })
        .finally(() => {
          isDone = true;
        });
      Services.tm.spinEventLoopUntil(
        "Wait until SessionStoreInternal.flushAllWindowsAsync finishes.",
        () => isDone
      );
    } else {
    }
  },

  async flushAllWindowsAsync(progress = {}) {
    let windowPromises = new Map(WINDOW_FLUSHING_PROMISES);
    WINDOW_FLUSHING_PROMISES.clear();

    for (let window of this._browserWindows) {
      windowPromises.set(window, lazy.TabStateFlusher.flushWindow(window));

      let baseWin = window.docShell.treeOwner.QueryInterface(Ci.nsIBaseWindow);
      baseWin.visibility = false;
    }

    progress.total = windowPromises.size;
    progress.current = 0;

    for (let [win, promise] of windowPromises) {
      await promise;

      if (win.__SSi && this._windows[win.__SSi]) {
        this._collectWindowData(win);
      }

      progress.current++;
    }

    var activeWindow = this._getTopWindow();
    if (activeWindow) {
      this.activeWindowSSiCache = activeWindow.__SSi || "";
    }
    DirtyWindows.clear();
  },

  onLastWindowCloseGranted: function ssi_onLastWindowCloseGranted() {
    this._restoreLastWindow = true;
  },

  onQuitApplication: function ssi_onQuitApplication(aData) {
    if (aData == "restart" || aData == "os-restart") {
      if (!PrivateBrowsingUtils.permanentPrivateBrowsing) {
        if (
          aData == "os-restart" &&
          !this._prefBranch.getBoolPref("sessionstore.resume_session_once")
        ) {
          this._prefBranch.setBoolPref(
            "sessionstore.resuming_after_os_restart",
            true
          );
        }
        this._prefBranch.setBoolPref("sessionstore.resume_session_once", true);
      }

      Services.obs.removeObserver(this, "browser:purge-session-history");
    }

    if (aData != "restart") {
      LastSession.clear(true);
    }

    this._uninit();
  },

  purgeDataForPrivateWindow(win) {
    if (lazy.RunState.isQuitting) {
      return;
    }

    let windowData = this._windows[win.__SSi];
    if (!windowData) {
      return;
    }

    if (windowData._closedTabs.length) {
      while (windowData._closedTabs.length) {
        this.removeClosedTabData(windowData, windowData._closedTabs, 0);
      }
      windowData._closedTabs = [];
      windowData._lastClosedTabGroupCount = -1;
      windowData.lastClosedTabGroupId = null;
      this._closedObjectsChanged = true;
    }

    if (windowData.closedGroups.length) {
      for (let closedGroup of windowData.closedGroups) {
        while (closedGroup.tabs.length) {
          this.removeClosedTabData(windowData, closedGroup.tabs, 0);
        }
      }
      windowData.closedGroups = [];
      this._closedObjectsChanged = true;
    }
  },

  onPurgeSessionHistory: function ssi_onPurgeSessionHistory() {
    lazy.SessionFile.wipe();
    if (lazy.RunState.isQuitting) {
      return;
    }
    LastSession.clear();

    let openWindows = {};
    for (let window of this._browserWindows) {
      openWindows[window.__SSi] = true;
    }

    for (let ix in this._windows) {
      if (ix in openWindows) {
        if (this._windows[ix]._closedTabs.length) {
          this._windows[ix]._closedTabs = [];
          this._closedObjectsChanged = true;
        }
        if (this._windows[ix].closedGroups.length) {
          this._windows[ix].closedGroups = [];
          this._closedObjectsChanged = true;
        }
      } else {
        delete this._windows[ix];
      }
    }
    if (this._closedWindows.length) {
      this._closedWindows = [];
      this._closedObjectsChanged = true;
    }
    var win = this._getTopWindow();
    if (win) {
      win.setTimeout(() => lazy.SessionSaver.run(), 0);
    } else if (lazy.RunState.isRunning) {
      lazy.SessionSaver.run();
    }

    this._clearRestoringWindows();
    this._saveableClosedWindowData = new WeakSet();
    this._lastClosedActions = [];
  },

  onPurgeDomainData: function ssi_onPurgeDomainData(aDomain) {
    function containsDomain(aEntry) {
      let host;
      try {
        host = Services.io.newURI(aEntry.url).host;
      } catch (e) {
      }
      if (host && Services.eTLD.hasRootDomain(host, aDomain)) {
        return true;
      }
      return aEntry.children && aEntry.children.some(containsDomain, this);
    }
    for (let ix in this._windows) {
      let closedTabsLists = [
        this._windows[ix]._closedTabs,
        ...this._windows[ix].closedGroups.map(g => g.tabs),
      ];

      for (let closedTabs of closedTabsLists) {
        for (let i = closedTabs.length - 1; i >= 0; i--) {
          if (closedTabs[i].state.entries.some(containsDomain, this)) {
            closedTabs.splice(i, 1);
            this._closedObjectsChanged = true;
          }
        }
      }
    }
    for (let ix = this._closedWindows.length - 1; ix >= 0; ix--) {
      let closedTabsLists = [
        this._closedWindows[ix]._closedTabs,
        ...this._closedWindows[ix].closedGroups.map(g => g.tabs),
      ];
      let openTabs = this._closedWindows[ix].tabs;
      let openTabCount = openTabs.length;

      for (let closedTabs of closedTabsLists) {
        for (let i = closedTabs.length - 1; i >= 0; i--) {
          if (closedTabs[i].state.entries.some(containsDomain, this)) {
            closedTabs.splice(i, 1);
          }
        }
      }
      for (let j = openTabs.length - 1; j >= 0; j--) {
        if (openTabs[j].entries.some(containsDomain, this)) {
          openTabs.splice(j, 1);
          if (this._closedWindows[ix].selected > j) {
            this._closedWindows[ix].selected--;
          }
        }
      }
      if (!openTabs.length) {
        this._closedWindows.splice(ix, 1);
      } else if (openTabs.length != openTabCount) {
        let selectedTab = openTabs[this._closedWindows[ix].selected - 1];
        let activeIndex = (selectedTab.index || selectedTab.entries.length) - 1;
        if (activeIndex >= selectedTab.entries.length) {
          activeIndex = selectedTab.entries.length - 1;
        }
        this._closedWindows[ix].title = selectedTab.entries[activeIndex].title;
      }
    }

    if (lazy.RunState.isRunning) {
      lazy.SessionSaver.run();
    }

    this._clearRestoringWindows();
  },

  onPrefChange: function ssi_onPrefChange(aData) {
    switch (aData) {
      case "sessionstore.max_tabs_undo":
        this._max_tabs_undo = this._prefBranch.getIntPref(
          "sessionstore.max_tabs_undo"
        );
        for (let ix in this._windows) {
          if (this._windows[ix]._closedTabs.length > this._max_tabs_undo) {
            this._windows[ix]._closedTabs.splice(
              this._max_tabs_undo,
              this._windows[ix]._closedTabs.length
            );
            this._closedObjectsChanged = true;
          }
        }
        break;
      case "sessionstore.max_windows_undo":
        this._max_windows_undo = this._prefBranch.getIntPref(
          "sessionstore.max_windows_undo"
        );
        this._capClosedWindows();
        break;
      case "sessionstore.restore_on_demand":
        this._restore_on_demand = this._prefBranch.getBoolPref(
          "sessionstore.restore_on_demand"
        );
        break;
      case "sessionstore.closedTabsFromAllWindows":
        this._closedTabsFromAllWindowsEnabled = this._prefBranch.getBoolPref(
          "sessionstore.closedTabsFromAllWindows"
        );
        this._closedObjectsChanged = true;
        break;
      case "sessionstore.closedTabsFromClosedWindows":
        this._closedTabsFromClosedWindowsEnabled = this._prefBranch.getBoolPref(
          "sessionstore.closedTabsFromClosedWindows"
        );
        this._closedObjectsChanged = true;
        break;
    }
  },

  onTabAdd: function ssi_onTabAdd(aWindow) {
    this.saveStateDelayed(aWindow);
  },

  onTabBrowserInserted: function ssi_onTabBrowserInserted(aWindow, aTab) {
    let browser = aTab.linkedBrowser;
    browser.addEventListener("SwapDocShells", this);
    browser.addEventListener("oop-browser-crashed", this);
    browser.addEventListener("oop-browser-buildid-mismatch", this);

    if (
      TAB_LAZY_STATES.has(aTab) &&
      !TAB_STATE_FOR_BROWSER.has(browser) &&
      lazy.TabStateCache.get(browser.permanentKey)
    ) {
      let tabState = lazy.TabState.clone(aTab, TAB_CUSTOM_VALUES.get(aTab));
      this.restoreTab(aTab, tabState);
    }

    TAB_LAZY_STATES.delete(aTab);
  },

  onTabRemove: function ssi_onTabRemove(aWindow, aTab, aNoNotification) {
    this.cleanUpRemovedBrowser(aTab);

    if (!aNoNotification) {
      this.saveStateDelayed(aWindow);
    }
  },

  onTabClose: function ssi_onTabClose(aWindow, aTab) {
    if (this._max_tabs_undo == 0) {
      return;
    }

    let tabState = lazy.TabState.collect(aTab, TAB_CUSTOM_VALUES.get(aTab));

    this.maybeSaveClosedTab(aWindow, aTab, tabState);
  },

  onTabGroupRemoveRequested: function ssi_onTabGroupRemoveRequested(
    win,
    tabGroup
  ) {
    if (this._max_tabs_undo == 0) {
      return;
    }

    if (this.getSavedTabGroup(tabGroup.id)) {
      return;
    }

    let closedGroups = this._windows[win.__SSi].closedGroups;
    let tabGroupState = lazy.TabGroupState.closed(tabGroup, win.__SSi);
    tabGroupState.tabs = this._collectClosedTabsForTabGroup(tabGroup.tabs, win);
    tabGroupState.splitViews = this._collectSplitViewDataForTabGroup(
      tabGroup.tabs
    );

    this._windows[win.__SSi]._lastClosedTabGroupCount =
      tabGroupState.tabs.length;
    closedGroups.unshift(tabGroupState);
    this._closedObjectsChanged = true;
  },

  _collectClosedTabsForTabGroup(tabs, win, { updateTabGroupId } = {}) {
    let closedTabs = [];
    tabs.forEach(tab => {
      let tabState = lazy.TabState.collect(tab, TAB_CUSTOM_VALUES.get(tab));
      if (updateTabGroupId) {
        tabState.groupId = updateTabGroupId;
      }
      this.maybeSaveClosedTab(win, tab, tabState, {
        closedTabsArray: closedTabs,
        closedInTabGroup: true,
      });
    });
    return closedTabs;
  },

  _collectSplitViewDataForTabGroup(tabs) {
    let splitViewData = new Map();
    tabs.forEach(tab => {
      if (tab.splitview) {
        if (!splitViewData.get(tab.splitview.splitViewId)) {
          splitViewData.set(tab.splitview.splitViewId, tab.splitview.state);
        }
      }
    });
    return Array.from(splitViewData.values());
  },

  onMoveToNewWindow(aFromBrowser, aToBrowser) {
    lazy.TabStateFlusher.flush(aFromBrowser).then(() => {
      let tabState = lazy.TabStateCache.get(aFromBrowser.permanentKey);
      if (!tabState) {
        throw new Error(
          "Unexpected undefined tabState for onMoveToNewWindow aFromBrowser"
        );
      }
      lazy.TabStateCache.update(aToBrowser.permanentKey, tabState);
    });
  },

  maybeSaveClosedTab(
    aWindow,
    aTab,
    tabState,
    { closedTabsArray, closedInTabGroup = false } = {}
  ) {
    let isPrivateWindow = PrivateBrowsingUtils.isWindowPrivate(aWindow);
    if (!isPrivateWindow && tabState.isPrivate) {
      return;
    }
    let permanentKey = aTab.linkedBrowser.permanentKey;

    let tabData = {
      permanentKey,
      state: tabState,
      title: aTab.label,
      image: aWindow.gBrowser.getIcon(aTab),
      pos: aTab._tPos,
      closedAt: Date.now(),
      closedInGroup: aTab._closedInMultiselection,
      closedInTabGroupId: closedInTabGroup ? tabState.groupId : null,
      sourceWindowId: aWindow.__SSi,
    };

    let winData = this._windows[aWindow.__SSi];
    let closedTabs = closedTabsArray || winData._closedTabs;

    if (this._shouldSaveTabState(tabState)) {
      this.saveClosedTabData(winData, closedTabs, tabData);
    }

    this._closingTabMap.set(permanentKey, {
      winData,
      closedTabs,
      tabData,
    });
  },

  resetBrowserToLazyState(aTab) {
    let browser = aTab.linkedBrowser;
    if (!browser.isConnected) {
      return;
    }

    this.cleanUpRemovedBrowser(aTab);

    aTab.setAttribute("pending", "true");

    this._crashedBrowsers.delete(browser.permanentKey);
    aTab.removeAttribute("crashed");

    let { userTypedValue = null, userTypedClear = 0 } = browser;
    let hasStartedLoad = browser.didStartLoadSinceLastUserTyping();

    let cacheState = lazy.TabStateCache.get(browser.permanentKey);

    let shouldUpdateCacheState =
      userTypedValue &&
      (!cacheState || (hasStartedLoad && !cacheState.userTypedValue));

    if (shouldUpdateCacheState) {
      lazy.TabStateCache.update(browser.permanentKey, {
        userTypedValue,
        userTypedClear: 1,
      });
    }

    TAB_LAZY_STATES.set(aTab, {
      url: browser.currentURI.spec,
      title: aTab.label,
      userTypedValue,
      userTypedClear,
    });
  },

  isBrowserInCrashedSet(aBrowser) {
    if (gDebuggingEnabled) {
      return this._crashedBrowsers.has(aBrowser.permanentKey);
    }
    throw new Error(
      "SessionStore.isBrowserInCrashedSet() should only be called in debug mode!"
    );
  },

  cleanUpRemovedBrowser(aTab) {
    let browser = aTab.linkedBrowser;

    browser.removeEventListener("SwapDocShells", this);
    browser.removeEventListener("oop-browser-crashed", this);
    browser.removeEventListener("oop-browser-buildid-mismatch", this);

    let previousState = TAB_STATE_FOR_BROWSER.get(browser);
    if (previousState) {
      this._resetTabRestoringState(aTab);
      if (previousState == TAB_STATE_RESTORING) {
        this.restoreNextTab();
      }
    }
  },

  saveClosedTabData(winData, closedTabs, tabData, saveAction = true) {
    let index = tabData.closedInTabGroupId
      ? closedTabs.length
      : closedTabs.findIndex(tab => {
          return tab.closedAt < tabData.closedAt;
        });

    if (index == -1) {
      index = closedTabs.length;
    }

    tabData.closedId = this._nextClosedId++;

    closedTabs.splice(index, 0, tabData);
    this._closedObjectsChanged = true;

    if (tabData.closedInGroup) {
      if (winData._lastClosedTabGroupCount < this._max_tabs_undo) {
        if (winData._lastClosedTabGroupCount < 0) {
          winData._lastClosedTabGroupCount = 1;
        } else {
          winData._lastClosedTabGroupCount++;
        }
      }
    } else {
      winData._lastClosedTabGroupCount = -1;
    }

    winData.lastClosedTabGroupId = tabData.closedInTabGroupId || null;

    if (saveAction) {
      this._addClosedAction(this._LAST_ACTION_CLOSED_TAB, tabData.closedId);
    }

    if (
      !tabData.closedInTabGroupId &&
      closedTabs.length > this._max_tabs_undo
    ) {
      closedTabs.splice(this._max_tabs_undo, closedTabs.length);
    }
  },

  removeClosedTabData(winData, closedTabs, index) {
    let [closedTab] = closedTabs.splice(index, 1);
    this._closedObjectsChanged = true;

    if (index < winData._lastClosedTabGroupCount) {
      winData._lastClosedTabGroupCount--;
    }

    if (closedTab.permanentKey) {
      this._closingTabMap.delete(closedTab.permanentKey);
      this._tabClosingByWindowMap.delete(closedTab.permanentKey);
      delete closedTab.permanentKey;
    }

    this._removeClosedAction(this._LAST_ACTION_CLOSED_TAB, closedTab.closedId);

    return closedTab;
  },

  onTabSelect: function ssi_onTabSelect(aWindow) {
    if (lazy.RunState.isRunning) {
      this._windows[aWindow.__SSi].selected =
        aWindow.gBrowser.tabContainer.selectedIndex;

      let tab = aWindow.gBrowser.selectedTab;
      this.maybeRestoreTabContent(tab);
    }
  },

  maybeRestoreTabContent(tab) {
    let browser = tab.linkedBrowser;

    if (TAB_STATE_FOR_BROWSER.get(browser) == TAB_STATE_NEEDS_RESTORE) {
      this.restoreTabContent(tab);
    }
  },

  onTabShow: function ssi_onTabShow(aWindow, aTab) {
    if (
      TAB_STATE_FOR_BROWSER.get(aTab.linkedBrowser) == TAB_STATE_NEEDS_RESTORE
    ) {
      TabRestoreQueue.hiddenToVisible(aTab);

      this.restoreNextTab();
    }

    this.saveStateDelayed(aWindow);
  },

  onTabHide: function ssi_onTabHide(aWindow, aTab) {
    if (
      TAB_STATE_FOR_BROWSER.get(aTab.linkedBrowser) == TAB_STATE_NEEDS_RESTORE
    ) {
      TabRestoreQueue.visibleToHidden(aTab);
    }

    this.saveStateDelayed(aWindow);
  },

  onBrowserCrashed(aBrowser) {
    this.enterCrashedState(aBrowser);
    lazy.TabStateFlusher.resolveAll(aBrowser);
  },

  enterCrashedState(browser) {
    this._crashedBrowsers.add(browser.permanentKey);

    let win = browser.documentGlobal;

    if (TAB_STATE_FOR_BROWSER.has(browser)) {
      let tab = win.gBrowser.getTabForBrowser(browser);
      if (tab) {
        this._resetLocalTabRestoringState(tab);
      }
    }
  },

  onIdleDaily() {
    this._cleanupOldData([this._closedWindows]);

    this._cleanupOldData(
      this._closedWindows.map(winData => winData._closedTabs)
    );

    this._cleanupOldData(
      this._closedWindows.map(winData => winData.closedGroups)
    );

    this._cleanupOldData(
      Object.keys(this._windows).map(key => this._windows[key]._closedTabs)
    );

    this._cleanupOldData(
      Object.keys(this._windows).map(key => this._windows[key].closedGroups)
    );

    this._notifyOfClosedObjectsChange();
  },

  _cleanupOldData(targets) {
    const TIME_TO_LIVE = this._prefBranch.getIntPref(
      "sessionstore.cleanup.forget_closed_after"
    );
    const now = Date.now();

    for (let array of targets) {
      for (let i = array.length - 1; i >= 0; --i) {
        let data = array[i];
        data.closedAt = data.closedAt || now;
        if (now - data.closedAt > TIME_TO_LIVE) {
          array.splice(i, 1);
          this._closedObjectsChanged = true;
        }
      }
    }
  },


  getBrowserState: function ssi_getBrowserState() {
    let state = this.getCurrentState();

    delete state.lastSessionState;

    delete state.deferredInitialState;

    return JSON.stringify(state);
  },

  setBrowserState: function ssi_setBrowserState(aState) {
    this._handleClosedWindows();

    try {
      var state = JSON.parse(aState);
    } catch (ex) {
    }
    if (!state) {
      throw Components.Exception(
        "Invalid state string: not JSON",
        Cr.NS_ERROR_INVALID_ARG
      );
    }
    if (!state.windows) {
      throw Components.Exception("No windows", Cr.NS_ERROR_INVALID_ARG);
    }

    this._maxSplitViewId = 0;
    this._initSplitViewIds(state);

    this._browserSetState = true;

    this._resetRestoringState();

    var window = this._getTopWindow();
    if (!window) {
      this._restoreCount = 1;
      this._openWindowWithState(state);
      return;
    }

    for (let otherWin of this._browserWindows) {
      if (otherWin != window) {
        otherWin.close();
        this.onClose(otherWin);
      }
    }

    if (this._closedWindows.length) {
      this._closedWindows = [];
      this._closedObjectsChanged = true;
    }

    this._restoreCount = state.windows ? state.windows.length : 0;

    this._globalState.setFromState(state);

    lazy.SessionCookies.restore(state.cookies || []);

    this.restoreWindows(window, state, { overwriteTabs: true });

    this._notifyOfClosedObjectsChange();
  },

  getWindowState: function ssi_getWindowState(aWindow) {
    if ("__SSi" in aWindow) {
      return Cu.cloneInto(this._getWindowState(aWindow), {});
    }

    if (DyingWindowCache.has(aWindow)) {
      let data = DyingWindowCache.get(aWindow);
      return Cu.cloneInto({ windows: [data] }, {});
    }

    throw Components.Exception(
      "Window is not tracked",
      Cr.NS_ERROR_INVALID_ARG
    );
  },

  setWindowState: function ssi_setWindowState(aWindow, aState, aOverwrite) {
    if (!aWindow.__SSi) {
      throw Components.Exception(
        "Window is not tracked",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    this.restoreWindows(aWindow, aState, { overwriteTabs: aOverwrite });

    this._notifyOfClosedObjectsChange();
  },

  getTabState: function ssi_getTabState(aTab) {
    if (!aTab || !aTab.documentGlobal) {
      throw Components.Exception("Need a valid tab", Cr.NS_ERROR_INVALID_ARG);
    }
    if (!aTab.documentGlobal.__SSi) {
      throw Components.Exception(
        "Default view is not tracked",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    let tabState = lazy.TabState.collect(aTab, TAB_CUSTOM_VALUES.get(aTab));

    return JSON.stringify(tabState);
  },

  setTabState(aTab, aState) {
    let tabState = aState;
    if (typeof tabState == "string") {
      tabState = JSON.parse(aState);
    }
    if (!tabState) {
      throw Components.Exception(
        "Invalid state string: not JSON",
        Cr.NS_ERROR_INVALID_ARG
      );
    }
    if (typeof tabState != "object") {
      throw Components.Exception("Not an object", Cr.NS_ERROR_INVALID_ARG);
    }
    if (!("entries" in tabState)) {
      throw Components.Exception(
        "Invalid state object: no entries",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    let window = aTab.documentGlobal;
    if (!window || !("__SSi" in window)) {
      throw Components.Exception(
        "Window is not tracked",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    if (TAB_STATE_FOR_BROWSER.has(aTab.linkedBrowser)) {
      this._resetTabRestoringState(aTab);
    }

    this._ensureNoNullsInTabDataList(
      window.gBrowser.tabs,
      this._windows[window.__SSi].tabs,
      aTab._tPos
    );
    this.restoreTab(aTab, tabState);

    this._notifyOfClosedObjectsChange();
  },

  getInternalObjectState(obj) {
    if (obj.__SSi) {
      return this._windows[obj.__SSi];
    }
    return obj.loadURI
      ? TAB_STATE_FOR_BROWSER.get(obj)
      : TAB_CUSTOM_VALUES.get(obj);
  },

  getObjectTypeForClosedId(aClosedId) {
    if (this.getClosedWindowDataByClosedId(aClosedId)) {
      return this._LAST_ACTION_CLOSED_WINDOW;
    }
    return this._LAST_ACTION_CLOSED_TAB;
  },

  getClosedWindowDataByClosedId: function ssi_getClosedWindowDataByClosedId(
    aClosedId
  ) {
    return this._closedWindows.find(
      closedData => closedData.closedId == aClosedId
    );
  },

  getWindowById: function ssi_getWindowById(aSessionStoreId) {
    let resultWindow;
    for (let window of this._browserWindows) {
      if (window.__SSi === aSessionStoreId) {
        resultWindow = window;
        break;
      }
    }
    return resultWindow;
  },

  duplicateTab: function ssi_duplicateTab(
    aWindow,
    aTab,
    aDelta = 0,
    aRestoreImmediately = true,
    { inBackground, tabIndex } = {}
  ) {
    if (!aTab || !aTab.documentGlobal) {
      throw Components.Exception("Need a valid tab", Cr.NS_ERROR_INVALID_ARG);
    }
    if (!aTab.documentGlobal.__SSi) {
      throw Components.Exception(
        "Default view is not tracked",
        Cr.NS_ERROR_INVALID_ARG
      );
    }
    if (!aWindow.gBrowser) {
      throw Components.Exception(
        "Invalid window object: no gBrowser",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    let userContextId = aTab.getAttribute("usercontextid") || "";

    let tabOptions = {
      userContextId,
      tabIndex,
      ...(aTab == aWindow.gBrowser.selectedTab
        ? { relatedToCurrent: true, ownerTab: aTab }
        : {}),
      skipLoad: true,
      preferredRemoteType: aTab.linkedBrowser.remoteType,
      tabGroup: aTab.group,
    };
    let newTab = aWindow.gBrowser.addTrustedTab(null, tabOptions);

    let uriObj = aTab.linkedBrowser.currentURI;
    if (!uriObj || (uriObj && !uriObj.schemeIs("about"))) {
      newTab.setAttribute("busy", "true");
    }

    aWindow.gBrowser.setDefaultIcon(newTab, uriObj);

    let tabState = lazy.TabState.collect(aTab, TAB_CUSTOM_VALUES.get(aTab));

    let browser = aTab.linkedBrowser;
    lazy.TabStateFlusher.flush(browser).then(() => {
      if (newTab.closing || !newTab.linkedBrowser) {
        return;
      }

      let window = newTab.documentGlobal;

      if (!window || !window.__SSi || window.closed) {
        return;
      }

      let options = { includePrivateData: true };
      lazy.TabState.copyFromCache(browser.permanentKey, tabState, options);

      tabState.index += aDelta;
      tabState.index = Math.max(
        1,
        Math.min(tabState.index, tabState.entries.length)
      );
      tabState.pinned = false;

      if (inBackground === false) {
        aWindow.gBrowser.selectedTab = newTab;
      }

      this.restoreTab(newTab, tabState, {
        restoreImmediately: aRestoreImmediately,
      });
    });

    return newTab;
  },

  getWindows(aWindowOrOptions) {
    let isPrivate;
    if (!aWindowOrOptions) {
      aWindowOrOptions = this._getTopWindow();
    }
    if (aWindowOrOptions instanceof Ci.nsIDOMWindow) {
      isPrivate = PrivateBrowsingUtils.isWindowPrivate(aWindowOrOptions);
    } else {
      isPrivate = Boolean(aWindowOrOptions.private);
    }

    const browserWindows = Array.from(this._browserWindows).filter(win => {
      return PrivateBrowsingUtils.isWindowPrivate(win) === isPrivate;
    });
    return browserWindows;
  },

  getWindowForTabClosedId(aClosedId, aIncludePrivate) {
    const privateValues = aIncludePrivate ? [false, true] : [false];
    for (let privateness of privateValues) {
      for (let window of this.getWindows({ private: privateness })) {
        const windowState = this._windows[window.__SSi];
        const closedTabs =
          this._getStateForClosedTabsAndClosedGroupTabs(windowState);
        if (!closedTabs.length) {
          continue;
        }
        if (closedTabs.find(tab => tab.closedId === aClosedId)) {
          return window;
        }
      }
    }
    return undefined;
  },

  getLastClosedTabCount(aWindow) {
    if ("__SSi" in aWindow) {
      return Math.min(
        Math.max(this._windows[aWindow.__SSi]._lastClosedTabGroupCount, 1),
        this.getClosedTabCountForWindow(aWindow)
      );
    }

    throw (Components.returnCode = Cr.NS_ERROR_INVALID_ARG);
  },

  resetLastClosedTabCount(aWindow) {
    if ("__SSi" in aWindow) {
      this._windows[aWindow.__SSi]._lastClosedTabGroupCount = -1;
      this._windows[aWindow.__SSi].lastClosedTabGroupId = null;
    } else {
      throw (Components.returnCode = Cr.NS_ERROR_INVALID_ARG);
    }
  },

  getClosedTabCountForWindow: function ssi_getClosedTabCountForWindow(aWindow) {
    if ("__SSi" in aWindow) {
      return this._getStateForClosedTabsAndClosedGroupTabs(
        this._windows[aWindow.__SSi]
      ).length;
    }

    if (!DyingWindowCache.has(aWindow)) {
      throw Components.Exception(
        "Window is not tracked",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    return this._getStateForClosedTabsAndClosedGroupTabs(
      DyingWindowCache.get(aWindow)
    ).length;
  },

  _prepareClosedTabOptions(aOptions = {}) {
    const sourceOptions = Object.assign(
      {
        closedTabsFromAllWindows: this._closedTabsFromAllWindowsEnabled,
        closedTabsFromClosedWindows: this._closedTabsFromClosedWindowsEnabled,
        sourceWindow: null,
      },
      aOptions instanceof Ci.nsIDOMWindow
        ? { sourceWindow: aOptions }
        : aOptions
    );
    if (!sourceOptions.sourceWindow) {
      sourceOptions.sourceWindow = this._getTopWindow(sourceOptions.private);
    }
    if (!sourceOptions.sourceWindow) {
      sourceOptions.private = false;
    }
    if (!sourceOptions.hasOwnProperty("private")) {
      sourceOptions.private = PrivateBrowsingUtils.isWindowPrivate(
        sourceOptions.sourceWindow
      );
    }
    return sourceOptions;
  },

  getClosedTabCount(aOptions) {
    const sourceOptions = this._prepareClosedTabOptions(aOptions);
    let tabCount = 0;

    if (sourceOptions.closedTabsFromAllWindows) {
      tabCount += this.getWindows({ private: sourceOptions.private })
        .map(win => this.getClosedTabCountForWindow(win))
        .reduce((total, count) => total + count, 0);
    } else {
      tabCount += this.getClosedTabCountForWindow(sourceOptions.sourceWindow);
    }

    if (!sourceOptions.private && sourceOptions.closedTabsFromClosedWindows) {
      tabCount += this.getClosedTabCountFromClosedWindows();
    }
    return tabCount;
  },

  getClosedTabCountFromClosedWindows:
    function ssi_getClosedTabCountFromClosedWindows() {
      const tabCount = this._closedWindows
        .map(
          winData =>
            this._getStateForClosedTabsAndClosedGroupTabs(winData).length
        )
        .reduce((total, count) => total + count, 0);
      return tabCount;
    },

  getClosedTabDataForWindow: function ssi_getClosedTabDataForWindow(aWindow) {
    return this._getClonedDataForWindow(
      aWindow,
      this._getStateForClosedTabsAndClosedGroupTabs
    );
  },

  getClosedTabData: function ssi_getClosedTabData(aOptions) {
    const sourceOptions = this._prepareClosedTabOptions(aOptions);
    const closedTabData = [];
    if (sourceOptions.closedTabsFromAllWindows) {
      for (let win of this.getWindows({ private: sourceOptions.private })) {
        closedTabData.push(...this.getClosedTabDataForWindow(win));
      }
    } else {
      closedTabData.push(
        ...this.getClosedTabDataForWindow(sourceOptions.sourceWindow)
      );
    }
    return closedTabData;
  },

  getClosedTabDataFromClosedWindows:
    function ssi_getClosedTabDataFromClosedWindows() {
      const closedTabData = [];
      for (let winData of this._closedWindows) {
        const sourceClosedId = winData.closedId;
        const closedTabs = Cu.cloneInto(
          this._getStateForClosedTabsAndClosedGroupTabs(winData),
          {}
        );
        for (let tabData of closedTabs) {
          tabData.sourceClosedId = sourceClosedId;
        }
        closedTabData.push(...closedTabs);
      }
      return closedTabData;
    },

  getClosedTabGroups: function ssi_getClosedTabGroups(aOptions) {
    const sourceOptions = this._prepareClosedTabOptions(aOptions);
    const closedTabGroups = [];
    if (sourceOptions.closedTabsFromAllWindows) {
      for (let win of this.getWindows({ private: sourceOptions.private })) {
        closedTabGroups.push(
          ...this._getClonedDataForWindow(win, w => w.closedGroups ?? [])
        );
      }
    } else if (sourceOptions.sourceWindow.closedGroups) {
      closedTabGroups.push(
        ...this._getClonedDataForWindow(
          sourceOptions.sourceWindow,
          w => w.closedGroups ?? []
        )
      );
    }

    if (sourceOptions.closedTabsFromClosedWindows) {
      for (let winData of this.getClosedWindowData()) {
        if (!winData.closedGroups) {
          continue;
        }
        for (let groupData of winData.closedGroups) {
          for (let tabData of groupData.tabs) {
            tabData.sourceClosedId = winData.closedId;
          }
        }
        closedTabGroups.push(...winData.closedGroups);
      }
    }
    return closedTabGroups;
  },

  getLastClosedTabGroupId(aWindow) {
    if ("__SSi" in aWindow) {
      return this._windows[aWindow.__SSi].lastClosedTabGroupId;
    }

    throw new Error("Window is not tracked");
  },

  _getClonedDataForWindow: function ssi_getClonedDataForWindow(
    aWindow,
    selector
  ) {
    let options = { wrapReflectors: true };
    let winData;

    if ("__SSi" in aWindow) {
      winData = this._windows[aWindow.__SSi];
    }

    if (!winData && !DyingWindowCache.has(aWindow)) {
      throw Components.Exception(
        "Window is not tracked",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    winData ??= DyingWindowCache.get(aWindow);
    let data = selector(winData);
    return Cu.cloneInto(data, {}, options);
  },

  _getStateForClosedTabsAndClosedGroupTabs:
    function ssi_getStateForClosedTabsAndClosedGroupTabs(winData, aIndex) {
      const closedGroups = winData.closedGroups ?? [];
      const closedTabs = winData._closedTabs ?? [];

      let result = [];
      let groupIdx = 0;
      let tabIdx = 0;
      let current = 0;
      let totalLength = closedGroups.length + closedTabs.length;

      while (current < totalLength) {
        let group = closedGroups[groupIdx];
        let tab = closedTabs[tabIdx];

        if (
          groupIdx < closedGroups.length &&
          (tabIdx >= closedTabs.length || group?.closedAt > tab?.closedAt)
        ) {
          group.tabs.forEach((groupTab, idx) => {
            groupTab._originalStateIndex = idx;
            groupTab._originalGroupStateIndex = groupIdx;
            result.push(groupTab);
          });
          groupIdx++;
        } else {
          tab._originalStateIndex = tabIdx;
          result.push(tab);
          tabIdx++;
        }

        current++;
        if (current > aIndex) {
          break;
        }
      }

      if (aIndex !== undefined) {
        return result[aIndex];
      }

      return result;
    },

  _getClosedTabStateFromUnifiedIndex: function ssi_getClosedTabForUnifiedIndex(
    sourceWinData,
    tabState
  ) {
    let closedTabSet, closedTabIndex;
    if (tabState._originalGroupStateIndex == null) {
      closedTabSet = sourceWinData._closedTabs;
    } else {
      closedTabSet =
        sourceWinData.closedGroups[tabState._originalGroupStateIndex].tabs;
    }
    closedTabIndex = tabState._originalStateIndex;

    return { closedTabSet, closedTabIndex };
  },

  undoCloseTab: function ssi_undoCloseTab(aSource, aIndex, aTargetWindow) {
    const sourceWinData = this._resolveClosedDataSource(aSource);
    const isPrivateSource = Boolean(sourceWinData.isPrivate);
    if (aTargetWindow && !aTargetWindow.__SSi) {
      throw Components.Exception(
        "Target window is not tracked",
        Cr.NS_ERROR_INVALID_ARG
      );
    } else if (!aTargetWindow) {
      aTargetWindow = this._getTopWindow(isPrivateSource);
    }
    if (
      isPrivateSource !== PrivateBrowsingUtils.isWindowPrivate(aTargetWindow)
    ) {
      throw Components.Exception(
        "Target window doesn't have the same privateness as the source window",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    aIndex = aIndex || 0;

    const closedTabState = this._getStateForClosedTabsAndClosedGroupTabs(
      sourceWinData,
      aIndex
    );
    if (!closedTabState) {
      throw Components.Exception(
        "Invalid index: not in the closed tabs",
        Cr.NS_ERROR_INVALID_ARG
      );
    }
    let { closedTabSet, closedTabIndex } =
      this._getClosedTabStateFromUnifiedIndex(sourceWinData, closedTabState);

    let { state, pos } = this.removeClosedTabData(
      sourceWinData,
      closedTabSet,
      closedTabIndex
    );
    this._cleanupOrphanedClosedGroups(sourceWinData);

    let url;
    if (state.entries?.length) {
      let activeIndex = (state.index || state.entries.length) - 1;
      activeIndex = Math.min(activeIndex, state.entries.length - 1);
      activeIndex = Math.max(activeIndex, 0);
      url = state.entries[activeIndex].url;
    }
    let preferredRemoteType = this.getPreferredRemoteType(
      url,
      aTargetWindow,
      state.userContextId
    );

    let tabbrowser = aTargetWindow.gBrowser;
    let tab = (tabbrowser.selectedTab = tabbrowser.addTrustedTab(null, {
      tabIndex: aSource == aTargetWindow ? pos : Infinity,
      pinned: state.pinned,
      userContextId: state.userContextId,
      skipLoad: true,
      preferredRemoteType,
      tabGroup: tabbrowser.tabGroups.find(g => g.id == state.groupId),
    }));

    this.restoreTab(tab, state);

    this._notifyOfClosedObjectsChange();

    return tab;
  },

  undoClosedTabFromClosedWindow: function ssi_undoClosedTabFromClosedWindow(
    aSource,
    aClosedId,
    aTargetWindow
  ) {
    const sourceWinData = this._resolveClosedDataSource(aSource);
    const closedTabs =
      this._getStateForClosedTabsAndClosedGroupTabs(sourceWinData);
    const closedIndex = closedTabs.findIndex(
      tabData => tabData.closedId == aClosedId
    );
    if (closedIndex >= 0) {
      return this.undoCloseTab(aSource, closedIndex, aTargetWindow);
    }
    throw Components.Exception(
      "Invalid closedId: not in the closed tabs",
      Cr.NS_ERROR_INVALID_ARG
    );
  },

  getPreferredRemoteType(url, aWindow, userContextId) {
    return ChromeUtils.predictRemoteTypeForURI(url, {
      window: aWindow,
      userContextId,
    });
  },

  _resolveClosedDataSource(aSource) {
    let winData;
    if (aSource instanceof Ci.nsIDOMWindow) {
      winData = this.getWindowStateData(aSource);
    } else if (aSource.sourceWindow instanceof Ci.nsIDOMWindow) {
      winData = this.getWindowStateData(aSource.sourceWindow);
    } else if (typeof aSource.sourceClosedId == "number") {
      winData = this.getClosedWindowDataByClosedId(aSource.sourceClosedId);
      if (!winData) {
        throw Components.Exception(
          "No such closed window",
          Cr.NS_ERROR_INVALID_ARG
        );
      }
    } else if (typeof aSource.sourceWindowId == "string") {
      let win = this.getWindowById(aSource.sourceWindowId);
      winData = this.getWindowStateData(win);
    } else {
      throw Components.Exception(
        "Invalid source object",
        Cr.NS_ERROR_INVALID_ARG
      );
    }
    return winData;
  },

  forgetClosedTab: function ssi_forgetClosedTab(aSource, aIndex) {
    const winData = this._resolveClosedDataSource(aSource);
    aIndex = aIndex || 0;
    if (!(aIndex in winData._closedTabs)) {
      throw Components.Exception(
        "Invalid index: not in the closed tabs",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    this.removeClosedTabData(winData, winData._closedTabs, aIndex);

    this._notifyOfClosedObjectsChange();
  },

  forgetClosedTabGroup: function ssi_forgetClosedTabGroup(aSource, tabGroupId) {
    const winData = this._resolveClosedDataSource(aSource);
    let closedGroupIndex = winData.closedGroups.findIndex(
      closedTabGroup => closedTabGroup.id == tabGroupId
    );
    if (closedGroupIndex < 0) {
      throw Components.Exception(
        "Closed tab group not found",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    let closedGroup = winData.closedGroups[closedGroupIndex];
    while (closedGroup.tabs.length) {
      this.removeClosedTabData(winData, closedGroup.tabs, 0);
    }
    winData.closedGroups.splice(closedGroupIndex, 1);

    this._notifyOfClosedObjectsChange();
  },

  forgetSavedTabGroup: function ssi_forgetSavedTabGroup(savedTabGroupId) {
    let savedGroupIndex = this._savedGroups.findIndex(
      savedTabGroup => savedTabGroup.id == savedTabGroupId
    );
    if (savedGroupIndex < 0) {
      throw Components.Exception(
        "Saved tab group not found",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    let savedGroup = this._savedGroups[savedGroupIndex];
    for (let i = 0; i < savedGroup.tabs.length; i++) {
      this.removeClosedTabData({}, savedGroup.tabs, i);
    }
    this._savedGroups.splice(savedGroupIndex, 1);
    this._notifyOfSavedTabGroupsChange();

    this._closedObjectsChanged = true;
    this._notifyOfClosedObjectsChange();
  },

  forgetClosedWindowById(aClosedId) {
    let closedIndex = this._closedWindows.findIndex(
      windowState => windowState.closedId == aClosedId
    );
    if (closedIndex < 0) {
      throw Components.Exception(
        "Invalid closedId: not in the closed windows",
        Cr.NS_ERROR_INVALID_ARG
      );
    }
    this.forgetClosedWindow(closedIndex);
  },

  forgetClosedTabById(aClosedId, aSourceOptions = {}) {
    let sourceWindowsData;
    let searchPrivateWindows = aSourceOptions.includePrivate ?? true;
    if (
      aSourceOptions instanceof Ci.nsIDOMWindow ||
      "sourceWindowId" in aSourceOptions ||
      "sourceClosedId" in aSourceOptions
    ) {
      sourceWindowsData = [this._resolveClosedDataSource(aSourceOptions)];
    } else {
      let browserWindows = Array.from(this._browserWindows);
      sourceWindowsData = [];
      for (let win of browserWindows) {
        if (
          !searchPrivateWindows &&
          PrivateBrowsingUtils.isWindowPrivate(win)
        ) {
          continue;
        }
        sourceWindowsData.push(this._windows[win.__SSi]);
      }
    }

    for (let winData of sourceWindowsData) {
      let closedTabs = this._getStateForClosedTabsAndClosedGroupTabs(winData);
      let closedTabState = closedTabs.find(
        tabData => tabData.closedId == aClosedId
      );

      if (closedTabState) {
        let { closedTabSet, closedTabIndex } =
          this._getClosedTabStateFromUnifiedIndex(winData, closedTabState);
        this.removeClosedTabData(winData, closedTabSet, closedTabIndex);
        this._notifyOfClosedObjectsChange();
        return;
      }
    }

    throw Components.Exception(
      "Invalid closedId: not found in the closed tabs of any window",
      Cr.NS_ERROR_INVALID_ARG
    );
  },

  getClosedWindowCount: function ssi_getClosedWindowCount() {
    return this._closedWindows.length;
  },

  getClosedWindowData: function ssi_getClosedWindowData() {
    let closedWindows = Cu.cloneInto(this._closedWindows, {});
    for (let closedWinData of closedWindows) {
      this._trimSavedTabGroupMetadataInClosedWindow(closedWinData);
    }
    return closedWindows;
  },

  _trimSavedTabGroupMetadataInClosedWindow(closedWinData) {
    let abbreviatedGroups = closedWinData.groups?.map(tabGroup =>
      lazy.TabGroupState.abbreviated(tabGroup)
    );
    closedWinData.groups = Cu.cloneInto(abbreviatedGroups, {});
  },

  maybeDontRestoreTabs(aWindow) {
    this._windows[aWindow.__SSi]._maybeDontRestoreTabs = true;
  },

  isLastRestorableWindow() {
    return (
      Object.values(this._windows).filter(winData => !winData.isPrivate)
        .length == 1 &&
      !this._closedWindows.some(win => win._shouldRestore || false)
    );
  },

  undoCloseWindow: function ssi_undoCloseWindow(aIndex) {
    if (!(aIndex in this._closedWindows)) {
      throw Components.Exception(
        "Invalid index: not in the closed windows",
        Cr.NS_ERROR_INVALID_ARG
      );
    }
    let state = { windows: this._removeClosedWindow(aIndex) };
    delete state.windows[0].closedAt; 

    this._trimSavedTabGroupMetadataInClosedWindow(state.windows[0]);
    for (let tabGroup of state.windows[0].groups ?? []) {
      if (this.getSavedTabGroup(tabGroup.id)) {
        this.forgetSavedTabGroup(tabGroup.id);
      }
    }

    let window = this._openWindowWithState(state);
    this.windowToFocus = window;
    WINDOW_SHOWING_PROMISES.get(window).promise.then(win =>
      this.restoreWindows(win, state, {
        overwriteTabs: true,
        trigger: "undo_close",
      })
    );

    this._notifyOfClosedObjectsChange();

    return window;
  },

  forgetClosedWindow: function ssi_forgetClosedWindow(aIndex) {
    aIndex = aIndex || 0;
    if (!(aIndex in this._closedWindows)) {
      throw Components.Exception(
        "Invalid index: not in the closed windows",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    let winData = this._closedWindows[aIndex];
    this._removeClosedWindow(aIndex);
    this._saveableClosedWindowData.delete(winData);

    this._notifyOfClosedObjectsChange();
  },

  getCustomWindowValue(aWindow, aKey) {
    if ("__SSi" in aWindow) {
      let data = this._windows[aWindow.__SSi].extData || {};
      return data[aKey] || "";
    }

    if (DyingWindowCache.has(aWindow)) {
      let data = DyingWindowCache.get(aWindow).extData || {};
      return data[aKey] || "";
    }

    throw Components.Exception(
      "Window is not tracked",
      Cr.NS_ERROR_INVALID_ARG
    );
  },

  setCustomWindowValue(aWindow, aKey, aStringValue) {
    if (typeof aStringValue != "string") {
      throw new TypeError("setCustomWindowValue only accepts string values");
    }

    if (!("__SSi" in aWindow)) {
      throw Components.Exception(
        "Window is not tracked",
        Cr.NS_ERROR_INVALID_ARG
      );
    }
    if (!this._windows[aWindow.__SSi].extData) {
      this._windows[aWindow.__SSi].extData = {};
    }
    this._windows[aWindow.__SSi].extData[aKey] = aStringValue;
    this.saveStateDelayed(aWindow);
  },

  deleteCustomWindowValue(aWindow, aKey) {
    if (
      aWindow.__SSi &&
      this._windows[aWindow.__SSi].extData &&
      this._windows[aWindow.__SSi].extData[aKey]
    ) {
      delete this._windows[aWindow.__SSi].extData[aKey];
    }
    this.saveStateDelayed(aWindow);
  },

  getCustomTabValue(aTab, aKey) {
    return (TAB_CUSTOM_VALUES.get(aTab) || {})[aKey] || "";
  },

  setCustomTabValue(aTab, aKey, aStringValue) {
    if (typeof aStringValue != "string") {
      throw new TypeError("setCustomTabValue only accepts string values");
    }

    if (!TAB_CUSTOM_VALUES.has(aTab)) {
      TAB_CUSTOM_VALUES.set(aTab, {});
    }

    TAB_CUSTOM_VALUES.get(aTab)[aKey] = aStringValue;
    this.saveStateDelayed(aTab.documentGlobal);
  },

  deleteCustomTabValue(aTab, aKey) {
    let state = TAB_CUSTOM_VALUES.get(aTab);
    if (state && aKey in state) {
      delete state[aKey];
      this.saveStateDelayed(aTab.documentGlobal);
    }
  },

  moveCustomTabValue(aFromTab, aToTab) {
    let state = TAB_CUSTOM_VALUES.get(aFromTab);
    if (state) {
      TAB_CUSTOM_VALUES.set(aToTab, state);
      TAB_CUSTOM_VALUES.delete(aFromTab);
    }
  },

  getLazyTabValue(aTab, aKey) {
    return (TAB_LAZY_STATES.get(aTab) || {})[aKey];
  },

  getCustomGlobalValue(aKey) {
    return this._globalState.get(aKey);
  },

  setCustomGlobalValue(aKey, aStringValue) {
    if (typeof aStringValue != "string") {
      throw new TypeError("setCustomGlobalValue only accepts string values");
    }

    this._globalState.set(aKey, aStringValue);
    this.saveStateDelayed();
  },

  deleteCustomGlobalValue(aKey) {
    this._globalState.delete(aKey);
    this.saveStateDelayed();
  },

  undoCloseById(aClosedId, aIncludePrivate = true, aTargetWindow) {
    for (let i = 0, l = this._closedWindows.length; i < l; i++) {
      if (this._closedWindows[i].closedId == aClosedId) {
        return this.undoCloseWindow(i);
      }
    }

    for (let sourceWindow of Services.wm.getEnumerator("navigator:browser")) {
      if (
        !aIncludePrivate &&
        PrivateBrowsingUtils.isWindowPrivate(sourceWindow)
      ) {
        continue;
      }
      let windowState = this._windows[sourceWindow.__SSi];
      if (windowState) {
        let closedTabs =
          this._getStateForClosedTabsAndClosedGroupTabs(windowState);
        for (let j = 0, l = closedTabs.length; j < l; j++) {
          if (closedTabs[j].closedId == aClosedId) {
            return this.undoCloseTab(sourceWindow, j, aTargetWindow);
          }
        }
      }
    }

    return undefined;
  },

  updateTabLabelAndIcon(tab, tabData = null) {
    if (tab.hasAttribute("customizemode")) {
      return;
    }

    let browser = tab.linkedBrowser;
    let win = browser.documentGlobal;

    if (!tabData) {
      tabData = lazy.TabState.collect(tab, TAB_CUSTOM_VALUES.get(tab));
      if (!tabData) {
        throw new Error("tabData not found for given tab");
      }
    }

    let activePageData = tabData.entries[tabData.index - 1] || null;

    if (activePageData) {
      if (activePageData.title && activePageData.title != activePageData.url) {
        win.gBrowser.setInitialTabTitle(tab, activePageData.title, {
          isContentTitle: true,
        });
      } else {
        win.gBrowser.setInitialTabTitle(tab, activePageData.url);
      }
    }

    if ("image" in tabData) {
      if (
        !activePageData ||
        (activePageData && activePageData.url != "about:blank")
      ) {
        win.gBrowser.setIcon(tab, tabData.image);
      }
      lazy.TabStateCache.update(browser.permanentKey, {
        image: null,
      });
    }
  },

  _forgetTabsWithUserContextId(userContextId) {
    for (let window of Services.wm.getEnumerator("navigator:browser")) {
      let windowState = this._windows[window.__SSi];
      if (windowState) {
        let indexes = [];
        windowState._closedTabs.forEach((closedTab, index) => {
          if (closedTab.state.userContextId == userContextId) {
            indexes.push(index);
          }
        });

        for (let index of indexes.reverse()) {
          this.removeClosedTabData(windowState, windowState._closedTabs, index);
        }
      }
    }

    this._notifyOfClosedObjectsChange();
  },

  restoreLastSession: function ssi_restoreLastSession() {
    if (!this.canRestoreLastSession) {
      throw Components.Exception("Last session can not be restored");
    }

    Services.obs.notifyObservers(null, NOTIFY_INITIATING_MANUAL_RESTORE);

    let windows = {};
    for (let window of this._browserWindows) {
      if (window.__SS_lastSessionWindowID) {
        windows[window.__SS_lastSessionWindowID] = window;
      }
    }

    let lastSessionState = LastSession.getState();

    if (!lastSessionState.windows.length) {
      throw Components.Exception(
        "lastSessionState has no windows",
        Cr.NS_ERROR_UNEXPECTED
      );
    }

    this._restoreCount = lastSessionState.windows.length;
    this._browserSetState = true;

    let lastWindow = this._getTopWindow();
    let canUseLastWindow = lastWindow && !lastWindow.__SS_lastSessionWindowID;

    this._globalState.setFromState(lastSessionState);

    let openWindows = [];
    let windowsToOpen = [];

    lazy.SessionCookies.restore(lastSessionState.cookies || []);

    for (let i = 0; i < lastSessionState.windows.length; i++) {
      let winState = lastSessionState.windows[i];

      if (this._restoreWithoutRestart) {
        let restoreIndex = this._closedWindows.findIndex(win => {
          return win.closedId == winState.closedId;
        });
        if (restoreIndex > -1) {
          this._closedWindows.splice(restoreIndex, 1);
        }
      }

      let lastSessionWindowID = winState.__lastSessionWindowID;
      delete winState.__lastSessionWindowID;

      let windowToUse = windows[lastSessionWindowID];
      if (!windowToUse && canUseLastWindow) {
        windowToUse = lastWindow;
        canUseLastWindow = false;
      }

      let [canUseWindow, canOverwriteTabs] =
        this._prepWindowToRestoreInto(windowToUse);

      if (canUseWindow) {
        if (!PERSIST_SESSIONS) {
          if (winState._closedTabs && winState._closedTabs.length) {
            let curWinState = this._windows[windowToUse.__SSi];
            curWinState._closedTabs = curWinState._closedTabs.concat(
              winState._closedTabs
            );
            curWinState._closedTabs.splice(
              this._max_tabs_undo,
              curWinState._closedTabs.length
            );
          }
        }
        this._updateWindowRestoreState(windowToUse, {
          windows: [winState],
          options: { overwriteTabs: canOverwriteTabs },
        });
        openWindows.push(windowToUse);
      } else {
        windowsToOpen.push(winState);
      }
    }

    this._openWindows({ windows: windowsToOpen }).then(openedWindows =>
      this._restoreWindowsInReversedZOrder(openWindows.concat(openedWindows))
    );

    if (this._restoreWithoutRestart) {
      this.removeDuplicateClosedWindows(lastSessionState);
    }

    if (lastSessionState._closedWindows) {
      for (let closedWindow of lastSessionState._closedWindows) {
        closedWindow.closedId = this._nextClosedId++;
        if (closedWindow._closedTabs?.length) {
          this._resetClosedTabIds(
            closedWindow._closedTabs,
            closedWindow.closedId
          );
        }
      }
      this._closedWindows = this._closedWindows.concat(
        lastSessionState._closedWindows
      );
      this._capClosedWindows();
      this._closedObjectsChanged = true;
    }

    let groupsToRemove = this._savedGroups.filter(
      group => group.removeAfterRestore
    );
    for (let group of groupsToRemove) {
      this.forgetSavedTabGroup(group.id);
    }

    this._recentCrashes =
      (lastSessionState.session && lastSessionState.session.recentCrashes) || 0;

    this._updateSessionStartTime(lastSessionState);

    LastSession.clear();

    this._notifyOfClosedObjectsChange();
  },

  removeDuplicateClosedWindows(lastSessionState) {
    let currentClosedIds = new Set(
      this._closedWindows.map(window => window.closedId)
    );

    lastSessionState._closedWindows = lastSessionState._closedWindows.filter(
      win => !currentClosedIds.has(win.closedId)
    );
  },

  reviveCrashedTab(aTab) {
    if (!aTab) {
      throw new Error(
        "SessionStore.reviveCrashedTab expected a tab, but got null."
      );
    }

    let browser = aTab.linkedBrowser;
    if (!this._crashedBrowsers.has(browser.permanentKey)) {
      return;
    }

    if (browser.isRemoteBrowser) {
      throw new Error(
        "SessionStore.reviveCrashedTab: " +
          "Somehow a crashed browser is still remote."
      );
    }

    aTab.removeAttribute("crashed");

    browser.loadURI(lazy.blankURI, {
      triggeringPrincipal: Services.scriptSecurityManager.createNullPrincipal({
        userContextId: aTab.userContextId,
      }),
      remoteTypeOverride: lazy.E10SUtils.NOT_REMOTE,
    });

    let data = lazy.TabState.collect(aTab, TAB_CUSTOM_VALUES.get(aTab));
    this.restoreTab(aTab, data, {
      forceOnDemand: true,
    });
  },

  reviveAllCrashedTabs() {
    for (let window of Services.wm.getEnumerator("navigator:browser")) {
      for (let tab of window.gBrowser.tabs) {
        this.reviveCrashedTab(tab);
      }
    }
  },

  getSessionHistory(tab, updatedCallback) {
    if (updatedCallback) {
      lazy.TabStateFlusher.flush(tab.linkedBrowser).then(() => {
        let sessionHistory = this.getSessionHistory(tab);
        if (sessionHistory) {
          updatedCallback(sessionHistory);
        }
      });
    }

    if (tab.linkedBrowser) {
      let tabState = lazy.TabState.collect(tab, TAB_CUSTOM_VALUES.get(tab));
      return { index: tabState.index - 1, entries: tabState.entries };
    }
    return null;
  },

  _prepWindowToRestoreInto: function ssi_prepWindowToRestoreInto(aWindow) {
    if (!aWindow) {
      return [false, false];
    }

    let canOverwriteTabs = false;

    let homePages = ["about:blank"];
    let removableTabs = [];
    let tabbrowser = aWindow.gBrowser;
    let startupPref = this._prefBranch.getIntPref("startup.page");
    if (startupPref == 1) {
      homePages = homePages.concat(lazy.HomePage.get(aWindow).split("|"));
    }

    for (let i = tabbrowser.pinnedTabCount; i < tabbrowser.tabs.length; i++) {
      let tab = tabbrowser.tabs[i];
      if (homePages.includes(tab.linkedBrowser.currentURI.spec)) {
        removableTabs.push(tab);
      }
    }

    if (
      tabbrowser.tabs.length > tabbrowser.visibleTabs.length &&
      tabbrowser.visibleTabs.length === removableTabs.length
    ) {
      removableTabs.shift();
    }

    if (tabbrowser.tabs.length == removableTabs.length) {
      canOverwriteTabs = true;
    } else {
      for (let i = removableTabs.length - 1; i >= 0; i--) {
        tabbrowser.removeTab(removableTabs.pop(), { animate: false });
      }
    }

    return [true, canOverwriteTabs];
  },


  _updateWindowFeatures: function ssi_updateWindowFeatures(aWindow) {
    var winData = this._windows[aWindow.__SSi];

    WINDOW_ATTRIBUTES.forEach(function (aAttr) {
      winData[aAttr] = this._getWindowDimension(aWindow, aAttr);
    }, this);

    if (winData.sizemode != "minimized") {
      winData.sizemodeBeforeMinimized = winData.sizemode;
    }

    var hidden = WINDOW_HIDEABLE_FEATURES.filter(function (aItem) {
      return aWindow[aItem] && !aWindow[aItem].visible;
    });
    if (hidden.length) {
      winData.hidden = hidden.join(",");
    } else if (winData.hidden) {
      delete winData.hidden;
    }

    const sidebarUIState = aWindow.SidebarController?.getUIState();
    if (sidebarUIState) {
      winData.sidebar = structuredClone(sidebarUIState);
    }

    let workspaceID = aWindow.getWorkspaceID();
    if (workspaceID) {
      winData.workspaceID = workspaceID;
    }

  },

  getCurrentState(aUpdateAll) {
    this._handleClosedWindows().then(() => {
      this._notifyOfClosedObjectsChange();
    });

    var activeWindow = this._getTopWindow();

    if (lazy.RunState.isRunning) {
      let index = 0;
      for (let window of this._orderedBrowserWindows) {
        if (!this._isWindowLoaded(window)) {
          continue;
        }
        if (aUpdateAll || DirtyWindows.has(window) || window == activeWindow) {
          this._collectWindowData(window);
        } else {
          this._updateWindowFeatures(window);
        }
        this._windows[window.__SSi].zIndex = ++index;
      }
      DirtyWindows.clear();
    }

    var total = [];
    var ids = [];
    var nonPopupCount = 0;
    var ix;

    for (ix in this._windows) {
      if (this._windows[ix]._restoring || this._windows[ix].isTaskbarTab) {
        continue;
      }
      total.push(this._windows[ix]);
      ids.push(ix);
      if (!this._windows[ix].isPopup) {
        nonPopupCount++;
      }
    }

    for (ix in this._statesToRestore) {
      for (let winData of this._statesToRestore[ix].windows) {
        total.push(winData);
        if (!winData.isPopup) {
          nonPopupCount++;
        }
      }
    }

    let lastClosedWindowsCopy = this._closedWindows.slice();

    if (AppConstants.platform != "macosx") {
      if (
        nonPopupCount == 0 &&
        !!lastClosedWindowsCopy.length &&
        lazy.RunState.isQuitting
      ) {
        do {
          total.unshift(lastClosedWindowsCopy.shift());
        } while (total[0].isPopup && lastClosedWindowsCopy.length);
      }
    }

    if (activeWindow) {
      this.activeWindowSSiCache = activeWindow.__SSi || "";
    }
    ix = ids.indexOf(this.activeWindowSSiCache);
    if (ix != -1 && total[ix] && total[ix].sizemode == "minimized") {
      ix = -1;
    }

    let session = {
      lastUpdate: Date.now(),
      startTime: this._sessionStartTime,
      recentCrashes: this._recentCrashes,
    };

    let state = {
      version: ["sessionrestore", FORMAT_VERSION],
      windows: total,
      selectedWindow: ix + 1,
      _closedWindows: lastClosedWindowsCopy,
      savedGroups: this._savedGroups,
      maxSplitViewId: this._maxSplitViewId,
      session,
      global: this._globalState.getState(),
    };

    state.cookies = lazy.SessionCookies.collect();

    if (LastSession.canRestore) {
      state.lastSessionState = LastSession.getState();
    }

    if (this._deferredInitialState) {
      state.deferredInitialState = this._deferredInitialState;
    }

    return state;
  },

  _getWindowState: function ssi_getWindowState(aWindow) {
    if (!this._isWindowLoaded(aWindow)) {
      return this._statesToRestore[WINDOW_RESTORE_IDS.get(aWindow)];
    }

    if (lazy.RunState.isRunning) {
      this._collectWindowData(aWindow);
    }

    return { windows: [this._windows[aWindow.__SSi]] };
  },

  getWindowStateData: function ssi_getWindowStateData(aWindow) {
    if (!aWindow.__SSi || !(aWindow.__SSi in this._windows)) {
      throw Components.Exception(
        "Window is not tracked",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    return this._windows[aWindow.__SSi];
  },

  _collectWindowData: function ssi_collectWindowData(aWindow) {
    let tabMap = new Map();

    if (!this._isWindowLoaded(aWindow)) {
      return tabMap;
    }

    let tabbrowser = aWindow.gBrowser;
    let tabs = tabbrowser.tabs;
    let winData = this._windows[aWindow.__SSi];
    let tabsData = (winData.tabs = []);

    for (let tab of tabs) {
      let tabData = lazy.TabState.collect(tab, TAB_CUSTOM_VALUES.get(tab));
      tabMap.set(tab, tabData);
      tabsData.push(tabData);
    }

    winData.groups = [];
    for (let tabGroup of aWindow.gBrowser.tabGroups) {
      let tabGroupData = lazy.TabGroupState.collect(tabGroup);
      winData.groups.push(tabGroupData);
    }
    winData.splitViews = [];
    for (let splitView of aWindow.gBrowser.splitViews) {
      let splitViewData = splitView.state;
      winData.splitViews.push(splitViewData);
    }
    let selectedIndex = tabbrowser.tabbox.selectedIndex + 1;
    winData.selected = selectedIndex;

    this._updateWindowFeatures(aWindow);

    if (aWindow.__SS_lastSessionWindowID) {
      this._windows[aWindow.__SSi].__lastSessionWindowID =
        aWindow.__SS_lastSessionWindowID;
    }

    DirtyWindows.remove(aWindow);
    return tabMap;
  },


  _openWindows(root) {
    let windowsOpened = [];
    for (let winData of root.windows) {
      if (!winData || !winData.tabs || !winData.tabs[0]) {
        this._log.debug(`_openWindows, skipping window with no tabs data`);
        this._restoreCount--;
        continue;
      }
      windowsOpened.push(this._openWindowWithState({ windows: [winData] }));
    }
    let windowOpenedPromises = [];
    for (const openedWindow of windowsOpened) {
      let deferred = WINDOW_SHOWING_PROMISES.get(openedWindow);
      windowOpenedPromises.push(deferred.promise);
    }
    return Promise.all(windowOpenedPromises);
  },

  _resetClosedTabIds(tabData, windowId) {
    for (let entry of tabData) {
      entry.closedId = this._nextClosedId++;
      entry.sourceWindowId = windowId;
    }
    return tabData;
  },

  _initSplitViewIds(state) {
    if (this._maxSplitViewId > 0) {
      this._log.error(
        `In _initSplitViewIds, _maxSplitViewId already has a value: ${this._maxSplitViewId}`
      );
    }
    for (let session of [
      state.deferredInitialState,
      state.lastSessionState,
      state,
    ]) {
      if (!session) {
        continue;
      }
      this._migrateSplitViewIds(session);
      this._maxSplitViewId = Math.max(
        this._maxSplitViewId,
        session.maxSplitViewId
      );
    }
  },

  _migrateSplitViewIds(state) {
    if (typeof state.maxSplitViewId == "number") {
      return;
    }
    let oldToNewMap = new Map();
    let windowsData = [...state.windows];
    if (state._closedWindows?.length) {
      windowsData.push.apply(windowsData, state._closedWindows);
    }
    for (let winData of windowsData) {
      if (!winData || !winData.tabs?.length) {
        continue;
      }

      for (let tabData of winData.tabs) {
        let idType = typeof tabData.splitViewId;
        if (idType === "undefined") {
          continue;
        }
        if (idType === "number") {
          this._maxSplitViewId = Math.max(
            this._maxSplitViewId,
            tabData.splitViewId
          );
          continue;
        }
        if (!oldToNewMap.has(tabData.splitViewId)) {
          oldToNewMap.set(
            tabData.splitViewId,
            SessionStore.getNextSplitViewId()
          );
          this._log.debug(
            `Migrating splitViewId: "${tabData.splitViewId}" -> ${oldToNewMap.get(tabData.splitViewId)}`
          );
        }
        tabData.splitViewId = oldToNewMap.get(tabData.splitViewId);
      }

      if (winData.splitViews) {
        for (let splitViewData of winData.splitViews) {
          if (oldToNewMap.has(splitViewData.id)) {
            splitViewData.id = oldToNewMap.get(splitViewData.id);
          }
        }
      }
    }
    state.maxSplitViewId = this._maxSplitViewId;
  },

  restoreWindow: function ssi_restoreWindow(aWindow, winData, aOptions = {}) {
    let overwriteTabs = aOptions && aOptions.overwriteTabs;
    let firstWindow = aOptions && aOptions.firstWindow;

    this.restoreSidebar(aWindow, winData.sidebar, winData.isPopup);

    if (aWindow && (!aWindow.__SSi || !this._windows[aWindow.__SSi])) {
      this.onLoad(aWindow);
    }

    this._sendWindowRestoringNotification(aWindow);
    this._setWindowStateBusy(aWindow);

    if (winData.workspaceID && lazy.gRestoreWindowsToVirtualDesktop) {
      this._log.debug(`Moving window to workspace: ${winData.workspaceID}`);
      aWindow.moveToWorkspace(winData.workspaceID);
    }

    if (!winData.tabs) {
      winData.tabs = [];
    } else if (
      firstWindow &&
      !overwriteTabs &&
      winData.tabs.length == 1 &&
      (!winData.tabs[0].entries || !winData.tabs[0].entries.length)
    ) {
      winData.tabs = [];
    }

    let selectTab = 0;
    if (overwriteTabs) {
      selectTab = parseInt(winData.selected || 1, 10);
      selectTab = Math.max(selectTab, 1);
      selectTab = Math.min(selectTab, winData.tabs.length);
    }

    let tabbrowser = aWindow.gBrowser;

    let arrowScrollbox = tabbrowser.tabContainer.arrowScrollbox;
    let smoothScroll = arrowScrollbox.smoothScroll;
    arrowScrollbox.smoothScroll = false;

    let initialTabs;
    if (!overwriteTabs && firstWindow) {
      initialTabs = Array.from(tabbrowser.tabs);
    }

    if (overwriteTabs) {
      for (let i = tabbrowser.browsers.length - 1; i >= 0; i--) {
        if (!tabbrowser.tabs[i].selected) {
          tabbrowser.removeTab(tabbrowser.tabs[i]);
        }
      }
    }

    let restoreTabsLazily =
      this._prefBranch.getBoolPref("sessionstore.restore_tabs_lazily") &&
      this._restore_on_demand;

    this._log.debug(
      `restoreWindow, will restore ${winData.tabs.length} tabs and ${
        winData.groups?.length ?? 0
      } tab groups and ${winData.splitViews?.length ?? 0} splitviews, restoreTabsLazily: ${restoreTabsLazily}`
    );
    if (winData.tabs.length) {
      var tabs = tabbrowser.createTabsForSessionRestore(
        restoreTabsLazily,
        selectTab,
        winData.tabs,
        winData.groups ?? [],
        winData.splitViews ?? []
      );
      this._log.debug(
        `restoreWindow, createTabsForSessionRestore returned ${tabs.length} tabs`
      );
      const openTabGroupIdsInWindow = new Set(
        tabbrowser.tabGroups.map(group => group.id)
      );
      this._savedGroups = this._savedGroups.filter(
        savedTabGroup => !openTabGroupIdsInWindow.has(savedTabGroup.id)
      );
    }

    if (initialTabs) {
      let endPosition = tabbrowser.tabs.length - 1;
      for (let i = 0; i < initialTabs.length; i++) {
        tabbrowser.unpinTab(initialTabs[i]);
        tabbrowser.moveTabTo(initialTabs[i], {
          tabIndex: endPosition,
          forceUngrouped: true,
        });
      }
    }

    delete aWindow.__SS_lastSessionWindowID;
    if (winData.__lastSessionWindowID) {
      aWindow.__SS_lastSessionWindowID = winData.__lastSessionWindowID;
    }

    if (overwriteTabs) {
      delete this._windows[aWindow.__SSi].extData;
    }

    lazy.SessionCookies.restore(winData.cookies || []);

    if (winData.extData) {
      if (!this._windows[aWindow.__SSi].extData) {
        this._windows[aWindow.__SSi].extData = {};
      }
      for (var key in winData.extData) {
        this._windows[aWindow.__SSi].extData[key] = winData.extData[key];
      }
    }

    let newClosedTabsData;
    if (winData._closedTabs) {
      newClosedTabsData = winData._closedTabs;
      this._resetClosedTabIds(newClosedTabsData, aWindow.__SSi);
    } else {
      newClosedTabsData = [];
    }

    let newLastClosedTabGroupCount = winData._lastClosedTabGroupCount || -1;

    if (overwriteTabs || firstWindow) {
      this._windows[aWindow.__SSi]._closedTabs = newClosedTabsData;
    } else if (this._max_tabs_undo > 0) {
      if (PERSIST_SESSIONS) {
        newClosedTabsData = this._windows[aWindow.__SSi]._closedTabs.filter(
          tab => !tab.removeAfterRestore
        );
      } else {
        newClosedTabsData = newClosedTabsData.concat(
          this._windows[aWindow.__SSi]._closedTabs
        );
      }

      this._windows[aWindow.__SSi]._closedTabs = newClosedTabsData.slice(
        0,
        this._max_tabs_undo
      );
    }
    this._windows[aWindow.__SSi]._lastClosedTabGroupCount =
      newLastClosedTabGroupCount;

    let newClosedTabGroupsData = winData.closedGroups || [];
    newClosedTabGroupsData.forEach(group => {
      this._resetClosedTabIds(group.tabs, aWindow.__SSi);
    });
    this._windows[aWindow.__SSi].closedGroups = newClosedTabGroupsData;
    this._windows[aWindow.__SSi].lastClosedTabGroupId =
      winData.lastClosedTabGroupId || null;

    if (!this._isWindowLoaded(aWindow)) {
      delete this._statesToRestore[WINDOW_RESTORE_IDS.get(aWindow)];
      WINDOW_RESTORE_IDS.delete(aWindow);
      delete this._windows[aWindow.__SSi]._restoring;
    }

    if (winData.tabs.length) {
      this.restoreTabs(aWindow, tabs, winData.tabs, selectTab);
    }

    arrowScrollbox.smoothScroll = smoothScroll;

  },

  prepareConnectionToHost(tab, url) {
    if (url && !url.startsWith("about:")) {
      let principal = Services.scriptSecurityManager.createNullPrincipal({
        userContextId: tab.userContextId,
      });
      let browsingContext = tab.linkedBrowser.browsingContext;
      let callbacks = {
        QueryInterface: ChromeUtils.generateQI(["nsIInterfaceRequestor"]),
        getInterface(iid) {
          if (iid.equals(Ci.nsILoadContext)) {
            return browsingContext;
          }
          throw Components.Exception("", Cr.NS_ERROR_NO_INTERFACE);
        },
      };
      try {
        let uri = Services.io.newURI(url);
        Services.io.speculativeConnect(uri, principal, callbacks, false);
        return true;
      } catch (error) {
        return false;
      }
    }
    return false;
  },

  speculativeConnectOnTabHover(tab) {
    let tabState = TAB_LAZY_STATES.get(tab);
    if (tabState && !tabState.connectionPrepared) {
      let url = this.getLazyTabValue(tab, "url");
      let prepared = this.prepareConnectionToHost(tab, url);
      if (gDebuggingEnabled) {
        tab.__test_connection_prepared = prepared;
        tab.__test_connection_url = url;
      }
      tabState.connectionPrepared = true;
    }
  },

  _restoreWindowsFeaturesAndTabs(windows) {
    let resizePromises = [];
    for (let window of windows) {
      let state = this._statesToRestore[WINDOW_RESTORE_IDS.get(window)];
      resizePromises.push(
        this.restoreWindowFeatures(window, state.windows[0], state.options)
      );
    }

    for (let window of windows) {
      let state = this._statesToRestore[WINDOW_RESTORE_IDS.get(window)];
      this.restoreWindow(
        window,
        state.windows[0],
        state.options || { overwriteTabs: true }
      );
      WINDOW_RESTORE_ZINDICES.delete(window);
    }
    for (let resizePromise of resizePromises) {
      resizePromise.then(resizedWindow => {
        this._setWindowStateReady(resizedWindow);

        this._sendWindowRestoredNotification(resizedWindow);

        Services.obs.notifyObservers(
          resizedWindow,
          NOTIFY_SINGLE_WINDOW_RESTORED
        );

        this._sendRestoreCompletedNotifications();
      });
    }
  },

  _restoreWindowsInReversedZOrder(windows) {
    windows.sort(
      (a, b) =>
        (WINDOW_RESTORE_ZINDICES.get(a) || 0) -
        (WINDOW_RESTORE_ZINDICES.get(b) || 0)
    );

    this.windowToFocus = windows[0];
    this._restoreWindowsFeaturesAndTabs(windows);
  },

  restoreWindows: function ssi_restoreWindows(aWindow, aState, aOptions = {}) {
    if (aWindow && (!aWindow.__SSi || !this._windows[aWindow.__SSi])) {
      this.onLoad(aWindow);
    }

    let root;
    try {
      root = typeof aState == "string" ? JSON.parse(aState) : aState;
    } catch (ex) {
      this._log.debug(`restoreWindows failed to parse ${typeof aState} state`);
      this._log.error(ex);
      this._sendRestoreCompletedNotifications();
      return;
    }

    if (root._closedWindows) {
      this._closedWindows = root._closedWindows;
      for (let closedWindow of this._closedWindows) {
        closedWindow.closedId = this._nextClosedId++;
        if (closedWindow._closedTabs?.length) {
          this._resetClosedTabIds(
            closedWindow._closedTabs,
            closedWindow.closedId
          );
        }
      }
      this._log.debug(`Restored ${this._closedWindows.length} closed windows`);
      this._closedObjectsChanged = true;
    }

    this._log.debug(
      `restoreWindows will restore ${root.windows?.length} windows`
    );
    if (!root.windows || !root.windows.length) {
      this._sendRestoreCompletedNotifications();
      return;
    }

    let firstWindowData = root.windows.splice(0, 1);
    this._updateWindowRestoreState(aWindow, {
      windows: firstWindowData,
      options: aOptions,
    });

    this._openWindows(root).then(windows => {
      windows.unshift(aWindow);

      this._restoreWindowsInReversedZOrder(windows);
    });

  },

  restoreTabs(aWindow, aTabs, aTabData, aSelectTab) {
    var tabbrowser = aWindow.gBrowser;

    let numTabsToRestore = aTabs.length;
    let numTabsInWindow = tabbrowser.tabs.length;
    let tabsDataArray = this._windows[aWindow.__SSi].tabs;

    if (numTabsInWindow == numTabsToRestore) {
      tabsDataArray.length = 0;
    } else {
      tabsDataArray.splice(numTabsInWindow - numTabsToRestore);
    }

    if (numTabsInWindow < tabsDataArray.length) {
      tabsDataArray.length = numTabsInWindow;
    }

    this._ensureNoNullsInTabDataList(
      tabbrowser.tabs,
      tabsDataArray,
      numTabsInWindow - 1
    );

    if (aSelectTab > 0 && aSelectTab <= aTabs.length) {
      this._windows[aWindow.__SSi].selected = aSelectTab;
    }

    let selectedIndex = aTabs.indexOf(tabbrowser.selectedTab);
    if (selectedIndex > -1) {
      this.restoreTab(tabbrowser.selectedTab, aTabData[selectedIndex]);
    }

    for (let t = 0; t < aTabs.length; t++) {
      if (t != selectedIndex) {
        this.restoreTab(aTabs[t], aTabData[t]);
      }
    }
  },

  _ensureNoNullsInTabDataList(tabElements, tabDataList, changedTabPos) {
    let initialDataListLength = tabDataList.length;
    if (changedTabPos < initialDataListLength) {
      return;
    }
    while (tabDataList.length < changedTabPos) {
      let existingTabEl = tabElements[tabDataList.length];
      tabDataList.push({
        entries: [],
        lastAccessed: existingTabEl.lastAccessed,
      });
    }
    for (let i = 0; i < initialDataListLength; i++) {
      if (!tabDataList[i]) {
        let existingTabEl = tabElements[i];
        tabDataList[i] = {
          entries: [],
          lastAccessed: existingTabEl.lastAccessed,
        };
      }
    }
  },

  restoreTab(tab, tabData, options = {}) {
    let browser = tab.linkedBrowser;

    if (TAB_STATE_FOR_BROWSER.has(browser)) {
      this._log.warn("Must reset tab before calling restoreTab.");
      return;
    }

    let loadArguments = options.loadArguments;
    let window = tab.documentGlobal;
    let tabbrowser = window.gBrowser;
    let forceOnDemand = options.forceOnDemand;
    let isRemotenessUpdate = options.isRemotenessUpdate;

    let willRestoreImmediately =
      options.restoreImmediately ||
      tabbrowser.selectedBrowser == browser ||
      (tab.splitview && tab.splitview == tabbrowser.selectedTab.splitview);
    let isBrowserInserted = browser.isConnected;

    this._setWindowStateBusy(window);

    DirtyWindows.add(window);

    if (!tab.hasOwnProperty("_tPos")) {
      throw new Error(
        "Shouldn't be trying to restore a tab that has no position"
      );
    }
    this._windows[window.__SSi].tabs[tab._tPos] = tabData;


    if (tabData.lastAccessed) {
      tab.updateLastAccessed(tabData.lastAccessed);
    }

    if (!tabData.entries) {
      tabData.entries = [];
    }
    if (tabData.extData) {
      TAB_CUSTOM_VALUES.set(tab, Cu.cloneInto(tabData.extData, {}));
    } else {
      TAB_CUSTOM_VALUES.delete(tab);
    }

    delete tabData.closedAt;

    let activeIndex = (tabData.index || tabData.entries.length) - 1;
    activeIndex = Math.min(activeIndex, tabData.entries.length - 1);
    activeIndex = Math.max(activeIndex, 0);

    tabData.index = activeIndex + 1;

    tab.setAttribute("pending", "true");

    this._crashedBrowsers.delete(browser.permanentKey);

    if (
      options.restoreContentReason ==
      RESTORE_TAB_CONTENT_REASON.NAVIGATE_AND_RESTORE
    ) {
      delete tabData.userTypedValue;
      delete tabData.userTypedClear;
    }

    lazy.TabStateCache.update(browser.permanentKey, {
      history: { entries: [...tabData.entries], index: tabData.index },
      scroll: tabData.scroll || null,
      storage: tabData.storage || null,
      formdata: tabData.formdata || null,
      disallow: tabData.disallow || null,
      userContextId: tabData.userContextId || 0,

      image: tabData.image || "",
      searchMode: tabData.searchMode || null,
      userTypedValue: tabData.userTypedValue || "",
      userTypedClear: tabData.userTypedClear || 0,
    });

    if ("attributes" in tabData) {
      lazy.TabAttributes.set(tab, tabData.attributes);
    }

    if (isBrowserInserted) {
      let epoch = this.startNextEpoch(browser.permanentKey);

      TAB_STATE_FOR_BROWSER.set(browser, TAB_STATE_NEEDS_RESTORE);

      this._sendRestoreHistory(browser, {
        tabData,
        epoch,
        loadArguments,
        isRemotenessUpdate,
      });

      if (willRestoreImmediately) {
        this.restoreTabContent(tab, options);
      } else if (!forceOnDemand) {
        TabRestoreQueue.add(tab);
        if (TabRestoreQueue.willRestoreSoon(tab)) {
          if (activeIndex in tabData.entries) {
            let url = tabData.entries[activeIndex].url;
            let prepared = this.prepareConnectionToHost(tab, url);
            if (gDebuggingEnabled) {
              tab.__test_connection_prepared = prepared;
              tab.__test_connection_url = url;
            }
          }
        }
        this.restoreNextTab();
      }
    } else {
      let url = "about:blank";
      let title = "";

      if (activeIndex in tabData.entries) {
        url = tabData.entries[activeIndex].url;
        title = tabData.entries[activeIndex].title || url;
      }
      TAB_LAZY_STATES.set(tab, {
        url,
        title,
        userTypedValue: tabData.userTypedValue || "",
        userTypedClear: tabData.userTypedClear || 0,
      });
    }


    if (tabData.pinned) {
      tabbrowser.pinTab(tab);
    } else {
      tabbrowser.unpinTab(tab);
    }

    if (tabData.hidden) {
      tabbrowser.hideTab(tab);
    } else {
      tabbrowser.showTab(tab);
    }

    if (!!tabData.muted != browser.audioMuted) {
      tab.toggleMuteAudio(tabData.muteReason);
    }

    if (tab.hasAttribute("customizemode")) {
      window.gCustomizeMode.setTab(tab);
    }

    if (tabData.canonicalUrl) {
      tab.canonicalUrl = tabData.canonicalUrl;
    }

    this.updateTabLabelAndIcon(tab, tabData);

    this._setWindowStateReady(window);
  },

  restoreTabContent(aTab, aOptions = {}) {
    let loadArguments = aOptions.loadArguments;
    if (aTab.hasAttribute("customizemode") && !loadArguments) {
      return;
    }

    let browser = aTab.linkedBrowser;
    let window = aTab.documentGlobal;
    let tabData = lazy.TabState.clone(aTab, TAB_CUSTOM_VALUES.get(aTab));
    let activeIndex = tabData.index - 1;
    let activePageData = tabData.entries[activeIndex] || null;
    let uri = activePageData ? activePageData.url || null : null;

    this.markTabAsRestoring(aTab);

    this._sendRestoreTabContent(browser, {
      loadArguments,
      isRemotenessUpdate: aOptions.isRemotenessUpdate,
      reason:
        aOptions.restoreContentReason || RESTORE_TAB_CONTENT_REASON.SET_STATE,
    });

    if (
      aTab.selected &&
      !window.isBlankPageURL(uri) &&
      !aOptions.isRemotenessUpdate
    ) {
      browser.focus();
    }
  },

  markTabAsRestoring(aTab) {
    let browser = aTab.linkedBrowser;
    if (TAB_STATE_FOR_BROWSER.get(browser) != TAB_STATE_NEEDS_RESTORE) {
      throw new Error("Given tab is not pending.");
    }

    TabRestoreQueue.remove(aTab);

    this._tabsRestoringCount++;

    TAB_STATE_FOR_BROWSER.set(browser, TAB_STATE_RESTORING);
    aTab.removeAttribute("pending");
    aTab.removeAttribute("discarded");
  },

  restoreNextTab: function ssi_restoreNextTab() {
    if (lazy.RunState.isQuitting) {
      return;
    }

    if (this._tabsRestoringCount >= MAX_CONCURRENT_TAB_RESTORES) {
      return;
    }

    let tab = TabRestoreQueue.shift();
    if (tab) {
      this.restoreTabContent(tab);
    }
  },

  restoreWindowFeatures: function ssi_restoreWindowFeatures(
    aWindow,
    aWinData,
    aOptions = {}
  ) {
    var hidden = aWinData.hidden ? aWinData.hidden.split(",") : [];
    var isTaskbarTab =
      aWindow.document.documentElement.hasAttribute("taskbartab");

    if (!isTaskbarTab) {
      WINDOW_HIDEABLE_FEATURES.forEach(function (aItem) {
        aWindow[aItem].visible = !hidden.includes(aItem);
      });
    }

    if (aWinData.isPopup) {
      this._windows[aWindow.__SSi].isPopup = true;
      if (aWindow.gURLBar) {
        aWindow.gURLBar.readOnly = true;
      }
    } else {
      delete this._windows[aWindow.__SSi].isPopup;
      if (aWindow.gURLBar && !isTaskbarTab) {
        aWindow.gURLBar.readOnly = false;
      }
    }

    let promiseParts = Promise.withResolvers();
    aWindow.setTimeout(() => {
      this.restoreDimensions(
        aWindow,
        +(aWinData.width || 0),
        +(aWinData.height || 0),
        "screenX" in aWinData ? +aWinData.screenX : NaN,
        "screenY" in aWinData ? +aWinData.screenY : NaN,
        aWinData.sizemode || "",
        aWinData.sizemodeBeforeMinimized || ""
      );
      promiseParts.resolve(aWindow);
    }, 0);
    return promiseParts.promise;
  },

  restoreSidebar(aWindow, aSidebar, isPopup) {
    if (!aSidebar || isPopup || !aWindow.SidebarController) {
      return;
    }
    aWindow.SidebarController.markSessionRestoreStateReceived();
    aWindow.SidebarController.updateUIState(aSidebar);
  },

  restoreDimensions: function ssi_restoreDimensions(
    aWindow,
    aWidth,
    aHeight,
    aLeft,
    aTop,
    aSizeMode,
    aSizeModeBeforeMinimized
  ) {
    var win = aWindow;
    var _this = this;
    function win_(aName) {
      return _this._getWindowDimension(win, aName);
    }

    const dwu = win.windowUtils;
    let screen = lazy.gScreenManager.screenForRect(
      aLeft,
      aTop,
      aWidth,
      aHeight
    );
    if (screen) {
      let screenLeft = {},
        screenTop = {},
        screenWidth = {},
        screenHeight = {};
      screen.GetAvailRectDisplayPix(
        screenLeft,
        screenTop,
        screenWidth,
        screenHeight
      );

      screenLeft = screenLeft.value;
      screenTop = screenTop.value;
      screenWidth = screenWidth.value;
      screenHeight = screenHeight.value;

      let screenBottom = screenTop + screenHeight;
      let screenRight = screenLeft + screenWidth;

      let cssToDesktopScale =
        screen.defaultCSSScaleFactor / screen.contentsScaleFactor;

      let winSlopX = win.screenEdgeSlopX * cssToDesktopScale;
      let winSlopY = win.screenEdgeSlopY * cssToDesktopScale;

      let minSlop = MIN_SCREEN_EDGE_SLOP * cssToDesktopScale;
      let slopX = Math.max(minSlop, winSlopX);
      let slopY = Math.max(minSlop, winSlopY);

      if (aLeft < screenLeft - slopX) {
        aLeft = screenLeft - winSlopX;
      }
      let right = aLeft + aWidth * cssToDesktopScale;
      if (right > screenRight + slopX) {
        right = screenRight + winSlopX;
        if (aLeft > screenLeft) {
          aLeft = Math.max(
            right - aWidth * cssToDesktopScale,
            screenLeft - winSlopX
          );
        }
      }
      aWidth = (right - aLeft) / cssToDesktopScale;

      if (aTop < screenTop - slopY) {
        aTop = screenTop - winSlopY;
      }
      let bottom = aTop + aHeight * cssToDesktopScale;
      if (bottom > screenBottom + slopY) {
        bottom = screenBottom + winSlopY;
        if (aTop > screenTop) {
          aTop = Math.max(
            bottom - aHeight * cssToDesktopScale,
            screenTop - winSlopY
          );
        }
      }
      aHeight = (bottom - aTop) / cssToDesktopScale;
    }

    dwu.suppressAnimation(true);

    try {
      if (
        !isNaN(aLeft) &&
        !isNaN(aTop) &&
        (aLeft != win_("screenX") || aTop != win_("screenY"))
      ) {
        let desktopToCssScale =
          aWindow.desktopToDeviceScale / aWindow.devicePixelRatio;
        aWindow.moveTo(aLeft * desktopToCssScale, aTop * desktopToCssScale);
      }
      if (
        aWidth &&
        aHeight &&
        (aWidth != win_("width") || aHeight != win_("height")) &&
        !ChromeUtils.shouldResistFingerprinting("RoundWindowSize", null)
      ) {
        if (aSizeMode != "maximized" || win_("sizemode") != "maximized") {
          aWindow.resizeTo(aWidth, aHeight);
        }
      }
      this._windows[aWindow.__SSi].sizemodeBeforeMinimized =
        aSizeModeBeforeMinimized;
      if (
        aSizeMode &&
        win_("sizemode") != aSizeMode &&
        !ChromeUtils.shouldResistFingerprinting("RoundWindowSize", null)
      ) {
        switch (aSizeMode) {
          case "maximized":
            aWindow.maximize();
            break;
          case "minimized":
            if (aSizeModeBeforeMinimized == "maximized") {
              aWindow.maximize();
            }
            aWindow.minimize();
            break;
          case "normal":
            aWindow.restore();
            break;
        }
      }
      if (this.windowToFocus) {
        this.windowToFocus.focus();
      }
    } finally {
      dwu.suppressAnimation(false);
    }
  },


  saveStateDelayed(aWindow = null) {
    if (aWindow) {
      DirtyWindows.add(aWindow);
    }

    lazy.SessionSaver.runDelayed();
  },


  _removeClosedWindow(index) {
    for (let closedTab of this._closedWindows[index]._closedTabs) {
      this._removeClosedAction(
        this._LAST_ACTION_CLOSED_TAB,
        closedTab.closedId
      );
    }
    this._removeClosedAction(
      this._LAST_ACTION_CLOSED_WINDOW,
      this._closedWindows[index].closedId
    );
    let windows = this._closedWindows.splice(index, 1);
    this._closedObjectsChanged = true;
    return windows;
  },

  _notifyOfClosedObjectsChange() {
    if (!this._closedObjectsChanged) {
      return;
    }
    this._closedObjectsChanged = false;
    lazy.setTimeout(() => {
      Services.obs.notifyObservers(null, NOTIFY_CLOSED_OBJECTS_CHANGED);
    }, 0);
  },

  _notifyOfSavedTabGroupsChange() {
    lazy.setTimeout(() => {
      Services.obs.notifyObservers(null, NOTIFY_SAVED_TAB_GROUPS_CHANGED);
    }, 0);
  },

  _updateSessionStartTime: function ssi_updateSessionStartTime(state) {
    if (state.session && state.session.startTime) {
      this._sessionStartTime = state.session.startTime;
    }
  },

  _browserWindows: {
    *[Symbol.iterator]() {
      for (let window of lazy.BrowserWindowTracker.orderedWindows) {
        if (window.__SSi && !window.closed) {
          yield window;
        }
      }
    },
  },

  _orderedBrowserWindows: {
    *[Symbol.iterator]() {
      let windows = lazy.BrowserWindowTracker.orderedWindows;
      windows.sort((a, b) => {
        if (
          a.windowState == a.STATE_MINIMIZED &&
          b.windowState != b.STATE_MINIMIZED
        ) {
          return 1;
        }
        if (
          a.windowState != a.STATE_MINIMIZED &&
          b.windowState == b.STATE_MINIMIZED
        ) {
          return -1;
        }
        return 0;
      });
      for (let window of windows) {
        if (window.__SSi && !window.closed) {
          yield window;
        }
      }
    },
  },

  _getTopWindow: function ssi_getTopWindow(isPrivate) {
    const options = { allowPopups: true };
    if (typeof isPrivate !== "undefined") {
      options.private = isPrivate;
    }
    return lazy.BrowserWindowTracker.getTopWindow(options);
  },

  _handleClosedWindows: function ssi_handleClosedWindows() {
    let promises = [];
    for (let window of Services.wm.getEnumerator("navigator:browser")) {
      if (window.closed) {
        promises.push(this.onClose(window));
      }
    }
    return Promise.all(promises);
  },

  _updateWindowRestoreState(window, state) {
    if ("zIndex" in state.windows[0]) {
      WINDOW_RESTORE_ZINDICES.set(window, state.windows[0].zIndex);
    }
    do {
      var ID = "window" + Math.random();
    } while (ID in this._statesToRestore);
    WINDOW_RESTORE_IDS.set(window, ID);
    this._statesToRestore[ID] = state;
  },

  _openWindowWithState: function ssi_openWindowWithState(aState) {
    let argString;
    let features;
    let winState = aState.windows[0];
    if (winState.chromeFlags) {
      features = ["chrome", "suppressanimation"];
      let chromeFlags = winState.chromeFlags;
      const allFlags = Ci.nsIWebBrowserChrome.CHROME_ALL;
      const hasAll = (chromeFlags & allFlags) == allFlags;
      if (hasAll) {
        features.push("all");
      }
      for (let [flag, onValue, offValue] of CHROME_FLAGS_MAP) {
        if (hasAll && allFlags & flag) {
          continue;
        }
        let value = chromeFlags & flag ? onValue : offValue;
        if (value) {
          features.push(value);
        }
      }
    } else {
      features = ["chrome", "dialog=no", "suppressanimation"];
      let hidden = winState.hidden?.split(",") || [];
      if (!hidden.length) {
        features.push("all");
      } else {
        features.push("resizable");
        WINDOW_HIDEABLE_FEATURES.forEach(aFeature => {
          if (!hidden.includes(aFeature)) {
            features.push(WINDOW_OPEN_FEATURES_MAP[aFeature] || aFeature);
          }
        });
      }
    }
    WINDOW_ATTRIBUTES.forEach(aFeature => {
      if (aFeature in winState && !isNaN(winState[aFeature])) {
        features.push(aFeature + "=" + winState[aFeature]);
      }
    });

    if (winState.isPrivate) {
      features.push("private");
    }

    if (!argString) {
      argString = Cc["@mozilla.org/supports-string;1"].createInstance(
        Ci.nsISupportsString
      );
      argString.data = "";
    }

    this._log.debug(
      `Opening window:${winState.closedId} with features: ${features.join(
        ","
      )}, argString: ${argString}.`
    );
    var window = Services.ww.openWindow(
      null,
      AppConstants.BROWSER_CHROME_URL,
      "_blank",
      features.join(","),
      argString
    );

    this._updateWindowRestoreState(window, aState);
    WINDOW_SHOWING_PROMISES.set(window, Promise.withResolvers());

    return window;
  },

  _isCmdLineEmpty: function ssi_isCmdLineEmpty(aWindow, aState) {
    var pinnedOnly =
      aState.windows &&
      aState.windows.every(win => win.tabs.every(tab => tab.pinned));

    let hasFirstArgument = aWindow.arguments && aWindow.arguments[0];
    if (!pinnedOnly) {
      let defaultArgs = Cc["@mozilla.org/browser/clh;1"].getService(
        Ci.nsIBrowserHandler
      ).defaultArgs;
      if (
        aWindow.arguments &&
        aWindow.arguments[0] &&
        aWindow.arguments[0] == defaultArgs
      ) {
        hasFirstArgument = false;
      }
    }

    return !hasFirstArgument;
  },

  _getWindowDimension: function ssi_getWindowDimension(aWindow, aAttribute) {
    if (aAttribute == "sizemode") {
      switch (aWindow.windowState) {
        case aWindow.STATE_FULLSCREEN:
        case aWindow.STATE_MAXIMIZED:
          return "maximized";
        case aWindow.STATE_MINIMIZED:
          return "minimized";
        default:
          return "normal";
      }
    }

    if (aWindow.windowState != aWindow.STATE_NORMAL) {
      let docElem = aWindow.document.documentElement;
      let attr = parseInt(docElem.getAttribute(aAttribute), 10);
      if (attr) {
        if (aAttribute != "width" && aAttribute != "height") {
          return attr;
        }
        let appWin = aWindow.docShell.treeOwner
          .QueryInterface(Ci.nsIInterfaceRequestor)
          .getInterface(Ci.nsIAppWindow);
        let diff =
          aAttribute == "width"
            ? appWin.outerToInnerWidthDifferenceInCSSPixels
            : appWin.outerToInnerHeightDifferenceInCSSPixels;
        return attr + diff;
      }
    }

    switch (aAttribute) {
      case "width":
        return aWindow.outerWidth;
      case "height":
        return aWindow.outerHeight;
      case "screenX":
      case "screenY":
        return (
          (aWindow[aAttribute] * aWindow.devicePixelRatio) /
          aWindow.desktopToDeviceScale
        );
      default:
        return aAttribute in aWindow ? aWindow[aAttribute] : "";
    }
  },

  _needsRestorePage: function ssi_needsRestorePage(aState, aRecentCrashes) {
    const SIX_HOURS_IN_MS = 6 * 60 * 60 * 1000;

    let winData = aState.windows || null;
    if (!winData || !winData.length) {
      return false;
    }

    if (
      this._hasSingleTabWithURL(winData, "about:sessionrestore") ||
      this._hasSingleTabWithURL(winData, "about:welcomeback")
    ) {
      return false;
    }

    if (Services.appinfo.inSafeMode) {
      return true;
    }

    let max_resumed_crashes = this._prefBranch.getIntPref(
      "sessionstore.max_resumed_crashes"
    );
    let sessionAge =
      aState.session &&
      aState.session.lastUpdate &&
      Date.now() - aState.session.lastUpdate;

    let decision =
      max_resumed_crashes != -1 &&
      (aRecentCrashes > max_resumed_crashes ||
        (sessionAge && sessionAge >= SIX_HOURS_IN_MS));
    if (decision) {
      let key;
      if (aRecentCrashes > max_resumed_crashes) {
        if (sessionAge && sessionAge >= SIX_HOURS_IN_MS) {
          key = "shown_many_crashes_old_session";
        } else {
          key = "shown_many_crashes";
        }
      } else {
        key = "shown_old_session";
      }
    }
    return decision;
  },

  _hasSingleTabWithURL(aWinData, aURL) {
    if (
      aWinData &&
      aWinData.length == 1 &&
      aWinData[0].tabs &&
      aWinData[0].tabs.length == 1 &&
      aWinData[0].tabs[0].entries &&
      aWinData[0].tabs[0].entries.length == 1
    ) {
      return aURL == aWinData[0].tabs[0].entries[0].url;
    }
    return false;
  },

  _shouldSaveTabState: function ssi_shouldSaveTabState(aTabState) {
    const entryUrl = aTabState.entries[0]?.url;
    return (
      entryUrl &&
      !(
        aTabState.entries.length == 1 &&
        (entryUrl == "about:blank" ||
          (entryUrl == "about:home" && !aTabState.splitViewId) ||
          (entryUrl == "about:newtab" && !aTabState.splitViewId) ||
          entryUrl == "about:privatebrowsing") &&
        !aTabState.userTypedValue
      )
    );
  },

  shouldSaveTabsToGroup: function ssi_shouldSaveTabsToGroup(tabs) {
    if (!tabs) {
      return false;
    }
    for (let tab of tabs) {
      let tabState = lazy.TabState.collect(tab);
      if (this._shouldSaveTabState(tabState)) {
        return true;
      }
    }
    return false;
  },

  _shouldSaveTab: function ssi_shouldSaveTab(aTabState) {
    return (
      aTabState.userTypedValue ||
      (aTabState.attributes && aTabState.attributes.customizemode == "true") ||
      (aTabState.entries.length &&
        aTabState.entries[0].url != "about:privatebrowsing")
    );
  },

  _prepDataForDeferredRestore: function ssi_prepDataForDeferredRestore(
    startupState
  ) {
    let state = Cu.cloneInto(startupState, {});
    let hasPinnedTabs = false;
    let defaultState = {
      windows: [],
      selectedWindow: 1,
      savedGroups: state.savedGroups || [],
    };
    state.selectedWindow = state.selectedWindow || 1;

    for (let group of defaultState.savedGroups) {
      delete group.removeAfterRestore;
    }

    for (let wIndex = 0; wIndex < state.windows.length; ) {
      let window = state.windows[wIndex];
      window.selected = window.selected || 1;
      let newWindowState = {
        tabs: [],
      };
      if (PERSIST_SESSIONS) {
        newWindowState._closedTabs = Cu.cloneInto(window._closedTabs, {});
        newWindowState.closedGroups = Cu.cloneInto(window.closedGroups, {});
      }

      if (window.sidebar) {
        newWindowState.sidebar = window.sidebar;
      }

      let groupsToSave = new Map();
      for (let tIndex = 0; tIndex < window.tabs.length; ) {
        if (window.tabs[tIndex].pinned) {
          if (tIndex + 1 < window.selected) {
            window.selected -= 1;
          } else if (tIndex + 1 == window.selected) {
            newWindowState.selected = newWindowState.tabs.length + 1;
          }

          newWindowState.tabs = newWindowState.tabs.concat(
            window.tabs.splice(tIndex, 1)
          );
          continue;
        } else if (window.tabs[tIndex].groupId) {
          let groupStateToSave = window.groups.find(
            groupState => groupState.id == window.tabs[tIndex].groupId
          );
          let groupToSave = groupsToSave.get(groupStateToSave.id);
          if (!groupToSave) {
            groupToSave =
              lazy.TabGroupState.savedInClosedWindow(groupStateToSave);
            groupToSave.removeAfterRestore = true;
            groupsToSave.set(groupStateToSave.id, groupToSave);
          }
          let tabToAdd = window.tabs[tIndex];
          groupToSave.tabs.push(this._formatTabStateForSavedGroup(tabToAdd));
        } else if (!window.tabs[tIndex].hidden && PERSIST_SESSIONS) {

          let tabState = window.tabs[tIndex];

          let activeIndex = tabState.index;
          activeIndex = Math.min(activeIndex, tabState.entries.length - 1);
          activeIndex = Math.max(activeIndex, 0);

          if (activeIndex in tabState.entries) {
            let title =
              tabState.entries[activeIndex].title ||
              tabState.entries[activeIndex].url;

            let tabData = {
              state: tabState,
              title,
              image: tabState.image,
              pos: tIndex,
              closedAt: Date.now(),
              closedInGroup: false,
              removeAfterRestore: true,
            };

            if (this._shouldSaveTabState(tabState)) {
              let closedTabsList = newWindowState._closedTabs;
              this.saveClosedTabData(window, closedTabsList, tabData, false);
            }
          }
        }
        tIndex++;
      }

      groupsToSave.forEach(groupState => {
        const alreadySavedGroup = defaultState.savedGroups.find(
          existingGroup => existingGroup.id == groupState.id
        );
        if (alreadySavedGroup) {
          alreadySavedGroup.removeAfterRestore = true;
        } else {
          defaultState.savedGroups.push(groupState);
        }
      });

      hasPinnedTabs ||= !!newWindowState.tabs.length;

      if (newWindowState.tabs.length) {
        WINDOW_ATTRIBUTES.forEach(function (attr) {
          if (attr in window) {
            newWindowState[attr] = window[attr];
            delete window[attr];
          }
        });

        window.__lastSessionWindowID = newWindowState.__lastSessionWindowID =
          "" + Date.now() + Math.random();
      }

      if (
        newWindowState.tabs.length ||
        (PERSIST_SESSIONS &&
          (newWindowState._closedTabs.length ||
            newWindowState.closedGroups.length))
      ) {
        defaultState.windows.push(newWindowState);
        if (!window.tabs.length) {
          if (wIndex + 1 <= state.selectedWindow) {
            state.selectedWindow -= 1;
          } else if (wIndex + 1 == state.selectedWindow) {
            defaultState.selectedIndex = defaultState.windows.length + 1;
          }

          state.windows.splice(wIndex, 1);
          continue;
        }
      }
      wIndex++;
    }

    if (hasPinnedTabs) {
      defaultState.cookies = state.cookies;
      delete state.cookies;
    }
    return [defaultState, state];
  },

  _sendRestoreCompletedNotifications:
    function ssi_sendRestoreCompletedNotifications() {
      if (this._restoreCount > 1) {
        this._restoreCount--;
        this._log.warn(
          `waiting on ${this._restoreCount} windows to be restored before sending restore complete notifications.`
        );
        return;
      }

      if (this._restoreCount == -1) {
        return;
      }

      if (!this._browserSetState) {
        Services.obs.notifyObservers(null, NOTIFY_WINDOWS_RESTORED);
        this._log.debug(`All ${this._restoreCount} windows restored`);
        this._deferredAllWindowsRestored.resolve();
      } else {
        Services.obs.notifyObservers(null, NOTIFY_BROWSER_STATE_RESTORED);
      }

      let anyWindowNotCloaked = this._browserWindows[Symbol.iterator]().some(
        window => !window.isCloaked
      );
      if (!anyWindowNotCloaked) {
        lazy.BrowserWindowTracker.openWindow();
      }

      this._browserSetState = false;
      this._restoreCount = -1;
    },

  _setWindowStateBusyValue: function ssi_changeWindowStateBusyValue(
    aWindow,
    aValue
  ) {
    this._windows[aWindow.__SSi].busy = aValue;

    if (!this._isWindowLoaded(aWindow)) {
      let stateToRestore =
        this._statesToRestore[WINDOW_RESTORE_IDS.get(aWindow)].windows[0];
      stateToRestore.busy = aValue;
    }
  },

  _setWindowStateReady: function ssi_setWindowStateReady(aWindow) {
    let newCount = (this._windowBusyStates.get(aWindow) || 0) - 1;
    if (newCount < 0) {
      throw new Error("Invalid window busy state (less than zero).");
    }
    this._windowBusyStates.set(aWindow, newCount);

    if (newCount == 0) {
      this._setWindowStateBusyValue(aWindow, false);
      this._sendWindowStateReadyEvent(aWindow);
    }
  },

  _setWindowStateBusy: function ssi_setWindowStateBusy(aWindow) {
    let newCount = (this._windowBusyStates.get(aWindow) || 0) + 1;
    this._windowBusyStates.set(aWindow, newCount);

    if (newCount == 1) {
      this._setWindowStateBusyValue(aWindow, true);
      this._sendWindowStateBusyEvent(aWindow);
    }
  },

  _sendWindowStateReadyEvent: function ssi_sendWindowStateReadyEvent(aWindow) {
    let event = aWindow.document.createEvent("Events");
    event.initEvent("SSWindowStateReady", true, false);
    aWindow.dispatchEvent(event);
  },

  _sendWindowStateBusyEvent: function ssi_sendWindowStateBusyEvent(aWindow) {
    let event = aWindow.document.createEvent("Events");
    event.initEvent("SSWindowStateBusy", true, false);
    aWindow.dispatchEvent(event);
  },

  _sendWindowRestoringNotification(aWindow) {
    let event = aWindow.document.createEvent("Events");
    event.initEvent("SSWindowRestoring", true, false);
    aWindow.dispatchEvent(event);
  },

  _sendWindowRestoredNotification(aWindow) {
    let event = aWindow.document.createEvent("Events");
    event.initEvent("SSWindowRestored", true, false);
    aWindow.dispatchEvent(event);
  },

  _sendTabRestoredNotification(aTab, aIsRemotenessUpdate) {
    let event = aTab.ownerDocument.createEvent("CustomEvent");
    event.initCustomEvent("SSTabRestored", true, false, {
      isRemotenessUpdate: aIsRemotenessUpdate,
    });
    aTab.dispatchEvent(event);
  },

  _isWindowLoaded: function ssi_isWindowLoaded(aWindow) {
    return !WINDOW_RESTORE_IDS.has(aWindow);
  },

  _capClosedWindows: function ssi_capClosedWindows() {
    if (this._closedWindows.length <= this._max_windows_undo) {
      return;
    }
    let spliceTo = this._max_windows_undo;
    if (AppConstants.platform != "macosx") {
      let normalWindowIndex = 0;
      while (
        normalWindowIndex < this._closedWindows.length &&
        !!this._closedWindows[normalWindowIndex].isPopup
      ) {
        normalWindowIndex++;
      }
      if (normalWindowIndex >= this._max_windows_undo) {
        spliceTo = normalWindowIndex + 1;
      }
    }
    if (spliceTo < this._closedWindows.length) {
      this._closedWindows.splice(spliceTo, this._closedWindows.length);
      this._closedObjectsChanged = true;
    }
  },

  _clearRestoringWindows: function ssi_clearRestoringWindows() {
    for (let i = 0; i < this._closedWindows.length; i++) {
      delete this._closedWindows[i]._shouldRestore;
    }
  },

  _resetRestoringState: function ssi_initRestoringState() {
    TabRestoreQueue.reset();
    this._tabsRestoringCount = 0;
  },

  _resetLocalTabRestoringState(aTab) {
    let browser = aTab.linkedBrowser;

    let previousState = TAB_STATE_FOR_BROWSER.get(browser);

    if (!previousState) {
      console.error("Given tab is not restoring.");
      return;
    }

    TAB_STATE_FOR_BROWSER.delete(browser);

    this._restoreListeners.get(browser.permanentKey)?.unregister();
    browser.browsingContext.clearRestoreState();

    aTab.removeAttribute("pending");
    aTab.removeAttribute("discarded");

    if (previousState == TAB_STATE_RESTORING) {
      if (this._tabsRestoringCount) {
        this._tabsRestoringCount--;
      }
    } else if (previousState == TAB_STATE_NEEDS_RESTORE) {
      TabRestoreQueue.remove(aTab);
    }
  },

  _resetTabRestoringState(tab) {
    let browser = tab.linkedBrowser;

    if (!TAB_STATE_FOR_BROWSER.has(browser)) {
      console.error("Given tab is not restoring.");
      return;
    }

    this._resetLocalTabRestoringState(tab);
  },

  startNextEpoch(permanentKey) {
    let next = this.getCurrentEpoch(permanentKey) + 1;
    this._browserEpochs.set(permanentKey, next);
    return next;
  },

  getCurrentEpoch(permanentKey) {
    return this._browserEpochs.get(permanentKey) || 0;
  },

  isCurrentEpoch(permanentKey, epoch) {
    return this.getCurrentEpoch(permanentKey) == epoch;
  },

  resetEpoch(permanentKey, frameLoader = null) {
    this._browserEpochs.delete(permanentKey);
    if (frameLoader) {
      frameLoader.requestEpochUpdate(0);
    }
  },

  looseTimer(delay) {
    let DELAY_BEAT = 1000;
    let timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
    let beats = Math.ceil(delay / DELAY_BEAT);
    let deferred = Promise.withResolvers();
    timer.initWithCallback(
      function () {
        if (beats <= 0) {
          this._log.debug(`looseTimer of ${delay} timed out`);
          deferred.resolve();
        }
        --beats;
      },
      DELAY_BEAT,
      Ci.nsITimer.TYPE_REPEATING_PRECISE_CAN_SKIP
    );
    deferred.promise.then(
      () => timer.cancel(),
      () => timer.cancel()
    );
    return deferred;
  },

  _waitForStateStop(browser, expectedURL = null) {
    const deferred = Promise.withResolvers();

    const listener = {
      unregister(reject = true) {
        if (reject) {
          deferred.reject();
        }

        SessionStoreInternal._restoreListeners.delete(browser.permanentKey);

        try {
          browser.removeProgressListener(
            this,
            Ci.nsIWebProgress.NOTIFY_STATE_WINDOW
          );
        } catch {} 
      },

      onStateChange(webProgress, request, stateFlags) {
        if (
          webProgress.isTopLevel &&
          stateFlags & Ci.nsIWebProgressListener.STATE_IS_WINDOW &&
          stateFlags & Ci.nsIWebProgressListener.STATE_STOP
        ) {
          let aboutBlankOK = !expectedURL || expectedURL === "about:blank";
          let url = request.QueryInterface(Ci.nsIChannel).originalURI.spec;
          if (url !== "about:blank" || aboutBlankOK) {
            this.unregister(false);
            deferred.resolve();
          }
        }
      },

      QueryInterface: ChromeUtils.generateQI([
        "nsIWebProgressListener",
        "nsISupportsWeakReference",
      ]),
    };

    this._restoreListeners.get(browser.permanentKey)?.unregister();
    this._restoreListeners.set(browser.permanentKey, listener);

    browser.addProgressListener(
      listener,
      Ci.nsIWebProgress.NOTIFY_STATE_WINDOW
    );

    return deferred.promise;
  },

  _listenForNavigations(browser, callbacks) {
    const listener = {
      unregister() {
        browser.browsingContext?.sessionHistory?.removeSHistoryListener(this);

        try {
          browser.removeProgressListener(
            this,
            Ci.nsIWebProgress.NOTIFY_STATE_WINDOW
          );
        } catch {} 

        SessionStoreInternal._restoreListeners.delete(browser.permanentKey);
      },

      OnHistoryReload() {
        this.unregister();
        return callbacks.onHistoryReload();
      },

      OnHistoryNewEntry() {},
      OnHistoryGotoIndex() {},
      OnHistoryPurge() {},
      OnHistoryReplaceEntry() {},

      onStateChange(webProgress, request, stateFlags) {
        if (
          webProgress.isTopLevel &&
          stateFlags & Ci.nsIWebProgressListener.STATE_IS_WINDOW &&
          stateFlags & Ci.nsIWebProgressListener.STATE_START
        ) {
          this.unregister();
          callbacks.onStartRequest();
        }
      },

      QueryInterface: ChromeUtils.generateQI([
        "nsISHistoryListener",
        "nsIWebProgressListener",
        "nsISupportsWeakReference",
      ]),
    };

    this._restoreListeners.get(browser.permanentKey)?.unregister();
    this._restoreListeners.set(browser.permanentKey, listener);

    browser.browsingContext?.sessionHistory?.addSHistoryListener(listener);

    browser.addProgressListener(
      listener,
      Ci.nsIWebProgress.NOTIFY_STATE_WINDOW
    );
  },

  _restoreHistory(browser, data) {
    this._tabStateToRestore.set(browser.permanentKey, data);

    browser.stop();

    lazy.SessionHistory.restoreFromParent(
      browser.browsingContext.sessionHistory,
      data.tabData
    );

    let url = data.tabData?.entries[data.tabData.index - 1]?.url;
    let disallow = data.tabData?.disallow;

    let promise = SessionStoreUtils.restoreDocShellState(
      browser.browsingContext,
      url,
      disallow
    );
    this._tabStateRestorePromises.set(browser.permanentKey, promise);

    const onResolve = () => {
      if (TAB_STATE_FOR_BROWSER.get(browser) !== TAB_STATE_RESTORING) {
        this._listenForNavigations(browser, {
          onHistoryReload: () => {
            this._restoreTabContent(browser);
            return false;
          },

          onStartRequest: () => {
            this._tabStateToRestore.delete(browser.permanentKey);
            this._restoreTabContent(browser);
          },
        });
      }

      this._tabStateRestorePromises.delete(browser.permanentKey);

      this._restoreHistoryComplete(browser);
    };

    promise.then(onResolve).catch(() => {});
  },

  _restoreTabEntry(browser, tabData) {
    let haveUserTypedValue = tabData.userTypedValue && tabData.userTypedClear;
    if (!haveUserTypedValue && tabData.entries.length) {
      return SessionStoreUtils.initializeRestore(
        browser.browsingContext,
        lazy.SessionStoreHelper.buildRestoreData(
          tabData.formdata,
          tabData.scroll
        )
      );
    }
    let triggeringPrincipal =
      Services.scriptSecurityManager.getSystemPrincipal();
    if (!haveUserTypedValue) {
      let blankPromise = this._waitForStateStop(browser, "about:blank");
      browser.browsingContext.loadURI(lazy.blankURI, {
        triggeringPrincipal,
        loadFlags: Ci.nsIWebNavigation.LOAD_FLAGS_BYPASS_HISTORY,
      });
      return blankPromise;
    }

    let loadPromise = this._waitForStateStop(browser, tabData.userTypedValue);
    browser.browsingContext.fixupAndLoadURIString(tabData.userTypedValue, {
      loadFlags: Ci.nsIWebNavigation.LOAD_FLAGS_ALLOW_THIRD_PARTY_FIXUP,
      triggeringPrincipal,
    });

    return loadPromise;
  },

  _restoreTabContent(browser, options = {}) {
    this._restoreListeners.get(browser.permanentKey)?.unregister();

    this._restoreTabContentStarted(browser, options);

    let state = this._tabStateToRestore.get(browser.permanentKey);
    this._tabStateToRestore.delete(browser.permanentKey);

    let promises = [this._tabStateRestorePromises.get(browser.permanentKey)];

    if (state) {
      promises.push(this._restoreTabEntry(browser, state.tabData));
    } else {
      promises.push(this._waitForStateStop(browser));
    }

    Promise.allSettled(promises).then(() => {
      this._restoreTabContentComplete(browser, options);
    });
  },

  _sendRestoreTabContent(browser, options) {
    this._restoreTabContent(browser, options);
  },

  _restoreHistoryComplete(browser) {
    let win = browser.documentGlobal;
    let tab = win?.gBrowser.getTabForBrowser(browser);
    if (!tab) {
      return;
    }

    let tabData = lazy.TabState.collect(tab, TAB_CUSTOM_VALUES.get(tab));

    this.updateTabLabelAndIcon(tab, tabData);

    let event = win.document.createEvent("Events");
    event.initEvent("SSTabRestoring", true, false);
    tab.dispatchEvent(event);
  },

  _restoreTabContentStarted(browser, data) {
    let win = browser.documentGlobal;
    let tab = win?.gBrowser.getTabForBrowser(browser);
    if (!tab) {
      return;
    }

    let initiatedBySessionStore =
      TAB_STATE_FOR_BROWSER.get(browser) != TAB_STATE_NEEDS_RESTORE;
    let isNavigateAndRestore =
      data.reason == RESTORE_TAB_CONTENT_REASON.NAVIGATE_AND_RESTORE;

    let cacheState = lazy.TabStateCache.get(browser.permanentKey);
    if (cacheState.searchMode) {
      if (!initiatedBySessionStore || isNavigateAndRestore) {
        lazy.TabStateCache.update(browser.permanentKey, {
          searchMode: null,
          userTypedValue: null,
        });
      }
      return;
    }

    if (!initiatedBySessionStore) {
      this.markTabAsRestoring(tab);
    } else if (!isNavigateAndRestore) {
      let tabData = lazy.TabState.collect(tab, TAB_CUSTOM_VALUES.get(tab));
      if (
        tabData.userTypedValue &&
        !tabData.userTypedClear &&
        !browser.userTypedValue
      ) {
        browser.userTypedValue = tabData.userTypedValue;
        if (tab.selected) {
          win.gURLBar.setURI();
        }
      }

      lazy.TabStateCache.update(browser.permanentKey, {
        userTypedValue: null,
        userTypedClear: null,
      });
    }
  },

  _restoreTabContentComplete(browser, data) {
    let win = browser.documentGlobal;
    let tab = win?.gBrowser.getTabForBrowser(browser);
    if (!tab) {
      return;
    }
    let cacheState = lazy.TabStateCache.get(browser.permanentKey);
    if (cacheState.searchMode) {
      win.gURLBar.setSearchMode(cacheState.searchMode, browser);
      browser.userTypedValue = cacheState.userTypedValue;
      if (tab.selected) {
        win.gURLBar.setURI();
      }
      lazy.TabStateCache.update(browser.permanentKey, {
        searchMode: null,
        userTypedValue: null,
      });
    }

    if (gDebuggingEnabled) {
      Services.obs.notifyObservers(browser, NOTIFY_TAB_RESTORED);
    }

    SessionStoreInternal._resetLocalTabRestoringState(tab);
    SessionStoreInternal.restoreNextTab();

    this._sendTabRestoredNotification(tab, data.isRemotenessUpdate);

    Services.obs.notifyObservers(null, "sessionstore-one-or-no-tab-restored");
  },

  _sendRestoreHistory(browser, options) {
    if (options.tabData.storage) {
      SessionStoreUtils.restoreSessionStorageFromParent(
        browser.browsingContext,
        options.tabData.storage
      );
      delete options.tabData.storage;
    }

    this._restoreHistory(browser, options);

    if (browser && browser.frameLoader) {
      browser.frameLoader.requestEpochUpdate(options.epoch);
    }
  },

  addSavedTabGroup(tabGroup) {
    if (PrivateBrowsingUtils.isWindowPrivate(tabGroup.documentGlobal)) {
      throw new Error("Refusing to save tab group from private window");
    }

    let tabGroupState = lazy.TabGroupState.savedInOpenWindow(
      tabGroup,
      tabGroup.documentGlobal.__SSi
    );
    tabGroupState.tabs = this._collectClosedTabsForTabGroup(
      tabGroup.tabs,
      tabGroup.documentGlobal
    );
    tabGroupState.splitViews = this._collectSplitViewDataForTabGroup(
      tabGroup.tabs
    );
    this._recordSavedTabGroupState(tabGroupState);
  },

  addTabsToSavedGroup(tabGroupId, tabs) {
    let tabGroupState = this.getSavedTabGroup(tabGroupId);
    if (!tabGroupState) {
      throw new Error(`No tab group found with id ${tabGroupId}`);
    }

    const win = tabs[0].documentGlobal;
    if (!tabs.every(tab => tab.documentGlobal === win)) {
      throw new Error(`All tabs must be part of the same window`);
    }

    if (PrivateBrowsingUtils.isWindowPrivate(win)) {
      throw new Error(
        "Refusing to add tabs from private window to a saved tab group"
      );
    }

    let newTabState = this._collectClosedTabsForTabGroup(tabs, win, {
      updateTabGroupId: tabGroupId,
    });
    tabGroupState.tabs.push(...newTabState);
    let newSplitViewData = this._collectSplitViewDataForTabGroup(tabs);

    tabGroupState.splitViews ??= [];
    tabGroupState.splitViews.push(...newSplitViewData);

    this._notifyOfSavedTabGroupsChange();
    return tabGroupState;
  },

  _recordSavedTabGroupState(savedTabGroupState) {
    if (
      !savedTabGroupState.tabs.length ||
      this.getSavedTabGroup(savedTabGroupState.id)
    ) {
      return;
    }
    this._savedGroups.push(savedTabGroupState);
    this._notifyOfSavedTabGroupsChange();
  },

  getSavedTabGroup(tabGroupId) {
    return this._savedGroups.find(
      savedTabGroup => savedTabGroup.id == tabGroupId
    );
  },

  getSavedTabGroups() {
    return Cu.cloneInto(this._savedGroups, {});
  },

  getClosedTabGroup(source, tabGroupId) {
    let winData = this._resolveClosedDataSource(source);
    return winData?.closedGroups.find(
      closedGroup => closedGroup.id == tabGroupId
    );
  },

  undoCloseTabGroup(source, tabGroupId, targetWindow) {
    const sourceWinData = this._resolveClosedDataSource(source);
    const isPrivateSource = Boolean(sourceWinData.isPrivate);
    if (targetWindow && !targetWindow.__SSi) {
      throw Components.Exception(
        "Target window is not tracked",
        Cr.NS_ERROR_INVALID_ARG
      );
    } else if (!targetWindow) {
      targetWindow = this._getTopWindow(isPrivateSource);
    }
    if (
      isPrivateSource !== PrivateBrowsingUtils.isWindowPrivate(targetWindow)
    ) {
      throw Components.Exception(
        "Target window doesn't have the same privateness as the source window",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    let tabGroupData = this.getClosedTabGroup(source, tabGroupId);
    if (!tabGroupData) {
      throw Components.Exception(
        "Tab group not found in source",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    let group = this._createTabsForSavedOrClosedTabGroup(
      tabGroupData,
      targetWindow
    );
    this.forgetClosedTabGroup(source, tabGroupId);
    sourceWinData.lastClosedTabGroupId = null;


    let isVerticalMode = targetWindow.gBrowser.tabContainer.verticalMode;

    group.select();
    return group;
  },

  openSavedTabGroup(tabGroupId, targetWindow) {
    if (!targetWindow) {
      targetWindow = this._getTopWindow();
    }
    if (!targetWindow.__SSi) {
      throw Components.Exception(
        "Target window is not tracked",
        Cr.NS_ERROR_INVALID_ARG
      );
    }
    if (PrivateBrowsingUtils.isWindowPrivate(targetWindow)) {
      throw Components.Exception(
        "Cannot open a saved tab group in a private window",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    let tabGroupData = this.getSavedTabGroup(tabGroupId);
    if (!tabGroupData) {
      throw Components.Exception(
        "No saved tab group with specified id",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    if (tabGroupData.windowClosedId) {
      let closedWinData = this.getClosedWindowDataByClosedId(
        tabGroupData.windowClosedId
      );
      if (closedWinData) {
        this._removeSavedTabGroupFromClosedWindow(
          closedWinData,
          tabGroupData.id
        );
      }
    }

    let group = this._createTabsForSavedOrClosedTabGroup(
      tabGroupData,
      targetWindow
    );
    this.forgetSavedTabGroup(tabGroupId);

    group.select();
    return group;
  },

  _createTabsForSavedOrClosedTabGroup(tabGroupData, targetWindow) {
    let tabDataList = tabGroupData.tabs.map(tab => tab.state);
    let tabs = targetWindow.gBrowser.createTabsForSessionRestore(
      true,
      0, 
      tabDataList,
      [tabGroupData],
      tabGroupData.splitViews
    );

    this.restoreTabs(targetWindow, tabs, tabDataList, 0);
    return tabs[0].group;
  },

  _cleanupOrphanedClosedGroups(winData) {
    if (!winData.closedGroups) {
      return;
    }
    for (let index = winData.closedGroups.length - 1; index >= 0; index--) {
      if (winData.closedGroups[index].tabs.length === 0) {
        winData.closedGroups.splice(index, 1);
        this._closedObjectsChanged = true;
      }
    }
  },

  _removeSavedTabGroupFromClosedWindow(closedWinData, tabGroupId) {
    removeWhere(closedWinData.groups, tabGroup => tabGroup.id == tabGroupId);
    removeWhere(closedWinData.tabs, tab => tab.groupId == tabGroupId);
    this._closedObjectsChanged = true;
  },

  async validateState(state) {
    if (!state) {
      state = this.getCurrentState();
      delete state.lastSessionState;
      delete state.deferredInitialState;
    }
    const schema = await fetch(
      "resource:///modules/sessionstore/session.schema.json"
    ).then(rsp => rsp.json());

    let result;
    try {
      result = lazy.JsonSchema.validate(state, schema);
      if (!result.valid) {
        console.warn(
          "Session state didn't validate against the schema",
          result.errors
        );
      }
    } catch (ex) {
      console.error(`Error validating session state: ${ex.message}`, ex);
    }
    return result;
  },
};

var TabRestoreQueue = {
  tabs: { priority: [], visible: [], hidden: [] },

  prefs: {
    get restoreOnDemand() {
      let updateValue = () => {
        let value = Services.prefs.getBoolPref(PREF);
        let definition = { value, configurable: true };
        Object.defineProperty(this, "restoreOnDemand", definition);
        return value;
      };

      const PREF = "browser.sessionstore.restore_on_demand";
      Services.prefs.addObserver(PREF, updateValue);
      return updateValue();
    },

    get restorePinnedTabsOnDemand() {
      let updateValue = () => {
        let value = Services.prefs.getBoolPref(PREF);
        let definition = { value, configurable: true };
        Object.defineProperty(this, "restorePinnedTabsOnDemand", definition);
        return value;
      };

      const PREF = "browser.sessionstore.restore_pinned_tabs_on_demand";
      Services.prefs.addObserver(PREF, updateValue);
      return updateValue();
    },

    get restoreHiddenTabs() {
      let updateValue = () => {
        let value = Services.prefs.getBoolPref(PREF);
        let definition = { value, configurable: true };
        Object.defineProperty(this, "restoreHiddenTabs", definition);
        return value;
      };

      const PREF = "browser.sessionstore.restore_hidden_tabs";
      Services.prefs.addObserver(PREF, updateValue);
      return updateValue();
    },
  },

  reset() {
    this.tabs = { priority: [], visible: [], hidden: [] };
  },

  add(tab) {
    let { priority, hidden, visible } = this.tabs;

    if (tab.pinned) {
      priority.push(tab);
    } else if (tab.hidden) {
      hidden.push(tab);
    } else {
      visible.push(tab);
    }
  },

  remove(tab) {
    let { priority, hidden, visible } = this.tabs;

    let set = priority;
    let index = set.indexOf(tab);

    if (index == -1) {
      set = tab.hidden ? hidden : visible;
      index = set.indexOf(tab);
    }

    if (index > -1) {
      set.splice(index, 1);
    }
  },

  shift() {
    let set;
    let { priority, hidden, visible } = this.tabs;

    let { restoreOnDemand, restorePinnedTabsOnDemand } = this.prefs;
    let restorePinned = !(restoreOnDemand && restorePinnedTabsOnDemand);
    if (restorePinned && priority.length) {
      set = priority;
    } else if (!restoreOnDemand) {
      if (visible.length) {
        set = visible;
      } else if (this.prefs.restoreHiddenTabs && hidden.length) {
        set = hidden;
      }
    }

    return set && set.shift();
  },

  hiddenToVisible(tab) {
    let { hidden, visible } = this.tabs;
    let index = hidden.indexOf(tab);

    if (index > -1) {
      hidden.splice(index, 1);
      visible.push(tab);
    }
  },

  visibleToHidden(tab) {
    let { visible, hidden } = this.tabs;
    let index = visible.indexOf(tab);

    if (index > -1) {
      visible.splice(index, 1);
      hidden.push(tab);
    }
  },

  willRestoreSoon(tab) {
    let { priority, hidden, visible } = this.tabs;
    let { restoreOnDemand, restorePinnedTabsOnDemand, restoreHiddenTabs } =
      this.prefs;
    let restorePinned = !(restoreOnDemand && restorePinnedTabsOnDemand);
    let candidateSet = [];

    if (restorePinned && priority.length) {
      candidateSet.push(...priority);
    }

    if (!restoreOnDemand) {
      if (visible.length) {
        candidateSet.push(...visible);
      }

      if (restoreHiddenTabs && hidden.length) {
        candidateSet.push(...hidden);
      }
    }

    return candidateSet.indexOf(tab) > -1;
  },
};

var DyingWindowCache = {
  _data: new WeakMap(),

  has(window) {
    return this._data.has(window);
  },

  get(window) {
    return this._data.get(window);
  },

  set(window, data) {
    this._data.set(window, data);
  },

  remove(window) {
    this._data.delete(window);
  },
};

var DirtyWindows = {
  _data: new WeakMap(),

  has(window) {
    return this._data.has(window);
  },

  add(window) {
    return this._data.set(window, true);
  },

  remove(window) {
    this._data.delete(window);
  },

  clear(_window) {
    this._data = new WeakMap();
  },
};

var LastSession = {
  _state: null,

  get canRestore() {
    return !!this._state;
  },

  getState() {
    return this._state;
  },

  setState(state) {
    this._state = state;
  },

  clear(silent = false) {
    if (this._state) {
      this._state = null;
      if (!silent) {
        Services.obs.notifyObservers(null, NOTIFY_LAST_SESSION_CLEARED);
      }
    }
  },
};

function removeWhere(array, predicate) {
  for (let i = array.length - 1; i >= 0; i--) {
    if (predicate(array[i])) {
      array.splice(i, 1);
    }
  }
}

export const _LastSession = LastSession;
