/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import {
  cancelIdleCallback,
  clearTimeout,
  requestIdleCallback,
  setTimeout,
} from "resource://gre/modules/Timer.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PrivacyFilter: "resource://gre/modules/sessionstore/PrivacyFilter.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  RunState: "resource:///modules/sessionstore/RunState.sys.mjs",
  SessionFile: "resource:///modules/sessionstore/SessionFile.sys.mjs",
  SessionStore: "resource:///modules/sessionstore/SessionStore.sys.mjs",
  sessionStoreLogger: "resource:///modules/sessionstore/SessionLogger.sys.mjs",
});

const PREF_INTERVAL_ACTIVE = "browser.sessionstore.interval";
const PREF_INTERVAL_IDLE = "browser.sessionstore.interval.idle";
const PREF_IDLE_DELAY = "browser.sessionstore.idleDelay";

function notify(subject, topic) {
  Services.obs.notifyObservers(subject, topic);
}

export var SessionSaver = Object.freeze({
  run() {
    if (!lazy.RunState.isRunning) {
      lazy.sessionStoreLogger.debug("SessionSave run called during shutdown");
    }
    return SessionSaverInternal.run();
  },

  runDelayed() {
    SessionSaverInternal.runDelayed();
  },

  get lastSaveTime() {
    return SessionSaverInternal._lastSaveTime;
  },

  updateLastSaveTime() {
    SessionSaverInternal.updateLastSaveTime();
  },

  cancel() {
    SessionSaverInternal.cancel();
  },
});

var SessionSaverInternal = {
  _timeoutID: null,

  _idleCallbackID: null,

  _lastSaveTime: 0,

  _isIdle: false,

  _wasIdle: false,

  _intervalWhileActive: null,

  _intervalWhileIdle: null,

  _idleDelay: null,

  run() {
    return this._saveState(true );
  },

  runDelayed(delay = 2000) {
    if (this._timeoutID) {
      return;
    }

    let interval = this._isIdle
      ? this._intervalWhileIdle
      : this._intervalWhileActive;
    delay = Math.max(this._lastSaveTime + interval - Date.now(), delay, 0);

    this._wasIdle = this._isIdle;
    if (!lazy.RunState.isRunning) {
      lazy.sessionStoreLogger.debug(
        "SessionSaver scheduling a state save during shutdown"
      );
    }
    this._timeoutID = setTimeout(() => {
      let saveStateAsyncWhenIdle = () => {
        if (!lazy.RunState.isRunning) {
          lazy.sessionStoreLogger.debug(
            "SessionSaver saveStateAsyncWhenIdle callback during shutdown"
          );
        }
        this._saveStateAsync();
      };

      this._idleCallbackID = requestIdleCallback(saveStateAsyncWhenIdle);
    }, delay);
  },

  updateLastSaveTime() {
    this._lastSaveTime = Date.now();
  },

  cancel() {
    clearTimeout(this._timeoutID);
    this._timeoutID = null;
    cancelIdleCallback(this._idleCallbackID);
    this._idleCallbackID = null;
  },

  observe(subject, topic) {
    switch (topic) {
      case "idle":
        this._isIdle = true;
        break;
      case "active":
        this._isIdle = false;
        if (this._timeoutID && this._wasIdle) {
          clearTimeout(this._timeoutID);
          this._timeoutID = null;
          this.runDelayed();
        }
        break;
      default:
        throw new Error(`Unexpected change value ${topic}`);
    }
  },

  _saveState(forceUpdateAllWindows = false) {
    this.cancel();

    if (lazy.PrivateBrowsingUtils.permanentPrivateBrowsing) {

      this.updateLastSaveTime();
      return Promise.resolve();
    }

    let state = lazy.SessionStore.getCurrentState(forceUpdateAllWindows);
    lazy.PrivacyFilter.filterPrivateWindowsAndTabs(state);

    lazy.SessionStore.keepOnlyWorthSavingTabs(state);

    if (state.deferredInitialState) {
      state.windows = state.deferredInitialState.windows || [];
      delete state.deferredInitialState;
    }

    if (AppConstants.platform != "macosx") {
      if (lazy.sessionStoreLogger.isDebug) {
        lazy.sessionStoreLogger.debug(
          "SessionSaver._saveState, closed windows:"
        );
        for (let closedWin of state._closedWindows) {
          lazy.sessionStoreLogger.debug(
            `\t${closedWin.closedId}\t${closedWin.closedAt}\t${closedWin._shouldRestore}`
          );
        }
      }

      while (state._closedWindows.length) {
        let i = state._closedWindows.length - 1;

        if (!state._closedWindows[i]._shouldRestore) {
          break;
        }

        delete state._closedWindows[i]._shouldRestore;
        state.windows.unshift(state._closedWindows.pop());
      }
    }

    this._maybeClearCookiesAndStorage(state);

    return this._writeState(state);
  },

  _maybeClearCookiesAndStorage(state) {
    if (!lazy.RunState.isClosing) {
      return;
    }

    if (
      Services.prefs.getBoolPref("browser.sessionstore.resume_session_once")
    ) {
      return;
    }
    let sanitizeCookies =
      Services.prefs.getBoolPref("privacy.sanitize.sanitizeOnShutdown") &&
      Services.prefs.getBoolPref("privacy.clearOnShutdown.cookies");

    if (sanitizeCookies) {
      delete state.cookies;

      for (let window of state.windows) {
        for (let tab of window.tabs) {
          delete tab.storage;
        }
      }
    }
  },

  _saveStateAsync() {
    this._timeoutID = null;

    this._saveState();
  },

  _writeState(state) {
    if (!lazy.RunState.isRunning) {
      lazy.sessionStoreLogger.debug(
        "SessionSaver writing state during shutdown"
      );
    }
    this.updateLastSaveTime();

    return lazy.SessionFile.write(state).then(
      () => {
        this.updateLastSaveTime();
        if (!lazy.RunState.isRunning) {
          lazy.sessionStoreLogger.debug(
            "SessionSaver sessionstore-state-write-complete during shutdown"
          );
        }
        notify(null, "sessionstore-state-write-complete");
      },
      err => {
        lazy.sessionStoreLogger.error(
          "SessionSaver write() rejected with error",
          err
        );
      }
    );
  },
};

XPCOMUtils.defineLazyPreferenceGetter(
  SessionSaverInternal,
  "_intervalWhileActive",
  PREF_INTERVAL_ACTIVE,
  15000 ,
  () => {
    SessionSaverInternal.cancel();
    SessionSaverInternal.runDelayed(0);
  }
);

XPCOMUtils.defineLazyPreferenceGetter(
  SessionSaverInternal,
  "_intervalWhileIdle",
  PREF_INTERVAL_IDLE,
  3600000 
);

XPCOMUtils.defineLazyPreferenceGetter(
  SessionSaverInternal,
  "_idleDelay",
  PREF_IDLE_DELAY,
  180 ,
  (key, previous, latest) => {
    var idleService = Cc["@mozilla.org/widget/useridleservice;1"].getService(
      Ci.nsIUserIdleService
    );
    if (previous != undefined) {
      idleService.removeIdleObserver(SessionSaverInternal, previous);
    }
    if (latest != undefined) {
      idleService.addIdleObserver(SessionSaverInternal, latest);
    }
  }
);

var idleService = Cc["@mozilla.org/widget/useridleservice;1"].getService(
  Ci.nsIUserIdleService
);
idleService.addIdleObserver(
  SessionSaverInternal,
  SessionSaverInternal._idleDelay
);
