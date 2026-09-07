/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  clearTimeout: "resource://gre/modules/Timer.sys.mjs",
  setTimeout: "resource://gre/modules/Timer.sys.mjs",
});

const COLLECT_RESULTS_AFTER_MS = 10000;

const OBSERVED_TOPICS = [
  "sessionstore-restoring-on-startup",
  "sessionstore-initiating-manual-restore",
];

export var StartupPerformance = {
  RESTORED_TOPIC: "sessionstore-finished-restoring-initial-tabs",

  _startTimeStamp: null,

  _latestRestoredTimeStamp: null,

  _promiseFinished: null,

  _resolveFinished: null,

  _deadlineTimer: null,

  _hasFired: false,

  _isRestored: false,

  _totalNumberOfEagerTabs: 0,
  _totalNumberOfTabs: 0,
  _totalNumberOfWindows: 0,

  init() {
    for (let topic of OBSERVED_TOPICS) {
      Services.obs.addObserver(this, topic);
    }
  },

  get latestRestoredTimeStamp() {
    return this._latestRestoredTimeStamp;
  },

  get isRestored() {
    return this._isRestored;
  },

  _onRestorationStarts(isAutoRestore) {
    this._latestRestoredTimeStamp = this._startTimeStamp = Date.now();
    this._totalNumberOfEagerTabs = 0;
    this._totalNumberOfTabs = 0;
    this._totalNumberOfWindows = 0;


    for (let topic of OBSERVED_TOPICS) {
      Services.obs.removeObserver(this, topic);
    }

    Services.obs.addObserver(this, "sessionstore-single-window-restored");
    this._promiseFinished = new Promise(resolve => {
      this._resolveFinished = resolve;
    });
    this._promiseFinished.then(() => {
      try {
        this._isRestored = true;
        Services.obs.notifyObservers(null, this.RESTORED_TOPIC);

        if (this._latestRestoredTimeStamp == this._startTimeStamp) {
          return;
        }

        let delta = this._latestRestoredTimeStamp - this._startTimeStamp;
        if (isAutoRestore) {
        } else {
        }

        this._startTimeStamp = null;
      } catch (ex) {
        console.error("StartupPerformance: error after resolving promise", ex);
      }
    });
  },

  _startTimer() {
    if (this._hasFired) {
      return;
    }
    if (this._deadlineTimer) {
      lazy.clearTimeout(this._deadlineTimer);
    }
    this._deadlineTimer = lazy.setTimeout(() => {
      try {
        this._resolveFinished();
      } catch (ex) {
        console.error("StartupPerformance: Error in timeout handler", ex);
      } finally {
        this._deadlineTimer = null;
        this._hasFired = true;
        this._resolveFinished = null;
        Services.obs.removeObserver(
          this,
          "sessionstore-single-window-restored"
        );
      }
    }, COLLECT_RESULTS_AFTER_MS);
  },

  observe(subject, topic) {
    try {
      switch (topic) {
        case "sessionstore-restoring-on-startup":
          this._onRestorationStarts(true);
          break;
        case "sessionstore-initiating-manual-restore":
          this._onRestorationStarts(false);
          break;
        case "sessionstore-single-window-restored":
          {
            this._startTimer();

            this._totalNumberOfWindows += 1;

            let win = subject;

            let observer = event => {
              if (!event.detail.isRemotenessUpdate) {
                this._latestRestoredTimeStamp = Date.now();
                this._totalNumberOfEagerTabs += 1;
              }
            };
            win.gBrowser.tabContainer.addEventListener(
              "SSTabRestored",
              observer
            );
            this._totalNumberOfTabs += win.gBrowser.tabContainer.itemCount;

            this._promiseFinished.then(() => {
              if (!win.gBrowser.tabContainer) {
                return;
              }
              win.gBrowser.tabContainer.removeEventListener(
                "SSTabRestored",
                observer
              );
            });
          }
          break;
        default:
          throw new Error(`Unexpected topic ${topic}`);
      }
    } catch (ex) {
      console.error("StartupPerformance error", ex, ex.stack);
      throw ex;
    }
  },
};
