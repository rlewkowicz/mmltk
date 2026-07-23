/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import {
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";

const lazy = {};

const DEFAULT_ICON = "chrome://global/skin/icons/settings.svg";
const DYNAMIC_TYPE_NAME = "actions";

ChromeUtils.defineESModuleGetters(lazy, {
  ActionsProviderQuickActions:
    "moz-src:///browser/components/urlbar/ActionsProviderQuickActions.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

export class UrlbarProviderActionsSearchMode extends UrlbarProvider {
  get type() {
    return UrlbarUtils.PROVIDER_TYPE.PROFILE;
  }

  async isActive(queryContext) {
    return (
      queryContext.searchMode?.source == lazy.UrlbarShared.RESULT_SOURCE.ACTIONS
    );
  }

  async startQuery(queryContext, addCallback) {
    let input = queryContext.trimmedLowerCaseSearchString;
    let results = await lazy.ActionsProviderQuickActions.getActions({
      input,
      includesExactMatch: true,
    });
    results.forEach(resultKey => {
      let result = new lazy.UrlbarResult({
        type: lazy.UrlbarShared.RESULT_TYPE.DYNAMIC,
        source: lazy.UrlbarShared.RESULT_SOURCE.ACTIONS,
        payload: {
          key: resultKey,
          dynamicType: DYNAMIC_TYPE_NAME,
          inputLength: queryContext.trimmedLowerCaseSearchString.length,
        },
      });
      addCallback(this, result);
    });
  }

  onEngagement(queryContext, controller, details) {
    if (details.element.hasAttribute("disabled")) {
      return;
    }
    lazy.ActionsProviderQuickActions.pickAction(
      queryContext,
      controller,
      details.element,
      details.element.documentGlobal
    );
  }

  getViewTemplate(result) {
    let action = lazy.ActionsProviderQuickActions.getAction(result.payload.key);
    let inActive =
      ("isActive" in action && !action.isActive()) ||
      !(action.isVisible?.() ?? true);
    return {
      children: [
        {
          tag: "span",
          classList: ["urlbarView-action-btn"],
          attributes: {
            "data-action": result.payload.key,
            "data-input-length": result.payload.inputLength,
            role: "button",
            disabled: inActive,
          },
          children: [
            {
              tag: "img",
              attributes: {
                src: action.icon || DEFAULT_ICON,
              },
            },
            {
              name: `label`,
              tag: "span",
            },
          ],
        },
      ],
    };
  }

  getViewUpdate(result) {
    let action = lazy.ActionsProviderQuickActions.getAction(result.payload.key);

    return {
      label: {
        l10n: { id: action.label },
      },
    };
  }
}
