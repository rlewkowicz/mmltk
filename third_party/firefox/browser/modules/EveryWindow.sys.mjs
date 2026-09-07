/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


var initialized = false;
var callbacks = new Map();

function callForEveryWindow(callback) {
  let windowList = Services.wm.getEnumerator("navigator:browser");
  for (let win of windowList) {
    win.delayedStartupPromise.then(() => {
      callback(win);
    });
  }
}

export const EveryWindow = {
  get readyWindows() {
    return Array.from(Services.wm.getEnumerator("navigator:browser")).filter(
      win => win.gBrowserInit?.delayedStartupFinished
    );
  },

  registerCallback: function EW_registerCallback(id, init, uninit) {
    if (callbacks.has(id)) {
      return false;
    }

    if (!initialized) {
      let addUnloadListener = win => {
        function observer(subject, topic) {
          if (topic == "domwindowclosed" && subject === win) {
            Services.ww.unregisterNotification(observer);
            for (let c of callbacks.values()) {
              c.uninit(win, true);
            }
          }
        }
        Services.ww.registerNotification(observer);
      };

      Services.obs.addObserver(win => {
        for (let c of callbacks.values()) {
          c.init(win);
        }
        addUnloadListener(win);
      }, "browser-delayed-startup-finished");

      callForEveryWindow(addUnloadListener);

      initialized = true;
    }

    callForEveryWindow(init);
    callbacks.set(id, { id, init, uninit });

    return true;
  },

  unregisterCallback: function EW_unregisterCallback(id, callUninit = true) {
    if (!callbacks.has(id)) {
      return;
    }

    if (callUninit) {
      callForEveryWindow(callbacks.get(id).uninit);
    }

    callbacks.delete(id);
  },
};
