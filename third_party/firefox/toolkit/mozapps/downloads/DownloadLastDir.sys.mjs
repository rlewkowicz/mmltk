/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const LAST_DIR_PREF = "browser.download.lastDir";
const SAVE_PER_SITE_PREF = LAST_DIR_PREF + ".savePerSite";
const nsIFile = Ci.nsIFile;

import { PrivateBrowsingUtils } from "resource://gre/modules/PrivateBrowsingUtils.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};
XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "cps2",
  "@mozilla.org/content-pref/service;1",
  Ci.nsIContentPrefService2
);

let nonPrivateLoadContext = Cu.createLoadContext();
let privateLoadContext = Cu.createPrivateLoadContext();

var observer = {
  QueryInterface: ChromeUtils.generateQI(["nsIObserver"]),

  observe(aSubject, aTopic) {
    switch (aTopic) {
      case "last-pb-context-exited":
        gDownloadLastDirFile = null;
        break;
      case "browser:purge-session-history": {
        gDownloadLastDirFile = null;
        if (Services.prefs.prefHasUserValue(LAST_DIR_PREF)) {
          Services.prefs.clearUserPref(LAST_DIR_PREF);
        }
        let promise = new Promise(resolve =>
          lazy.cps2.removeByName(LAST_DIR_PREF, null, {
            handleCompletion: resolve,
          })
        );
        if (aSubject && typeof subject == "object") {
          aSubject.promise = promise;
        }
        break;
      }
    }
  },
};

Services.obs.addObserver(observer, "last-pb-context-exited");
Services.obs.addObserver(observer, "browser:purge-session-history");

function readLastDirPref() {
  try {
    return Services.prefs.getComplexValue(LAST_DIR_PREF, nsIFile);
  } catch (e) {
    return null;
  }
}

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "isContentPrefEnabled",
  SAVE_PER_SITE_PREF,
  true
);

var gDownloadLastDirFile = readLastDirPref();

export class DownloadLastDir {
  constructor(aWindow, aForcePrivate) {
    let isPrivate = false;
    if (aWindow === null) {
      isPrivate =
        aForcePrivate || PrivateBrowsingUtils.permanentPrivateBrowsing;
    } else {
      let loadContext = aWindow.docShell.QueryInterface(Ci.nsILoadContext);
      isPrivate = loadContext.usePrivateBrowsing;
    }

    this.fakeContext = isPrivate ? privateLoadContext : nonPrivateLoadContext;
  }

  isPrivate() {
    return this.fakeContext.usePrivateBrowsing;
  }

  get file() {
    return this.#getLastFile();
  }
  set file(val) {
    this.setFile(null, val);
  }

  cleanupPrivateFile() {
    gDownloadLastDirFile = null;
  }

  #getLastFile() {
    if (gDownloadLastDirFile && !gDownloadLastDirFile.exists()) {
      gDownloadLastDirFile = null;
    }

    if (this.isPrivate()) {
      if (!gDownloadLastDirFile) {
        gDownloadLastDirFile = readLastDirPref();
      }
      return gDownloadLastDirFile;
    }
    return readLastDirPref();
  }

  async getFileAsync(aURI) {
    let plainPrefFile = this.#getLastFile();
    if (!aURI || !lazy.isContentPrefEnabled) {
      return plainPrefFile;
    }

    return new Promise(resolve => {
      lazy.cps2.getByDomainAndName(
        this.#cpsGroupFromURL(aURI),
        LAST_DIR_PREF,
        this.fakeContext,
        {
          _result: null,
          handleResult(aResult) {
            this._result = aResult;
          },
          handleCompletion(aReason) {
            let file = plainPrefFile;
            if (
              aReason == Ci.nsIContentPrefCallback2.COMPLETE_OK &&
              this._result instanceof Ci.nsIContentPref
            ) {
              try {
                file = Cc["@mozilla.org/file/local;1"].createInstance(
                  Ci.nsIFile
                );
                file.initWithPath(this._result.value);
              } catch (e) {
                file = plainPrefFile;
              }
            }
            resolve(file);
          },
        }
      );
    });
  }

  setFile(aURI, aFile) {
    if (aURI && lazy.isContentPrefEnabled) {
      if (aFile instanceof Ci.nsIFile) {
        lazy.cps2.set(
          this.#cpsGroupFromURL(aURI),
          LAST_DIR_PREF,
          aFile.path,
          this.fakeContext
        );
      } else {
        lazy.cps2.removeByDomainAndName(
          this.#cpsGroupFromURL(aURI),
          LAST_DIR_PREF,
          this.fakeContext
        );
      }
    }
    if (this.isPrivate()) {
      if (aFile instanceof Ci.nsIFile) {
        gDownloadLastDirFile = aFile.clone();
      } else {
        gDownloadLastDirFile = null;
      }
    } else if (aFile instanceof Ci.nsIFile) {
      Services.prefs.setComplexValue(LAST_DIR_PREF, nsIFile, aFile);
    } else if (Services.prefs.prefHasUserValue(LAST_DIR_PREF)) {
      Services.prefs.clearUserPref(LAST_DIR_PREF);
    }
  }

  #cpsGroupFromURL(url) {
    if (typeof url == "string") {
      url = new URL(url);
    } else if (url instanceof Ci.nsIURI) {
      url = URL.fromURI(url);
    }
    if (!URL.isInstance(url)) {
      return url;
    }
    if (url.protocol == "data:") {
      return url.href.match(/^data:[^;,]*/i)[0].replace(/:$/, ":text/plain");
    }
    if (url.protocol == "file:") {
      return "file:///";
    }
    return url.href;
  }
}
