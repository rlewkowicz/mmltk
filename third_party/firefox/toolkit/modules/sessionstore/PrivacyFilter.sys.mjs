/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PrivacyLevel: "resource://gre/modules/sessionstore/PrivacyLevel.sys.mjs",
});

export var PrivacyFilter = Object.freeze({
  filterCanonicalUrl(url) {
    if (!url) {
      return null;
    }
    return lazy.PrivacyLevel.check(url) ? url : null;
  },

  filterSessionStorageData(data) {
    let retval = {};

    if (lazy.PrivacyLevel.shouldSaveEverything()) {
      return data;
    }

    if (!lazy.PrivacyLevel.canSaveAnything()) {
      return null;
    }

    for (let host of Object.keys(data)) {
      if (lazy.PrivacyLevel.check(host)) {
        retval[host] = data[host];
      }
    }

    return Object.keys(retval).length ? retval : null;
  },

  filterFormData(data) {
    if (lazy.PrivacyLevel.shouldSaveEverything()) {
      return Object.keys(data).length ? data : null;
    }

    if (!lazy.PrivacyLevel.canSaveAnything()) {
      return null;
    }

    if (!data || (data.url && !lazy.PrivacyLevel.check(data.url))) {
      return null;
    }

    let retval = {};

    for (let key of Object.keys(data)) {
      if (key === "children") {
        let recurse = child => this.filterFormData(child);
        let children = data.children.map(recurse).filter(child => child);

        if (children.length) {
          retval.children = children;
        }
      } else if (data.url) {
        retval[key] = data[key];
      }
    }

    return Object.keys(retval).length ? retval : null;
  },

  filterPrivateWindowsAndTabs(browserState) {
    for (let i = browserState.windows.length - 1; i >= 0; i--) {
      let win = browserState.windows[i];

      if (win.isPrivate) {
        browserState.windows.splice(i, 1);

        if (browserState.selectedWindow >= i) {
          browserState.selectedWindow--;
        }
      }
    }

    browserState._closedWindows = browserState._closedWindows.filter(
      win => !win.isPrivate
    );
  },
});
