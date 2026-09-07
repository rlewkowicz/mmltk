/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const { AppConstants } = ChromeUtils.importESModule(
  "resource://gre/modules/AppConstants.sys.mjs"
);
const { XPCOMUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
);


const lazy = {};

if (Cc["@mozilla.org/gio-service;1"]) {
  XPCOMUtils.defineLazyServiceGetter(
    lazy,
    "gGIOService",
    "@mozilla.org/gio-service;1",
    Ci.nsIGIOService
  );
} else {
  lazy.gGIOService = null;
}

export const DefaultBrowserHelper = {
  _backoffIndex: 0,

  _pollingTimer: undefined,

  _lastPolledIsDefault: undefined,

  _changeListeners: new Set(),

  get shellSvc() {
    return (
      AppConstants.HAVE_SHELL_SERVICE &&
      // @ts-ignore from utilityOverlay.js
      window.getShellService()
    );
  },

  pollForDefaultChanges(hasChanged) {
    this._changeListeners.add(hasChanged);

    if (!this._pollingTimer) {
      this._lastPolledIsDefault = this.isBrowserDefault;

      const backoffTimes = [
        1000, 1000, 1000, 1000, 2000, 2000, 2000, 5000, 5000, 10000,
      ];

      const pollForDefaultBrowser = () => {
        if (
          (location.hash == "" ||
            location.hash == "#general" ||
            location.hash == "#sync" ||
            location.hash == "#browserIcon") &&
          document.visibilityState == "visible"
        ) {
          const { isBrowserDefault } = this;
          if (isBrowserDefault !== this._lastPolledIsDefault) {
            this._lastPolledIsDefault = isBrowserDefault;
            for (let listener of this._changeListeners) {
              listener();
            }
          }
        }

        if (!this._pollingTimer) {
          return;
        }

        this._pollingTimer = window.setTimeout(
          () => {
            window.requestIdleCallback(pollForDefaultBrowser);
          },
          backoffTimes[
            this._backoffIndex + 1 < backoffTimes.length
              ? this._backoffIndex++
              : backoffTimes.length - 1
          ]
        );
      };

      this._pollingTimer = window.setTimeout(() => {
        window.requestIdleCallback(pollForDefaultBrowser);
      }, backoffTimes[this._backoffIndex]);
    }

    return () => {
      this._changeListeners.delete(hasChanged);
      if (!this._changeListeners.size) {
        this.clearPollingForDefaultChanges();
      }
    };
  },

  clearPollingForDefaultChanges() {
    if (this._pollingTimer) {
      clearTimeout(this._pollingTimer);
      this._pollingTimer = undefined;
    }
  },

  get isBrowserDefault() {
    if (!this.canCheck) {
      return false;
    }
    return this.shellSvc?.isDefaultBrowser(false, true);
  },

  async setDefaultBrowser() {
    this._backoffIndex = 0;

    try {
      await this.shellSvc?.setDefaultBrowser(false);
    } catch (e) {
      console.error(e);
    }
  },

  get canCheck() {
    return (
      this.shellSvc &&
      !lazy.gGIOService?.isRunningUnderFlatpak
    );
  },
};
