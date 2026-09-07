/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import {
  ActionsProvider,
  ActionsResult,
} from "moz-src:///browser/components/urlbar/ActionsProvider.sys.mjs";

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  SessionStore: "resource:///modules/sessionstore/SessionStore.sys.mjs",
  TabMetrics: "moz-src:///browser/components/tabbrowser/TabMetrics.sys.mjs",
});

const MIN_SEARCH_PREF = "tabGroups.minSearchLength";

class ProviderTabGroups extends ActionsProvider {
  get name() {
    return "ActionsProviderTabGroups";
  }

  isActive(queryContext) {
    return (
      queryContext.sapName == "urlbar" &&
      Services.prefs.getBoolPref("browser.tabs.groups.enabled") &&
      (!queryContext.restrictSource ||
        queryContext.restrictSource == lazy.UrlbarShared.RESULT_SOURCE.TABS) &&
      queryContext.trimmedSearchString.length < 50 &&
      queryContext.trimmedSearchString.length >=
        lazy.UrlbarPrefs.get(MIN_SEARCH_PREF)
    );
  }

  async queryActions(queryContext) {
    let window = lazy.BrowserWindowTracker.getTopWindow({
      allowFromInactiveWorkspace: true,
    });
    if (!window) {
      if (!false) {
        console.error("Couldn't find a browser window.");
      }
      return null;
    }
    let results = [];
    let i = 0;

    for (let group of window.gBrowser.getAllTabGroups({
      sortByLastSeenActive: true,
    })) {
      if (
        group.documentGlobal == window &&
        window.gBrowser.selectedTab.group == group
      ) {
        continue;
      }
      if (!this.#matches(group.label, queryContext)) {
        continue;
      }

      results.push(
        this.#makeResult({
          key: `tabgroup-${i++}`,
          l10nId: "urlbar-result-action-switch-to-tabgroup",
          l10nArgs: { group: group.label },
          dataset: { groupId: group.id },
          color: group.color,
        })
      );
    }

    if (queryContext.isPrivate) {
      return results;
    }

    for (let savedGroup of lazy.SessionStore.getSavedTabGroups()) {
      if (!this.#matches(savedGroup.name, queryContext)) {
        continue;
      }
      let result = this.#makeResult({
        key: `tabgroup-${i++}`,
        l10nId: "urlbar-result-action-open-saved-tabgroup",
        l10nArgs: { group: savedGroup.name },
        color: savedGroup.color,
        dataset: { savedGroupId: savedGroup.id },
      });
      results.push(result);
    }

    return results;
  }

  onPick(_queryContext, controller, action) {
    let group;
    if (action.dataset.savedGroupId) {
      group = lazy.SessionStore.openSavedTabGroup(
        action.dataset.savedGroupId,
        controller.browserWindow,
        {
          source: lazy.TabMetrics.METRIC_SOURCE.SUGGEST,
        }
      );
    } else {
      group = controller.browserWindow.gBrowser.getTabGroupById(
        action.dataset.groupId
      );
    }

    if (group) {
      group.select();
      group.documentGlobal.focus();
    }
  }

  #matches(groupName, queryContext) {
    groupName = groupName.toLowerCase();
    if (queryContext.trimmedLowerCaseSearchString.length == 1) {
      return groupName.startsWith(queryContext.trimmedLowerCaseSearchString);
    }
    return queryContext.tokens.every(token =>
      groupName.includes(token.lowerCaseValue)
    );
  }

  #makeResult({ key, l10nId, l10nArgs, color, dataset }) {
    return new ActionsResult({
      providerName: this.name,
      key,
      l10nId,
      l10nArgs,
      icon: "chrome://browser/skin/tabbrowser/tab-groups.svg",
      dataset: {
        ...dataset,
        style: {
          "--tab-group-color": `var(--tab-group-${color})`,
          "--tab-group-color-invert": `var(--tab-group-${color}-invert)`,
          "--tab-group-color-pale": `var(--tab-group-${color}-pale)`,
          "--tab-group-background-color": `var(--tab-group-${color})`,
          "--tab-group-text-color": `var(--tab-group-${color}-text)`,
          "--tab-group-background-color-hover": `var(--tab-group-${color}-hover)`,
        },
      },
    });
  }
}

export var ActionsProviderTabGroups = new ProviderTabGroups();
