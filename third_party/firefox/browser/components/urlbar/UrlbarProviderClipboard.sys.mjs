/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import {
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  UrlUtils: "resource://gre/modules/UrlUtils.sys.mjs",
});

const RESULT_MENU_COMMANDS = {
  DISMISS: "dismiss",
};
export const CLIPBOARD_IMPRESSION_LIMIT = 2;

export class UrlbarProviderClipboard extends UrlbarProvider {
  #previousClipboard = {
    value: "",
    impressionsLeft: CLIPBOARD_IMPRESSION_LIMIT,
  };

  constructor() {
    super();
  }

  get type() {
    return UrlbarUtils.PROVIDER_TYPE.PROFILE;
  }

  setPreviousClipboardValue(newValue) {
    this.#previousClipboard.value = newValue;
  }

  async isActive(queryContext, controller) {
    if (
      !lazy.UrlbarPrefs.get("clipboard.featureGate") ||
      !lazy.UrlbarPrefs.get("suggest.clipboard") ||
      queryContext.searchString ||
      queryContext.restrictInSearchMode()
    ) {
      return false;
    }
    let textFromClipboard = controller.browserWindow.readFromClipboard();

    if (
      !textFromClipboard ||
      textFromClipboard.length > 2048 ||
      lazy.UrlUtils.REGEXP_SPACES.test(textFromClipboard)
    ) {
      return false;
    }
    textFromClipboard =
      UrlbarUtils.sanitizeTextFromClipboard(textFromClipboard);
    const validUrl = this.#validUrl(textFromClipboard);
    if (!validUrl) {
      return false;
    }

    if (this.#previousClipboard.value === validUrl) {
      if (this.#previousClipboard.impressionsLeft <= 0) {
        return false;
      }
    } else {
      this.#previousClipboard = {
        value: validUrl,
        impressionsLeft: CLIPBOARD_IMPRESSION_LIMIT,
      };
    }

    return true;
  }

  #validUrl(clipboardVal) {
    let givenUrl = URL.parse(clipboardVal);
    if (!givenUrl) {
      return null;
    }

    if (givenUrl.protocol == "http:" || givenUrl.protocol == "https:") {
      return givenUrl.href;
    }

    return null;
  }

  getPriority() {
    return 1;
  }

  async startQuery(queryContext, addCallback) {
    let result = new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.URL,
      source: lazy.UrlbarShared.RESULT_SOURCE.OTHER_LOCAL,
      payload: {
        title: UrlbarUtils.prepareUrlForDisplay(this.#previousClipboard.value, {
          trimURL: false,
        }),
        url: this.#previousClipboard.value,
        icon: "chrome://global/skin/icons/clipboard.svg",
        isBlockable: true,
      },
    });

    addCallback(this, result);
  }

  onEngagement(queryContext, controller, details) {
    this.#previousClipboard.impressionsLeft = 0; 
    this.#handlePossibleCommand(
      controller.view,
      details.result,
      details.selType
    );
  }

  onImpression() {
    this.#previousClipboard.impressionsLeft--;
  }

  #handlePossibleCommand(view, result, selType) {
    switch (selType) {
      case RESULT_MENU_COMMANDS.DISMISS:
        view.controller.removeResult(result);
        this.#previousClipboard.impressionsLeft = 0;
        break;
    }
  }
}
