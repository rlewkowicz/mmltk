/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import {
  ActionsProvider,
  ActionsResult,
} from "moz-src:///browser/components/urlbar/ActionsProvider.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  QuickActionsLoaderDefault:
    "moz-src:///browser/components/urlbar/QuickActionsLoaderDefault.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
});

const ENABLED_PREF = "suggest.quickactions";
const MATCH_IN_PHRASE_PREF = "quickactions.matchInPhrase";
const MIN_SEARCH_PREF = "quickactions.minimumSearchString";


class ProviderQuickActions extends ActionsProvider {
  get name() {
    return "ActionsProviderQuickActions";
  }

  isActive(queryContext) {
    return (
      !AppConstants.MOZ_MINIMAL_BROWSER &&
      queryContext.sapName == "urlbar" &&
      lazy.UrlbarPrefs.get(ENABLED_PREF) &&
      !queryContext.restrictInSearchMode() &&
      queryContext.trimmedSearchString.length < 50 &&
      queryContext.trimmedSearchString.length >=
        lazy.UrlbarPrefs.get(MIN_SEARCH_PREF)
    );
  }

  async queryActions(queryContext) {
    let input = queryContext.trimmedLowerCaseSearchString;
    let results = await this.getActions({ input });

    if (lazy.UrlbarPrefs.get(MATCH_IN_PHRASE_PREF)) {
      for (let [keyword, keys] of this.#keywords) {
        if (input.includes(keyword) && keys.length) {
          keys.forEach(key => results.add(key));
        }
      }
    }

    results.forEach(key => {
      const action = this.#actions.get(key);
      if (!(action.isVisible?.() ?? true)) {
        results.delete(key);
      }
    });

    if (!results.size) {
      return null;
    }

    return [...results].map(key => {
      let action = this.#actions.get(key);
      return new ActionsResult({
        providerName: this.name,
        key,
        l10nId: action.label,
        icon: action.icon,
        dataset: {
          action: key,
          inputLength: queryContext.trimmedSearchString.length,
        },
      });
    });
  }

  async getActions({ input, includesExactMatch = false }) {
    await lazy.QuickActionsLoaderDefault.ensureLoaded();

    let results = new Set(this.#prefixes.get(input));

    if (includesExactMatch) {
      let actions = this.#keywords.get(input);
      actions?.forEach(action => results.add(action));
    }

    return results;
  }

  getAction(key) {
    return this.#actions.get(key);
  }

  onPick(queryContext, controller, element) {
    this.pickAction(queryContext, controller, element);
  }

  pickAction(queryContext, controller, element) {
    let action = element.dataset.action;
    let inputLength = Math.min(element.dataset.inputLength, 10);
    let options = this.#actions.get(action).onPick(queryContext, controller);
    if (options?.focusContent) {
      controller.browserWindow.gBrowser.selectedBrowser.focus();
    }
  }

  addAction(key, definition) {
    this.#actions.set(key, definition);
    definition.commands.forEach(cmd => {
      let keys = this.#keywords.get(cmd) ?? [];
      keys.push(key);
      this.#keywords.set(cmd, keys);
    });
    this.#loopOverPrefixes(definition.commands, prefix => {
      let result = this.#prefixes.get(prefix);
      if (result) {
        result.add(key);
      } else {
        result = new Set([key]);
      }
      this.#prefixes.set(prefix, result);
    });
  }

  removeAction(key) {
    let definition = this.#actions.get(key);
    this.#actions.delete(key);
    definition.commands.forEach(cmd => {
      let keys = this.#keywords.get(cmd) ?? [];
      this.#keywords.set(
        cmd,
        keys.filter(k => k != key)
      );
    });
    this.#loopOverPrefixes(definition.commands, prefix => {
      let result = this.#prefixes.get(prefix);
      if (result) {
        result.delete(key);
      }
      this.#prefixes.set(prefix, result);
    });
  }

  #keywords = new Map();

  #prefixes = new Map();

  #actions = new Map();

  #loopOverPrefixes(commands, fun) {
    for (const command of commands) {
      for (let i = 1; i <= command.length; i++) {
        let prefix = command.substring(0, command.length - i);
        fun(prefix);
      }
    }
  }
}

export var ActionsProviderQuickActions = new ProviderQuickActions();
