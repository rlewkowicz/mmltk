/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import {
  Directive,
  noChange,
  nothing,
  directive,
} from "chrome://global/content/vendor/lit.all.mjs";

import { MozLitElement } from "chrome://global/content/lit-utils.mjs";

const { XPCOMUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
);

const lazy = XPCOMUtils.declareLazy({
  srdEnabled: { pref: "browser.settings-redesign.enabled" },
});

function expandPaneName(category) {
  return category
    ? `pane${category[0].toUpperCase()}${category.slice(1)}`
    : undefined;
}



class SpreadDirective extends Directive {
  #prevProps = {};

  // eslint-disable-next-line no-unused-vars
  render(props) {
    return nothing;
  }

  update(part, [props]) {

    let el = part.element;

    for (let [key, value] of Object.entries(props)) {
      if (value === this.#prevProps[key]) {
        continue;
      }

      if (key.startsWith("?")) {
        el.toggleAttribute(key.slice(1), Boolean(value));
      } else if (key.startsWith(".")) {
        // @ts-ignore
        el[key.slice(1)] = value;
      } else if (key.startsWith("@")) {
        throw new Error(
          `Event listeners are not yet supported with spread (${key})`
        );
      } else {
        el.setAttribute(key, String(value));
      }
    }

    this.#prevProps = props;

    return noChange;
  }
}

export const spread = directive(SpreadDirective);

const HEADING_LEVEL_KEYS = ["headinglevel", "headingLevel", ".headingLevel"];

export function bumpHeadingLevelForSrd(level, srdEnabled) {
  if (!srdEnabled || typeof level !== "number") {
    return level;
  }
  return Math.max(level + 1, 3);
}

export class SettingElement extends MozLitElement {
  getCommonPropertyMapping(config) {
    let controlAttrs = { ...(config.controlAttrs ?? {}) };
    if (lazy.srdEnabled) {
      for (let key of HEADING_LEVEL_KEYS) {
        if (typeof controlAttrs[key] === "number") {
          controlAttrs[key] = bumpHeadingLevelForSrd(
            controlAttrs[key],
            lazy.srdEnabled
          );
        }
      }
    }
    return {
      id: config.id,
      "data-l10n-id": config.l10nId ? config.l10nId : undefined,
      "data-l10n-args": config.l10nArgs
        ? JSON.stringify(config.l10nArgs)
        : undefined,
      ".iconSrc": config.iconSrc,
      "data-load-pane": expandPaneName(config.loadPane),
      ".supportPage":
        config.supportPage != undefined ? config.supportPage : undefined,
      slot: config.slot,
      ...controlAttrs,
    };
  }
}
