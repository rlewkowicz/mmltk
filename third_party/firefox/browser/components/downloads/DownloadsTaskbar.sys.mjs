/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */



const lazy = {};
const gInterfaces = {};

function defineResettableGetter(object, name, callback) {
  let result = undefined;

  Object.defineProperty(object, name, {
    get() {
      if (typeof result == "undefined") {
        result = callback();
      }

      return result;
    },
    set(value) {
      if (value === null) {
        result = undefined;
      } else {
        throw new Error("don't set this to nonnull");
      }
    },
  });
}

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  Downloads: "resource://gre/modules/Downloads.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

defineResettableGetter(gInterfaces, "winTaskbar", function () {
  if (!("@mozilla.org/windows-taskbar;1" in Cc)) {
    return null;
  }
  let winTaskbar = Cc["@mozilla.org/windows-taskbar;1"].getService(
    Ci.nsIWinTaskbar
  );
  return winTaskbar.available && winTaskbar;
});

defineResettableGetter(gInterfaces, "macTaskbarProgress", function () {
  return (
    "@mozilla.org/widget/macdocksupport;1" in Cc &&
    Cc["@mozilla.org/widget/macdocksupport;1"].getService(Ci.nsITaskbarProgress)
  );
});

defineResettableGetter(gInterfaces, "gtkTaskbarProgress", function () {
  return (
    "@mozilla.org/widget/taskbarprogress/gtk;1" in Cc &&
    Cc["@mozilla.org/widget/taskbarprogress/gtk;1"].getService(
      Ci.nsIGtkTaskbarProgress
    )
  );
});

class DownloadsTaskbarInstance {
  #summary = null;

  #taskbarProgresses = new Set();

  #filter = null;

  constructor(aFilter) {
    this.#filter = aFilter;
  }

  async registerIndicator(aBrowserWindow, aForcedBackend) {
    if (
      aForcedBackend == "windows" ||
      (!aForcedBackend && gInterfaces.winTaskbar)
    ) {
      this.#windowsAttachIndicator(aBrowserWindow);
    } else if (!this.#taskbarProgresses.size) {
      if (
        aForcedBackend == "mac" ||
        (!aForcedBackend && gInterfaces.macTaskbarProgress)
      ) {
        this.#taskbarProgresses.add(gInterfaces.macTaskbarProgress);
        Services.obs.addObserver(() => {
          this.#taskbarProgresses.clear();
          gInterfaces.macTaskbarProgress = null;
        }, "quit-application-granted");
      } else if (
        aForcedBackend == "linux" ||
        (!aForcedBackend && gInterfaces.gtkTaskbarProgress)
      ) {
        this.#taskbarProgresses.add(gInterfaces.gtkTaskbarProgress);

        this.#attachGtkTaskbarProgress(aBrowserWindow);
      } else {
        return;
      }
    }

    if (!this.#summary) {
      try {
        let summary = await lazy.Downloads.getSummary(this.#filter);

        if (!this.#summary) {
          this.#summary = summary;
          await this.#summary.addView(this);
        }
      } catch (e) {
        console.error(e);
      }
    }
  }

  #windowsAttachIndicator(aWindow) {
    let { docShell } = aWindow.browsingContext.topChromeWindow;
    let taskbarProgress = gInterfaces.winTaskbar.getTaskbarProgress(docShell);
    this.#taskbarProgresses.add(taskbarProgress);

    if (this.#summary) {
      this.onSummaryChanged();
    }

    aWindow.addEventListener("unload", () => {
      this.#taskbarProgresses.delete(taskbarProgress);
    });
  }

  #attachGtkTaskbarProgress(aWindow) {
    let taskbarProgress = this.#taskbarProgresses.values().next().value;
    taskbarProgress.setPrimaryWindow(aWindow);

    if (this.#summary) {
      this.onSummaryChanged();
    }

    aWindow.addEventListener("unload", () => {
      let browserWindow = this.#determineProgressRepresentative();
      if (browserWindow) {
        this.#attachGtkTaskbarProgress(browserWindow);
      } else {
        this.#taskbarProgresses.clear();
      }
    });
  }

  #determineProgressRepresentative() {
    if (this.#filter == lazy.Downloads.ALL) {
      return lazy.BrowserWindowTracker.getTopWindow();
    }

    return lazy.BrowserWindowTracker.getTopWindow({
      private: this.#filter == lazy.Downloads.PRIVATE,
    });
  }

  reset() {
    if (this.#summary) {
      this.#summary.removeView(this);
    }

    this.#taskbarProgresses.clear();
  }

  updateProgress(aProgressState, aCurrentValue, aMaxValue) {
    for (let progress of this.#taskbarProgresses) {
      progress.setProgressState(aProgressState, aCurrentValue, aMaxValue);
    }
  }


  onSummaryChanged() {
    if (!this.#taskbarProgresses.size) {
      return;
    }

    if (this.#summary.allHaveStopped || this.#summary.progressTotalBytes == 0) {
      this.updateProgress(Ci.nsITaskbarProgress.STATE_NO_PROGRESS, 0, 0);
    } else if (this.#summary.allUnknownSize) {
      this.updateProgress(Ci.nsITaskbarProgress.STATE_INDETERMINATE, 0, 0);
    } else {
      let progressCurrentBytes = Math.min(
        this.#summary.progressTotalBytes,
        this.#summary.progressCurrentBytes
      );
      this.updateProgress(
        Ci.nsITaskbarProgress.STATE_NORMAL,
        progressCurrentBytes,
        this.#summary.progressTotalBytes
      );
    }
  }
}

const gDownloadsTaskbarInstances = {};

export var DownloadsTaskbar = {
  async registerIndicator(aWindow, aForcedBackend) {
    let filter = this._selectFilterForWindow(aWindow, aForcedBackend);
    if (!(filter in gDownloadsTaskbarInstances)) {
      gDownloadsTaskbarInstances[filter] = new DownloadsTaskbarInstance(filter);
    }

    await gDownloadsTaskbarInstances[filter].registerIndicator(
      aWindow,
      aForcedBackend
    );
  },

  _selectFilterForWindow(aWindow, aForcedBackend) {
    if (
      aForcedBackend == "windows" ||
      (!aForcedBackend && gInterfaces.winTaskbar)
    ) {
      return lazy.PrivateBrowsingUtils.isWindowPrivate(aWindow)
        ? lazy.Downloads.PRIVATE
        : lazy.Downloads.PUBLIC;
    }

    return lazy.Downloads.ALL;
  },

  resetBetweenTests() {
    for (const key of Object.keys(gDownloadsTaskbarInstances)) {
      gDownloadsTaskbarInstances[key].reset();
      delete gDownloadsTaskbarInstances[key];
    }

    gInterfaces.macTaskbarProgress = null;
    gInterfaces.winTaskbar = null;
    gInterfaces.gtkTaskbarProgress = null;
  },
};
