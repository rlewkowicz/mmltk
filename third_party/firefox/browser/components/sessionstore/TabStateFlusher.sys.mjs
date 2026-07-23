/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

export var TabStateFlusher = Object.freeze({
  flush(browser) {
    return TabStateFlusherInternal.flush(browser);
  },

  flushWindow(window) {
    return TabStateFlusherInternal.flushWindow(window);
  },

  resolveAll(browser, success = true, message = "") {
    TabStateFlusherInternal.resolveAll(browser, success, message);
  },
});

var TabStateFlusherInternal = {
  _requests: new WeakMap(),

  initEntry(entry) {
    entry.cancelPromise = new Promise(resolve => {
      entry.cancel = resolve;
    }).then(result => {
      TabStateFlusherInternal.initEntry(entry);
      return result;
    });

    return entry;
  },

  flush(browser) {
    let nativePromise = Promise.resolve();
    if (browser && browser.frameLoader) {
      nativePromise = browser.frameLoader.requestTabStateFlush();
    }

    let permanentKey = browser.permanentKey;
    let request = this._requests.get(permanentKey);
    if (!request) {
      request = this.initEntry({});
      this._requests.set(permanentKey, request);
    }

    return Promise.race([nativePromise, request.cancelPromise]);
  },

  flushWindow(window) {
    let promises = [];
    for (let browser of window.gBrowser.browsers) {
      if (window.gBrowser.getTabForBrowser(browser).linkedPanel) {
        promises.push(this.flush(browser));
      }
    }
    return Promise.all(promises);
  },

  resolveAll(browser, success = true, message = "") {
    if (!this._requests.has(browser.permanentKey)) {
      return;
    }

    let { cancel } = this._requests.get(browser.permanentKey);

    if (!success) {
      console.error("Failed to flush browser: ", message);
    }

    cancel(success);
  },
};
