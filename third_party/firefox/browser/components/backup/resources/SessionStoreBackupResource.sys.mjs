/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

import {
  BackupResource,
  bytesToFuzzyKilobytes,
} from "resource:///modules/backup/BackupResource.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  SessionStore: "resource:///modules/sessionstore/SessionStore.sys.mjs",
  setTimeout: "resource://gre/modules/Timer.sys.mjs",
  TabStateFlusher: "resource:///modules/sessionstore/TabStateFlusher.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "logConsole", function () {
  return console.createInstance({
    prefix: "SessionStoreBackupResource",
    maxLogLevel: Services.prefs.getBoolPref("browser.backup.log", false)
      ? "Debug"
      : "Warn",
  });
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "TAB_FLUSH_TIMEOUT",
  "browser.backup.tab-flush-timeout",
  5000
);

export class SessionStoreBackupResource extends BackupResource {
  constructor(sessionStore = null) {
    super();
    this._sessionStore = sessionStore;
  }

  static get key() {
    return "sessionstore";
  }

  static get requiresEncryption() {
    return false;
  }

  get #sessionStore() {
    return this._sessionStore || lazy.SessionStore;
  }

  get filteredSessionStoreState() {
    let sessionStoreState = this.#sessionStore.getCurrentState(true);
    sessionStoreState.cookies = [];

    if (sessionStoreState.windows) {
      sessionStoreState.windows = sessionStoreState.windows.filter(
        w => !w?.isPrivate
      );
      sessionStoreState.windows.forEach(win => {
        if (win.tabs) {
          win.tabs.forEach(tab => delete tab.storage);
        }
        if (win._closedTabs) {
          win._closedTabs.forEach(closedTab => delete closedTab.state.storage);
        }
      });
    }
    if (sessionStoreState.savedGroups) {
      sessionStoreState.savedGroups.forEach(group => {
        if (group.tabs) {
          group.tabs.forEach(tab => delete tab.state.storage);
        }
      });
    }

    return sessionStoreState;
  }

  async backup(
    stagingPath,
    profilePath = PathUtils.profileDir,
    _isEncrypting = false
  ) {
    await Promise.race([
      Promise.allSettled(
        lazy.BrowserWindowTracker.orderedWindows.map(
          lazy.TabStateFlusher.flushWindow
        )
      ),
      new Promise((_, reject) =>
        lazy.setTimeout(reject, lazy.TAB_FLUSH_TIMEOUT, { timeout: true })
      ),
    ]).catch(e => {
      if (e?.timeout) {
        lazy.logConsole.warn("Timed out waiting while flushing tab state.");
      } else {
        lazy.logConsole.error(
          "Unrecognized error while flushing tab state.",
          e
        );
      }
    });

    let sessionStorePath = PathUtils.join(stagingPath, "sessionstore.jsonlz4");

    await IOUtils.writeJSON(sessionStorePath, this.filteredSessionStoreState, {
      compress: true,
    });
    await BackupResource.copyFiles(profilePath, stagingPath, [
      "sessionstore-backups",
    ]);

    return null;
  }

  async recover(_manifestEntry, recoveryPath, destProfilePath) {
    await BackupResource.copyFiles(recoveryPath, destProfilePath, [
      "sessionstore.jsonlz4",
      "sessionstore-backups",
    ]);

    return null;
  }

}
