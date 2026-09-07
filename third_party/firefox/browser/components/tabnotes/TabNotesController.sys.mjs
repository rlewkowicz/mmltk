/**
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";


const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  TabNotes: "moz-src:///browser/components/tabnotes/TabNotes.sys.mjs",
});
ChromeUtils.defineLazyGetter(lazy, "logConsole", function () {
  return console.createInstance({
    prefix: "TabNotesController",
    maxLogLevel: Services.prefs.getBoolPref("browser.tabs.notes.debug", false)
      ? "Debug"
      : "Warn",
  });
});

const EVENTS = [
  "CanonicalURL:Identified",
  "TabNote:Created",
  "TabNote:Edited",
  "TabNote:Removed",
  "TabNote:Expand",
];

class TabNotesControllerClass {
  TAB_NOTES_ENABLED;

  #isStartupComplete = false;
  #isInitialized = false;

  browserFirstWindowReady() {
    XPCOMUtils.defineLazyPreferenceGetter(
      this,
      "TAB_NOTES_ENABLED",
      "browser.tabs.notes.enabled",
      false
    );
    this.#isStartupComplete = true;
    Services.obs.addObserver(this, "CanonicalURL:ActorRegistered");
    Services.obs.addObserver(this, "CanonicalURL:ActorUnregistered");
    if (this.TAB_NOTES_ENABLED) {
      lazy.logConsole.debug("browserFirstWindowReady", "Tab notes enabled");
      return this.#init().then(() => {
        for (const win of lazy.BrowserWindowTracker.orderedWindows) {
          this.#initWindow(win);
        }
      });
    }
    lazy.logConsole.debug("browserFirstWindowReady", "Tab notes disabled");
    return Promise.resolve();
  }

  async #init() {
    if (!this.#isInitialized) {
      this.#isInitialized = true;
      lazy.logConsole.debug("TabNotes initialized");
      return lazy.TabNotes.init();
    }
    return Promise.resolve();
  }

  browserWindowDelayedStartup(win) {
    lazy.logConsole.debug("browserWindowDelayedStartup", win);
    if (!this.#isStartupComplete) {
      lazy.logConsole.debug(
        "browserWindowDelayedStartup",
        "initialization deferred until startup complete"
      );
      return;
    }
    if (this.TAB_NOTES_ENABLED) {
      this.#initWindow(win);
    }
  }

  #initWindow(win) {
    EVENTS.forEach(eventName => win.addEventListener(eventName, this));
    win.gBrowser.addTabsProgressListener(this);

    for (const tab of win.gBrowser.tabs) {
      if (tab.canonicalUrl && lazy.TabNotes.isEligible(tab)) {
        lazy.TabNotes.has(tab).then(hasTabNote => {
          tab.hasTabNote = hasTabNote;
        });
      }
    }

    lazy.logConsole.debug("initWindow", win, EVENTS);
  }

  browserWindowUnload(win) {
    if (this.TAB_NOTES_ENABLED) {
      this.#unloadWindow(win);
      lazy.logConsole.debug("browserWindowUnload", EVENTS, win);
    }
  }

  #unloadWindow(win) {
    EVENTS.forEach(eventName => win.removeEventListener(eventName, this));
    win.gBrowser.removeTabsProgressListener(this);
    lazy.logConsole.debug("unloadWindow", win, EVENTS);
  }

  browserQuitApplicationGranted() {
    return this.#deinit();
  }

  async #deinit() {
    if (this.#isInitialized) {
      this.#isInitialized = false;
      lazy.logConsole.debug("TabNotes deinitialized");
    }
    return lazy.TabNotes.deinit();
  }

  handleEvent(event) {
    switch (event.type) {
      case "CanonicalURL:Identified":
        {
          const browser = event.target;
          const { canonicalUrl } = event.detail;
          const gBrowser = browser.getTabBrowser();
          const tab = gBrowser.getTabForBrowser(browser);
          tab.canonicalUrl = canonicalUrl;
          lazy.TabNotes.has(tab).then(hasTabNote => {
            tab.hasTabNote = hasTabNote;
            lazy.logConsole.debug("TabNote:Determined", tab, hasTabNote);
            tab.dispatchEvent(
              new CustomEvent("TabNote:Determined", {
                detail: { hasTabNote },
              })
            );
          });

          lazy.logConsole.debug("CanonicalURL:Identified", tab, canonicalUrl);
        }
        break;
      case "TabNote:Created":
        {
          const { note, telemetrySource } = event.detail;
          if (telemetrySource) {
          }
          const { canonicalUrl } = event.target;
          for (const win of lazy.BrowserWindowTracker.orderedWindows) {
            for (const tab of win.gBrowser.tabs) {
              if (tab.canonicalUrl == canonicalUrl) {
                tab.hasTabNote = true;
              }
            }
          }
          lazy.logConsole.debug("TabNote:Created", canonicalUrl);
        }
        break;
      case "TabNote:Edited":
        {
          const { canonicalUrl } = event.target;
          const { note, telemetrySource } = event.detail;
          if (telemetrySource) {
          }
          lazy.logConsole.debug("TabNote:Edited", canonicalUrl);
        }
        break;
      case "TabNote:Removed":
        {
          const { telemetrySource, note } = event.detail;
          const now = Temporal.Now.instant();
          const noteAgeHours = Math.round(
            now.since(note.created).total("hours")
          );
          if (telemetrySource) {
          }

          const { canonicalUrl } = event.target;
          for (const win of lazy.BrowserWindowTracker.orderedWindows) {
            for (const tab of win.gBrowser.tabs) {
              if (tab.canonicalUrl == canonicalUrl) {
                tab.hasTabNote = false;
              }
            }
          }
          lazy.logConsole.debug("TabNote:Removed", canonicalUrl);
        }
        break;

      case "TabNote:Expand": {
        const tab = event.target;
        lazy.TabNotes.get(tab).then(note => {
          if (note) {
            lazy.logConsole.debug("TabNote:Expand", note);
          }
        });
      }
    }
  }

  observe(_aSubject, aTopic) {
    switch (aTopic) {
      case "CanonicalURL:ActorRegistered":
        lazy.logConsole.debug(
          "CanonicalURL actor registered, requesting canonical URLs"
        );
        this.#init()
          .then(() => {
            for (const win of lazy.BrowserWindowTracker.orderedWindows) {
              this.#initWindow(win);
              for (const tab of win.gBrowser.tabs) {
                try {
                  let parent =
                    tab.linkedBrowser.browsingContext?.currentWindowGlobal.getActor(
                      "CanonicalURL"
                    );

                  parent?.sendAsyncMessage("CanonicalURL:Detect");
                } catch (e) {
                  if (
                    DOMException.isInstance(e) &&
                    e.message.includes("Window protocol")
                  ) {
                  } else {
                    lazy.logConsole.error(e);
                  }
                }
              }
            }
          })
          .then(() => {
            Services.obs.notifyObservers(null, "TabNote:Enabled");
          });

        break;
      case "CanonicalURL:ActorUnregistered":
        lazy.logConsole.debug(
          "CanonicalURL actor unregistered, clearing all tabs"
        );
        this.#deinit()
          .then(() => {
            for (const win of lazy.BrowserWindowTracker.orderedWindows) {
              this.#unloadWindow(win);
              for (const tab of win.gBrowser.tabs) {
                this.#resetTab(tab);
              }
            }
          })
          .then(() => {
            Services.obs.notifyObservers(null, "TabNote:Disabled");
          });
        break;
    }
  }

  onLocationChange(aBrowser, aWebProgress, aRequest, aLocation, aFlags) {
    if (!aWebProgress.isTopLevel) {
      return;
    }

    if (aFlags & Ci.nsIWebProgressListener.LOCATION_CHANGE_SAME_DOCUMENT) {
      if (
        aWebProgress.loadType & Ci.nsIDocShell.LOAD_CMD_RELOAD ||
        aWebProgress.loadType & Ci.nsIDocShell.LOAD_CMD_HISTORY
      ) {
        lazy.logConsole.debug(
          "reload/history navigation, waiting for pageshow or popstate",
          aLocation.spec
        );
        return;
      }

      if (aFlags & Ci.nsIWebProgressListener.LOCATION_CHANGE_HASHCHANGE) {
        lazy.logConsole.debug("fragment identifier changed", aLocation.spec);
        return;
      }

      if (aWebProgress.loadType & Ci.nsIDocShell.LOAD_CMD_PUSHSTATE) {
        let parent =
          aBrowser.browsingContext?.currentWindowGlobal.getExistingActor(
            "CanonicalURL"
          );

        if (parent) {
          parent.sendAsyncMessage("CanonicalURL:DetectFromPushState", {
            pushStateUrl: aLocation.spec,
          });
          lazy.logConsole.debug(
            "requesting CanonicalURL:DetectFromPushState due to history.pushState",
            aLocation.spec
          );
        }

        return;
      }

      return;
    }

    if (aFlags & Ci.nsIWebProgressListener.LOCATION_CHANGE_SESSION_STORE) {
      lazy.logConsole.debug("preserving tab note state during session restore");
      return;
    }

    const tab = aBrowser.documentGlobal.gBrowser.getTabForBrowser(aBrowser);
    this.#resetTab(tab);
    lazy.logConsole.debug("clear tab note due to location change", tab);
  }

  #resetTab(tab) {
    delete tab.canonicalUrl;
    tab.hasTabNote = false;
  }
}

export const TabNotesController = new TabNotesControllerClass();
