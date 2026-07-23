/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */



const lazy = {};
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

ChromeUtils.defineESModuleGetters(lazy, {
  CrashMonitor: "resource://gre/modules/CrashMonitor.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  SessionFile: "resource:///modules/sessionstore/SessionFile.sys.mjs",
  StartupPerformance:
    "resource:///modules/sessionstore/StartupPerformance.sys.mjs",
  sessionStoreLogger: "resource:///modules/sessionstore/SessionLogger.sys.mjs",
});

const STATE_RUNNING_STR = "running";

const TYPE_NO_SESSION = 0;
const TYPE_RECOVER_SESSION = 1;
const TYPE_RESUME_SESSION = 2;
const TYPE_DEFER_SESSION = 3;

const BROWSER_STARTUP_RESUME_SESSION = 3;

var gOnceInitializedDeferred = Promise.withResolvers();


export var SessionStartup = {
  NO_SESSION: TYPE_NO_SESSION,
  RECOVER_SESSION: TYPE_RECOVER_SESSION,
  RESUME_SESSION: TYPE_RESUME_SESSION,
  DEFER_SESSION: TYPE_DEFER_SESSION,

  _initialState: null,
  _sessionType: null,
  _initialized: false,

  _previousSessionCrashed: null,

  _resumeSessionEnabled: null,


  init() {
    Services.obs.notifyObservers(null, "sessionstore-init-started");

    if (!AppConstants.DEBUG) {
      lazy.StartupPerformance.init();
    }

    if (lazy.PrivateBrowsingUtils.permanentPrivateBrowsing) {
      this._initialized = true;
      gOnceInitializedDeferred.resolve();
      return;
    }

    if (
      Services.prefs.getBoolPref(
        "browser.sessionstore.resuming_after_os_restart"
      )
    ) {
      lazy.sessionStoreLogger.debug("resuming_after_os_restart");
      if (!Services.appinfo.restartedByOS) {
        Services.prefs.setBoolPref(
          "browser.sessionstore.resume_session_once",
          false
        );
      }
      Services.prefs.setBoolPref(
        "browser.sessionstore.resuming_after_os_restart",
        false
      );
    }

    lazy.SessionFile.read().then(
      result => {
        lazy.sessionStoreLogger.debug(
          `Completed SessionFile.read() with result.origin: ${result.origin}`
        );
        return this._onSessionFileRead(result);
      },
      err => {
        lazy.sessionStoreLogger.error("Failure from _onSessionFileRead", err);
      }
    );
  },

  _createSupportsString(data) {
    let string = Cc["@mozilla.org/supports-string;1"].createInstance(
      Ci.nsISupportsString
    );
    string.data = data;
    return string;
  },

  _onSessionFileRead({ source, parsed, noFilesFound }) {
    this._initialized = true;
    const crashReasons = {
      FINAL_STATE_WRITING_INCOMPLETE: "final-state-write-incomplete",
      SESSION_STATE_FLAG_MISSING:
        "session-state-missing-or-running-at-last-write",
    };

    let supportsStateString = this._createSupportsString(source);
    Services.obs.notifyObservers(
      supportsStateString,
      "sessionstore-state-read"
    );
    let stateString = supportsStateString.data;

    if (stateString != source) {
      lazy.sessionStoreLogger.debug(
        "After sessionstore-state-read, session has been modified"
      );
      try {
        this._initialState = JSON.parse(stateString);
      } catch (ex) {
        lazy.sessionStoreLogger.error(
          "'sessionstore-state-read' observer rewrote the state to something that won't parse",
          ex
        );
      }
    } else {
      this._initialState = parsed;
    }

    if (this._initialState == null) {
      this._sessionType = this.NO_SESSION;
      lazy.sessionStoreLogger.debug("No valid session found");
      Services.obs.notifyObservers(null, "sessionstore-state-finalized");
      gOnceInitializedDeferred.resolve();
      return;
    }

    let isAutomaticRestoreEnabled = this.isAutomaticRestoreEnabled();
    lazy.sessionStoreLogger.debug(
      `isAutomaticRestoreEnabled: ${isAutomaticRestoreEnabled}`
    );
    if (!isAutomaticRestoreEnabled && this._initialState) {
      lazy.sessionStoreLogger.debug(
        "Discarding previous session as we have initialState"
      );
      delete this._initialState.lastSessionState;
    }

    let previousSessionCrashedReason = "N/A";
    lazy.CrashMonitor.previousCheckpoints.then(checkpoints => {
      if (checkpoints) {
        this._previousSessionCrashed =
          !checkpoints["sessionstore-final-state-write-complete"];
        if (!checkpoints["sessionstore-final-state-write-complete"]) {
          previousSessionCrashedReason =
            crashReasons.FINAL_STATE_WRITING_INCOMPLETE;
        }
      } else if (noFilesFound) {

        this._previousSessionCrashed = false;
      } else {
        let stateFlagPresent =
          this._initialState.session && this._initialState.session.state;

        this._previousSessionCrashed =
          !stateFlagPresent ||
          this._initialState.session.state == STATE_RUNNING_STR;
        if (
          !stateFlagPresent ||
          this._initialState.session.state == STATE_RUNNING_STR
        ) {
          previousSessionCrashedReason =
            crashReasons.SESSION_STATE_FLAG_MISSING;
        }
      }

      lazy.sessionStoreLogger.debug(
        `Previous shutdown ok? ${this._previousSessionCrashed}, reason: ${previousSessionCrashedReason}`
      );

      Services.obs.addObserver(this, "sessionstore-windows-restored", true);

      if (this.sessionType == this.NO_SESSION) {
        lazy.sessionStoreLogger.debug("Will restore no session");
        this._initialState = null; 
      } else {
        Services.obs.addObserver(this, "browser:purge-session-history", true);
      }

      Services.obs.notifyObservers(null, "sessionstore-state-finalized");

      gOnceInitializedDeferred.resolve();
    });
  },

  observe(subject, topic) {
    switch (topic) {
      case "sessionstore-windows-restored":
        Services.obs.removeObserver(this, "sessionstore-windows-restored");
        lazy.sessionStoreLogger.debug(`sessionstore-windows-restored`);
        this._initialState = null;
        this._didRestore = true;
        break;
      case "browser:purge-session-history":
        Services.obs.removeObserver(this, "browser:purge-session-history");
        this._sessionType = this.NO_SESSION;
        break;
    }
  },


  get onceInitialized() {
    return gOnceInitializedDeferred.promise;
  },

  get state() {
    return this._initialState;
  },

  isAutomaticRestoreEnabled() {
    if (this._resumeSessionEnabled === null) {
      this._resumeSessionEnabled =
        !lazy.PrivateBrowsingUtils.permanentPrivateBrowsing &&
        (Services.prefs.getBoolPref(
          "browser.sessionstore.resume_session_once"
        ) ||
          Services.prefs.getIntPref("browser.startup.page") ==
            BROWSER_STARTUP_RESUME_SESSION);
    }

    return this._resumeSessionEnabled;
  },

  willRestore() {
    return (
      this.sessionType == this.RECOVER_SESSION ||
      this.sessionType == this.RESUME_SESSION
    );
  },

  willRestoreAsCrashed() {
    return this.sessionType == this.RECOVER_SESSION;
  },

  get willOverrideHomepage() {
    if (!this._initialState && !this.isAutomaticRestoreEnabled()) {
      return false;
    }
    if (this._didRestore) {
      return false;
    }

    return new Promise(resolve => {
      this.onceInitialized.then(() => {
        resolve(
          this.willRestore() &&
            this._initialState &&
            this._initialState.windows &&
            (!this.willRestoreAsCrashed()
              ? this._initialState.windows.filter(w => !w._maybeDontRestoreTabs)
              : this._initialState.windows
            ).some(w => w.tabs.some(t => !t.pinned))
        );
      });
    });
  },

  get sessionType() {
    if (this._sessionType === null) {
      let resumeFromCrash = Services.prefs.getBoolPref(
        "browser.sessionstore.resume_from_crash"
      );
      if (this.isAutomaticRestoreEnabled()) {
        this._sessionType = this.RESUME_SESSION;
      } else if (this._previousSessionCrashed && resumeFromCrash) {
        this._sessionType = this.RECOVER_SESSION;
      } else if (this._initialState) {
        this._sessionType = this.DEFER_SESSION;
      } else {
        this._sessionType = this.NO_SESSION;
      }
    }

    return this._sessionType;
  },

  get previousSessionCrashed() {
    return this._previousSessionCrashed;
  },

  resetForTest() {
    this._resumeSessionEnabled = null;
    this._sessionType = null;
  },

  QueryInterface: ChromeUtils.generateQI([
    "nsIObserver",
    "nsISupportsWeakReference",
  ]),
};
