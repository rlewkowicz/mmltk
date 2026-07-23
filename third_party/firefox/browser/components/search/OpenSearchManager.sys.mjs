/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
});


class _OpenSearchManager {
  #offeredEngines = new WeakMap();

  #hiddenEngines = new WeakMap();

  constructor() {
    Services.obs.addObserver(this, "browser-search-engine-modified");
  }

  observe(subject, _topic, data) {
    let engine = subject.wrappedJSObject;
    switch (data) {
      case "engine-added":
        this.#removeMaybeOfferedEngine(engine.name);
        break;
      case "engine-removed":
        this.#addMaybeOfferedEngine(engine.name);
        break;
    }
  }

  addEngine(browser, engine) {
    if (!lazy.SearchService.hasSuccessfullyInitialized) {
      return;
    }
    if (this.#offeredEngines.get(browser)?.some(e => e.title == engine.title)) {
      return;
    }

    let shouldBeHidden = !!lazy.SearchService.getEngineByName(engine.title);

    let engines =
      (shouldBeHidden
        ? this.#hiddenEngines.get(browser)
        : this.#offeredEngines.get(browser)) || [];

    engines.push({
      uri: engine.href,
      title: engine.title,
      get icon() {
        return browser.mIconURL;
      },
    });

    if (shouldBeHidden) {
      this.#hiddenEngines.set(browser, engines);
    } else {
      let win = browser.documentGlobal;
      this.#offeredEngines.set(browser, engines);
      if (browser == win.gBrowser.selectedBrowser) {
        this.updateOpenSearchBadge(win);
      }
    }
  }

  updateOpenSearchBadge(win) {
    let engines = this.#offeredEngines.get(win.gBrowser.selectedBrowser);
    for (let urlbar of  (
      win.document.querySelectorAll("moz-urlbar")
    )) {
      if (!urlbar.controller) {
        continue;
      }
      urlbar.addSearchEngineHelper.setEnginesFromBrowser(
        win.gBrowser.selectedBrowser,
        engines || []
      );
    }

    let searchBar = win.document.getElementById("searchbar");
    if (!searchBar) {
      return;
    }

    if (engines && engines.length) {
      searchBar.setAttribute("addengines", "true");
    } else {
      searchBar.removeAttribute("addengines");
    }
  }

  #addMaybeOfferedEngine(engineName) {
    for (let win of lazy.BrowserWindowTracker.orderedWindows) {
      for (let browser of win.gBrowser.browsers) {
        let hiddenEngines = this.#hiddenEngines.get(browser) || [];
        let offeredEngines = this.#offeredEngines.get(browser) || [];

        for (let i = 0; i < hiddenEngines.length; i++) {
          if (hiddenEngines[i].title == engineName) {
            offeredEngines.push(hiddenEngines[i]);
            if (offeredEngines.length == 1) {
              this.#offeredEngines.set(browser, offeredEngines);
            }

            hiddenEngines.splice(i, 1);
            if (browser == win.gBrowser.selectedBrowser) {
              this.updateOpenSearchBadge(win);
            }
            break;
          }
        }
      }
    }
  }

  #removeMaybeOfferedEngine(engineName) {
    for (let win of lazy.BrowserWindowTracker.orderedWindows) {
      for (let browser of win.gBrowser.browsers) {
        let hiddenEngines = this.#hiddenEngines.get(browser) || [];
        let offeredEngines = this.#offeredEngines.get(browser) || [];

        for (let i = 0; i < offeredEngines.length; i++) {
          if (offeredEngines[i].title == engineName) {
            hiddenEngines.push(offeredEngines[i]);
            if (hiddenEngines.length == 1) {
              this.#hiddenEngines.set(browser, hiddenEngines);
            }

            offeredEngines.splice(i, 1);
            if (browser == win.gBrowser.selectedBrowser) {
              this.updateOpenSearchBadge(win);
            }
            break;
          }
        }
      }
    }
  }

  getEngines(browser) {
    return this.#offeredEngines.get(browser) || [];
  }

  clearEngines(browser) {
    this.#offeredEngines.delete(browser);
    this.#hiddenEngines.delete(browser);
  }
}

export const OpenSearchManager = new _OpenSearchManager();
