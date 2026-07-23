/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PrivacyFilter: "resource://gre/modules/sessionstore/PrivacyFilter.sys.mjs",
  TabAttributes: "resource:///modules/sessionstore/TabAttributes.sys.mjs",
  TabStateCache: "resource:///modules/sessionstore/TabStateCache.sys.mjs",
  sessionStoreLogger: "resource:///modules/sessionstore/SessionLogger.sys.mjs",
});

class _TabState {
  update(permanentKey, { data }) {
    lazy.TabStateCache.update(permanentKey, data);
  }

  collect(tab, extData) {
    return this.#collectBaseTabData(tab, { extData });
  }

  clone(tab, extData) {
    return this.#collectBaseTabData(tab, { extData, includePrivateData: true });
  }

  #collectBaseTabData(tab, options) {
    let tabData = { entries: [], lastAccessed: tab.lastAccessed };
    let browser = tab.linkedBrowser;

    if (tab.pinned) {
      tabData.pinned = true;
    }

    tabData.hidden = tab.hidden;

    if (browser.audioMuted) {
      tabData.muted = true;
      tabData.muteReason = tab.muteReason;
    }

    if (tab.group) {
      tabData.groupId = tab.group.id;
    }

    if (tab.splitview) {
      tabData.splitViewId = tab.splitview.splitViewId;
    }

    if (tab.canonicalUrl) {
      let canonicalUrl = tab.canonicalUrl;
      if (!options.includePrivateData) {
        canonicalUrl = lazy.PrivacyFilter.filterCanonicalUrl(canonicalUrl);
      }
      if (canonicalUrl) {
        tabData.canonicalUrl = canonicalUrl;
      }
    }

    tabData.searchMode = tab.documentGlobal.gURLBar.getSearchMode(
      browser,
      true
    );

    tabData.userContextId = tab.userContextId || 0;

    tabData.attributes = lazy.TabAttributes.get(tab);

    if (options.extData) {
      tabData.extData = options.extData;
    }

    this.copyFromCache(browser.permanentKey, tabData, options);


    if (!("image" in tabData)) {
      let tabbrowser = tab.documentGlobal.gBrowser;
      tabData.image = tabbrowser.getIcon(tab);
    }

    if (!("userTypedValue" in tabData) && browser.userTypedValue) {
      tabData.userTypedValue = browser.userTypedValue;
      tabData.userTypedClear = browser.didStartLoadSinceLastUserTyping()
        ? 1
        : 0;
    }

    return tabData;
  }

  processAboutRestartrequiredEnties(aEntries) {
    if (
      !aEntries.some(e => e.url && e.url.startsWith("about:restartrequired"))
    ) {
      return aEntries;
    }

    let newEntries = structuredClone(aEntries);
    newEntries.forEach((item, index, object) => {
      if (item.url === "about:restartrequired") {
        object.splice(index, 1);
      } else if (item.url.startsWith("about:restartrequired")) {
        try {
          const parsedURL = new URL(item.url);
          if (parsedURL && parsedURL.searchParams.has("u")) {
            const previousURL = parsedURL.searchParams.get("u");
            object[index].url = previousURL;
          }
        } catch (ex) {
          lazy.sessionStoreLogger.error(
            `Exception when parsing "${item.url}"`,
            ex
          );
        }
      }
    });

    return newEntries;
  }

  copyFromCache(permanentKey, tabData, options = {}) {
    let data = lazy.TabStateCache.get(permanentKey);
    if (!data) {
      return;
    }

    let includePrivateData = options && options.includePrivateData;

    for (let key of Object.keys(data)) {
      let value = data[key];

      if (!includePrivateData) {
        if (key === "storage") {
          value = lazy.PrivacyFilter.filterSessionStorageData(value);
        } else if (key === "formdata") {
          value = lazy.PrivacyFilter.filterFormData(value);
        }
      }

      if (key === "history") {
        tabData.entries = [...value.entries];

        if (value.hasOwnProperty("index")) {
          tabData.index = value.index;
        }

        if (value.hasOwnProperty("requestedIndex")) {
          tabData.requestedIndex = value.requestedIndex;
        }

        tabData.entries = this.processAboutRestartrequiredEnties(value.entries);
      } else if (!value && (key == "scroll" || key == "formdata")) {

        delete tabData[key];
      } else {
        tabData[key] = value;
      }
    }
  }
}

export const TabState = new _TabState();
