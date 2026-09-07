/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const kEnabledPref = "browser.laterrun.enabled";
const kPagePrefRoot = "browser.laterrun.pages.";
const kSessionCountPref = "browser.laterrun.bookkeeping.sessionCount";
const kProfileCreationTime = "browser.laterrun.bookkeeping.profileCreationTime";
const kUpdateAppliedTime = "browser.laterrun.bookkeeping.updateAppliedTime";

const kSelfDestructSessionLimit = 50;
const kSelfDestructHoursLimit = 31 * 24;

class Page {
  constructor({
    pref,
    minimumHoursSinceInstall,
    minimumSessionCount,
    requireBoth,
    url,
  }) {
    this.pref = pref;
    this.minimumHoursSinceInstall = minimumHoursSinceInstall || 0;
    this.minimumSessionCount = minimumSessionCount || 1;
    this.requireBoth = requireBoth || false;
    this.url = url;
  }

  get hasRun() {
    return Services.prefs.getBoolPref(this.pref + "hasRun", false);
  }

  applies(sessionInfo) {
    if (this.hasRun) {
      return false;
    }
    if (this.requireBoth) {
      return (
        sessionInfo.sessionCount >= this.minimumSessionCount &&
        sessionInfo.hoursSinceInstall >= this.minimumHoursSinceInstall
      );
    }
    return (
      sessionInfo.sessionCount >= this.minimumSessionCount ||
      sessionInfo.hoursSinceInstall >= this.minimumHoursSinceInstall
    );
  }
}

export let LaterRun = {
  get ENABLE_REASON_NEW_PROFILE() {
    return 1;
  },
  get ENABLE_REASON_UPDATE_APPLIED() {
    return 2;
  },

  init(reason) {
    if (!this.enabled) {
      return;
    }

    if (reason == this.ENABLE_REASON_NEW_PROFILE) {
      if (
        Services.prefs.getPrefType(kProfileCreationTime) ==
        Ci.nsIPrefBranch.PREF_INVALID
      ) {
        Services.prefs.setIntPref(
          kProfileCreationTime,
          Math.floor(Date.now() / 1000)
        );
      }
      this.sessionCount++;
    } else if (reason == this.ENABLE_REASON_UPDATE_APPLIED) {
      Services.prefs.setIntPref(
        kUpdateAppliedTime,
        Math.floor(Services.startup.getStartupInfo().start.getTime() / 1000)
      );
    }

    if (
      this.hoursSinceInstall > kSelfDestructHoursLimit ||
      this.sessionCount > kSelfDestructSessionLimit
    ) {
      this.selfDestruct();
    }
  },

  get enabled() {
    return Services.prefs.getBoolPref(kEnabledPref, false);
  },

  enable(reason) {
    if (!this.enabled) {
      Services.prefs.setBoolPref(kEnabledPref, true);
      this.init(reason);
    }
  },

  get hoursSinceInstall() {
    let installStampSec = Services.prefs.getIntPref(
      kProfileCreationTime,
      Date.now() / 1000
    );
    return Math.floor((Date.now() / 1000 - installStampSec) / 3600);
  },

  get hoursSinceUpdate() {
    let updateStampSec = Services.prefs.getIntPref(kUpdateAppliedTime, 0);
    return Math.floor((Date.now() / 1000 - updateStampSec) / 3600);
  },

  get sessionCount() {
    if (this._sessionCount) {
      return this._sessionCount;
    }
    return (this._sessionCount = Services.prefs.getIntPref(
      kSessionCountPref,
      0
    ));
  },

  set sessionCount(val) {
    this._sessionCount = val;
    Services.prefs.setIntPref(kSessionCountPref, val);
  },

  selfDestruct() {
    Services.prefs.setBoolPref(kEnabledPref, false);
  },

  readPages() {
    let allPrefsForPages = Services.prefs.getChildList(kPagePrefRoot);
    let pageDataStore = new Map();
    for (let pref of allPrefsForPages) {
      let [slug, prop] = pref.substring(kPagePrefRoot.length).split(".");
      if (!pageDataStore.has(slug)) {
        pageDataStore.set(slug, {
          pref: pref.substring(0, pref.length - prop.length),
        });
      }
      if (prop == "requireBoth" || prop == "hasRun") {
        pageDataStore.get(slug)[prop] = Services.prefs.getBoolPref(pref, false);
      } else if (prop == "url") {
        pageDataStore.get(slug)[prop] = Services.prefs.getStringPref(pref, "");
      } else {
        pageDataStore.get(slug)[prop] = Services.prefs.getIntPref(pref, 0);
      }
    }
    let rv = [];
    for (let [, pageData] of pageDataStore) {
      if (pageData.url) {
        let urlString = Services.urlFormatter.formatURL(pageData.url.trim());
        let uri = URL.parse(urlString)?.URI;
        if (!uri) {
          console.error(
            "Invalid LaterRun page URL ",
            pageData.url,
            " ignored."
          );
          continue;
        }
        if (!uri.schemeIs("https")) {
          console.error("Insecure LaterRun page URL ", uri.spec, " ignored.");
        } else {
          pageData.url = uri.spec;
          rv.push(new Page(pageData));
        }
      }
    }
    return rv;
  },

  getURL() {
    if (!this.enabled) {
      return null;
    }
    let pages = this.readPages();
    let page = pages.find(p => p.applies(this));
    if (page) {
      Services.prefs.setBoolPref(page.pref + "hasRun", true);
      return page.url;
    }
    return null;
  },
};

LaterRun.init();
