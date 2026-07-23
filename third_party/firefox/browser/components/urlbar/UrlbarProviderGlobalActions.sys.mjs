/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import {
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

const DEFAULT_ICON = "chrome://global/skin/icons/settings.svg";
const DYNAMIC_TYPE_NAME = "actions";

const SUGGESTED_INDEX = 1;
const SUGGESTED_INDEX_TABS_MODE = 0;

const SCOTCH_BONNET_PREF = "scotchBonnet.enableOverride";
const ACTIONS_PREF = "secondaryActions.featureGate";
const QUICK_ACTIONS_PREF = "suggest.quickactions";
const MAX_ACTIONS_PREF = "secondaryActions.maxActionsShown";

const TIMES_TO_SHOW_PREF = "quickactions.timesToShowOnboardingLabel";
const TIMES_SHOWN_PREF = "quickactions.timesShownOnboardingLabel";

ChromeUtils.defineESModuleGetters(lazy, {
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

import { ActionsProviderQuickActions } from "moz-src:///browser/components/urlbar/ActionsProviderQuickActions.sys.mjs";
import { ActionsProviderTabGroups } from "moz-src:///browser/components/urlbar/ActionsProviderTabGroups.sys.mjs";

let globalActionsProviders = [
  ActionsProviderQuickActions,
  ActionsProviderTabGroups,
];
if (AppConstants.MOZ_PLACES) {
  let { ActionsProviderContextualSearch } = ChromeUtils.importESModule(
    "moz-src:///browser/components/urlbar/ActionsProviderContextualSearch.sys.mjs"
  );
  globalActionsProviders.unshift(ActionsProviderContextualSearch);
}

export class UrlbarProviderGlobalActions extends UrlbarProvider {
  get type() {
    return UrlbarUtils.PROVIDER_TYPE.PROFILE;
  }

  async isActive(queryContext) {
    return (
      (lazy.UrlbarPrefs.get(SCOTCH_BONNET_PREF) ||
        lazy.UrlbarPrefs.get(ACTIONS_PREF) ||
        queryContext.sapName == "searchbar") &&
      lazy.UrlbarPrefs.get(QUICK_ACTIONS_PREF)
    );
  }

  async startQuery(queryContext, addCallback) {
    let actionsResults = [];

    for (let provider of globalActionsProviders) {
      if (provider.isActive(queryContext)) {
        actionsResults.push(
          ...((await provider.queryActions(queryContext)) || [])
        );
      }
    }

    if (!actionsResults.length) {
      return;
    }

    if (actionsResults.length > lazy.UrlbarPrefs.get(MAX_ACTIONS_PREF)) {
      actionsResults.length = lazy.UrlbarPrefs.get(MAX_ACTIONS_PREF);
    }

    let showOnboardingLabel =
      lazy.UrlbarPrefs.get(TIMES_TO_SHOW_PREF) >
      lazy.UrlbarPrefs.get(TIMES_SHOWN_PREF);

    let query = actionsResults.some(a => a.key == "matched-contextual-search")
      ? ""
      : queryContext.searchString;

    let payload = {
      actionsResults,
      dynamicType: DYNAMIC_TYPE_NAME,
      inputLength: queryContext.searchString.length,
      input: query,
      showOnboardingLabel,
      query,
    };

    let result = new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.DYNAMIC,
      source: lazy.UrlbarShared.RESULT_SOURCE.ACTIONS,
      suggestedIndex:
        queryContext.restrictSource == lazy.UrlbarShared.RESULT_SOURCE.TABS
          ? SUGGESTED_INDEX_TABS_MODE
          : SUGGESTED_INDEX,
      payload,
    });
    addCallback(this, result);
  }

  async onEngagement(queryContext, controller, details) {
    let key = details.element.dataset.action;
    let action = details.result.payload.actionsResults.find(a => a.key == key);
    let provider = globalActionsProviders.find(
      p => p.name == action.providerName
    );
    provider.onPick(queryContext, controller, action);
  }

  onSearchSessionEnd(queryContext, controller, details) {
    let showOnboardingLabel = queryContext.results?.find(
      r => r.providerName == this.name
    )?.payload.showOnboardingLabel;
    if (showOnboardingLabel) {
      lazy.UrlbarPrefs.set(
        TIMES_SHOWN_PREF,
        lazy.UrlbarPrefs.get(TIMES_SHOWN_PREF) + 1
      );
    }
    for (let provider of globalActionsProviders) {
      provider.onSearchSessionEnd?.(queryContext, controller, details);
    }
  }

  getViewTemplate(result) {
    let children = result.payload.actionsResults.map((action, i) => {
      let btn = {
        name: `button-${i}`,
        tag: "span",
        classList: ["urlbarView-action-btn"],
        attributes: {
          inputLength: result.payload.inputLength,
          "data-action": action.key,
          role: "button",
        },
        children: [
          {
            tag: "img",
            attributes: {
              src: action.icon || DEFAULT_ICON,
            },
          },
          {
            name: `label-${i}`,
            tag: "span",
            classList: ["urlbarView-action-btn-label"],
          },
        ],
      };

      if (action.dataset?.style) {
        let style = "";
        for (let [prop, val] of Object.entries(action.dataset.style)) {
          style += `${prop}: ${val};`;
        }
        btn.attributes.style = style;
      }

      if (action.dataset?.providesSearchMode) {
        btn.attributes["data-provides-searchmode"] = "true";
        btn.attributes["data-engine"] = action.engine;
      }

      if (action.dataset?.immediateSearch) {
        btn.attributes["data-immediate-search"] = "true";
      }

      return btn;
    });

    if (result.payload.showOnboardingLabel) {
      children.unshift({
        name: "press-tab-label",
        tag: "span",
        classList: ["urlbarView-press-tab-label"],
      });
    }

    return { children };
  }

  getViewUpdate(result) {
    let viewUpdate = {};
    if (result.payload.showOnboardingLabel) {
      viewUpdate["press-tab-label"] = {
        l10n: { id: "press-tab-label" },
      };
    }
    result.payload.actionsResults.forEach((action, i) => {
      viewUpdate[`label-${i}`] = {
        l10n: { id: action.l10nId, args: action.l10nArgs },
      };
    });
    return viewUpdate;
  }
}
