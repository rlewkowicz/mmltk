/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  ExperimentAPI: "resource://nimbus/ExperimentAPI.sys.mjs",
  Normandy: "resource://normandy/Normandy.sys.mjs",
  TaskScheduler: "resource://gre/modules/TaskScheduler.sys.mjs",
});

const PREF_TIMEOUT = "first-startup.timeout";
const PREF_CATEGORY_TASKS = "first-startup.category-tasks-enabled";
const CATEGORY_NAME = "first-startup-new-profile";

export var FirstStartup = {
  NOT_STARTED: 0,
  IN_PROGRESS: 1,
  TIMED_OUT: 2,
  SUCCESS: 3,
  UNSUPPORTED: 4,

  _state: 0, 
  init(newProfile) {
    if (!newProfile) {
      return;
    }


    this._state = this.IN_PROGRESS;
    const timeout = Services.prefs.getIntPref(PREF_TIMEOUT, 30000); 
    let startingTime = ChromeUtils.now();
    let initialized = false;

    let promises = [];

    let normandyInitEndTime = null;
    let normandyInitPromise = null;
    if (AppConstants.MOZ_NORMANDY) {
      normandyInitPromise = lazy.Normandy.init({ runAsync: false }).finally(
        () => {
          normandyInitEndTime = ChromeUtils.now();
        }
      );
      promises.push(normandyInitPromise);
    }

    let deleteTasksEndTime = null;
    if (AppConstants.MOZ_UPDATE_AGENT) {
      promises.push(
        lazy.TaskScheduler.deleteAllTasks()
          .catch(() => {})
          .finally(() => {
            deleteTasksEndTime = ChromeUtils.now();
          })
      );
    }

    const CATEGORY_TASKS_ENABLED = Services.prefs.getBoolPref(
      PREF_CATEGORY_TASKS,
      false
    );
    let categoryTasksEndTime = null;
    if (CATEGORY_TASKS_ENABLED && AppConstants.MOZ_NORMANDY) {
      promises.push(
        normandyInitPromise.finally(() => {
          return lazy.BrowserUtils.callModulesFromCategory({
            categoryName: CATEGORY_NAME,
            profileMarker: "first-startup-new-profile-tasks",
            idleDispatch: false,
          }).finally(() => {
            categoryTasksEndTime = ChromeUtils.now();
          });
        })
      );
    }

    if (promises.length) {
      Promise.allSettled(promises).then(() => (initialized = true));

      this.elapsed = 0;
      Services.tm.spinEventLoopUntil("FirstStartup.sys.mjs:init", () => {
        this.elapsed = Math.ceil(ChromeUtils.now() - startingTime);
        if (this.elapsed >= timeout) {
          this._state = this.TIMED_OUT;
          return true;
        } else if (initialized) {
          this._state = this.SUCCESS;
          return true;
        }
        return false;
      });
    } else {
      this._state = this.UNSUPPORTED;
    }

    if (AppConstants.MOZ_NORMANDY) {

      const nimbusTimestamps =
        lazy.ExperimentAPI.getAndClearFirstStartupTimestamps();




    }

    if (AppConstants.MOZ_UPDATE_AGENT) {
    }

    if (CATEGORY_TASKS_ENABLED) {
    }

  },

  get state() {
    return this._state;
  },

  resetForTesting() {
    this._state = this.NOT_STARTED;
  },
};
