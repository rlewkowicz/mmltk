/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

import { UnitConverterSimple } from "moz-src:///browser/components/urlbar/unitconverters/UnitConverterSimple.sys.mjs";
import { UnitConverterTemperature } from "moz-src:///browser/components/urlbar/unitconverters/UnitConverterTemperature.sys.mjs";
import { UnitConverterTimezone } from "moz-src:///browser/components/urlbar/unitconverters/UnitConverterTimezone.sys.mjs";
import {
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "ClipboardHelper",
  "@mozilla.org/widget/clipboardhelper;1",
  Ci.nsIClipboardHelper
);

const CONVERTERS = [
  new UnitConverterSimple(),
  new UnitConverterTemperature(),
  new UnitConverterTimezone(),
];

const DYNAMIC_RESULT_TYPE = "unitConversion";
const VIEW_TEMPLATE = {
  attributes: {
    selectable: true,
  },
  children: [
    {
      name: "content",
      tag: "span",
      classList: ["urlbarView-no-wrap"],
      children: [
        {
          name: "icon",
          tag: "img",
          classList: ["urlbarView-favicon"],
          attributes: {
            src: "chrome://global/skin/icons/edit-copy.svg",
          },
        },
        {
          name: "output",
          tag: "strong",
          attributes: {
            dir: "ltr",
          },
        },
        {
          name: "action",
          tag: "span",
        },
      ],
    },
  ],
};

export class UrlbarProviderUnitConversion extends UrlbarProvider {
  constructor() {
    super();
  }

  get type() {
    return UrlbarUtils.PROVIDER_TYPE.PROFILE;
  }

  async isActive({ searchString }) {
    if (!lazy.UrlbarPrefs.get("unitConversion.enabled")) {
      return false;
    }

    for (const converter of CONVERTERS) {
      const result = converter.convert(searchString);
      if (result) {
        this._activeResult = result;
        return true;
      }
    }

    this._activeResult = null;
    return false;
  }

  getViewTemplate(_result) {
    return VIEW_TEMPLATE;
  }

  getViewUpdate(result) {
    return {
      output: {
        textContent: result.payload.output,
      },
      action: {
        l10n: { id: "urlbar-result-action-copy-to-clipboard" },
      },
    };
  }

  startQuery(queryContext, addCallback) {
    const result = new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.DYNAMIC,
      source: lazy.UrlbarShared.RESULT_SOURCE.OTHER_LOCAL,
      suggestedIndex: lazy.UrlbarPrefs.get("unitConversion.suggestedIndex"),
      payload: {
        dynamicType: DYNAMIC_RESULT_TYPE,
        output: this._activeResult,
        input: queryContext.searchString,
      },
    });
    addCallback(this, result);
  }

  onEngagement(queryContext, controller, details) {
    let { element } = details;
    const { textContent } = element.querySelector(
      ".urlbarView-dynamic-unitConversion-output"
    );
    lazy.ClipboardHelper.copyString(textContent);
  }
}
