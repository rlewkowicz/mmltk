/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "PlatformKeys", function () {
  return Services.strings.createBundle(
    "chrome://global-platform/locale/platformKeys.properties"
  );
});

ChromeUtils.defineLazyGetter(lazy, "Keys", function () {
  return Services.strings.createBundle(
    "chrome://global/locale/keys.properties"
  );
});

export var ShortcutUtils = {
  IS_VALID: "valid",
  INVALID_KEY: "invalid_key",
  INVALID_KEY_IN_EXTENSION_MANIFEST: "invalid_key_in_extension_manifest",
  INVALID_MODIFIER: "invalid_modifier",
  INVALID_COMBINATION: "invalid_combination",
  DUPLICATE_MODIFIER: "duplicate_modifier",
  MODIFIER_REQUIRED: "modifier_required",

  CLOSE_TAB: "CLOSE_TAB",
  CYCLE_TABS: "CYCLE_TABS",
  TOGGLE_CARET_BROWSING: "TOGGLE_CARET_BROWSING",
  MOVE_TAB_BACKWARD: "MOVE_TAB_BACKWARD",
  MOVE_TAB_FORWARD: "MOVE_TAB_FORWARD",
  MOVE_TAB_TO_START: "MOVE_TAB_TO_START",
  MOVE_TAB_TO_END: "MOVE_TAB_TO_END",
  NEXT_TAB: "NEXT_TAB",
  PREVIOUS_TAB: "PREVIOUS_TAB",

  prettifyShortcut(aElemKey) {
    let elemString = this.getModifierString(aElemKey.getAttribute("modifiers"));
    let key = this.getKeyString(
      aElemKey.getAttribute("keycode"),
      aElemKey.getAttribute("key")
    );
    if (!key) {
      console.warn(
        "Key element",
        aElemKey,
        'is missing "key" and "keycode" attributes necessary to define a shortcut'
      );
      return "";
    }
    return elemString + key;
  },

  metaKeyIsCommandKey() {
    return AppConstants.platform == "macosx";
  },

  getModifierString(elemMod) {
    if (!elemMod) {
      return "";
    }

    let elemString = "";
    let haveCloverLeaf = false;
    if (elemMod.match("accel")) {
      if (Services.appinfo.OS == "Darwin") {
        haveCloverLeaf = true;
      } else {
        elemString +=
          lazy.PlatformKeys.GetStringFromName("VK_CONTROL") +
          lazy.PlatformKeys.GetStringFromName("MODIFIER_SEPARATOR");
      }
    }
    if (elemMod.match("access")) {
      if (Services.appinfo.OS == "Darwin") {
        elemString +=
          lazy.PlatformKeys.GetStringFromName("VK_CONTROL") +
          lazy.PlatformKeys.GetStringFromName("MODIFIER_SEPARATOR");
      } else {
        elemString +=
          lazy.PlatformKeys.GetStringFromName("VK_ALT") +
          lazy.PlatformKeys.GetStringFromName("MODIFIER_SEPARATOR");
      }
    }
    if (elemMod.match("meta") && !this.metaKeyIsCommandKey()) {
      elemString +=
        lazy.PlatformKeys.GetStringFromName("VK_COMMAND_OR_WIN") +
        lazy.PlatformKeys.GetStringFromName("MODIFIER_SEPARATOR");
    }
    if (elemMod.match("shift")) {
      elemString +=
        lazy.PlatformKeys.GetStringFromName("VK_SHIFT") +
        lazy.PlatformKeys.GetStringFromName("MODIFIER_SEPARATOR");
    }
    if (elemMod.match("alt")) {
      elemString +=
        lazy.PlatformKeys.GetStringFromName("VK_ALT") +
        lazy.PlatformKeys.GetStringFromName("MODIFIER_SEPARATOR");
    }
    if (elemMod.match("ctrl") || elemMod.match("control")) {
      elemString +=
        lazy.PlatformKeys.GetStringFromName("VK_CONTROL") +
        lazy.PlatformKeys.GetStringFromName("MODIFIER_SEPARATOR");
    }
    if (elemMod.match("meta") && this.metaKeyIsCommandKey()) {
      elemString +=
        lazy.PlatformKeys.GetStringFromName("VK_COMMAND_OR_WIN") +
        lazy.PlatformKeys.GetStringFromName("MODIFIER_SEPARATOR");
    }

    if (haveCloverLeaf) {
      elemString +=
        lazy.PlatformKeys.GetStringFromName("VK_COMMAND_OR_WIN") +
        lazy.PlatformKeys.GetStringFromName("MODIFIER_SEPARATOR");
    }

    return elemString;
  },

  getKeyString(keyCode, keyAttribute) {
    let key = "";
    if (keyCode) {
      keyCode = keyCode.toUpperCase();
      if (AppConstants.platform == "macosx") {
        switch (keyCode) {
          case "VK_LEFT":
            return "\u2190"; 
          case "VK_RIGHT":
            return "\u2192"; 
        }
      }
      try {
        let bundle = keyCode == "VK_RETURN" ? lazy.PlatformKeys : lazy.Keys;
        key = bundle.GetStringFromName(keyCode);
      } catch (ex) {
        console.error("Error finding ", keyCode, ": ", ex);
        key = keyCode.replace(/^VK_/, "");
      }
    } else if (keyAttribute) {
      key = keyAttribute.toUpperCase();
    }

    return key;
  },

  getKeyAttribute(chromeKey) {
    if (/^[A-Z]$/.test(chromeKey)) {
      return ["key", chromeKey];
    }
    return ["keycode", this.getKeycodeAttribute(chromeKey)];
  },

  getKeycodeAttribute(chromeKey) {
    if (/^[0-9]/.test(chromeKey)) {
      return `VK_${chromeKey}`;
    }
    return `VK${chromeKey.replace(/([A-Z])/g, "_$&").toUpperCase()}`;
  },

  findShortcut(aElemCommand) {
    let document = aElemCommand.ownerDocument;
    return document.querySelector(
      'key[command="' + aElemCommand.getAttribute("id") + '"]'
    );
  },

  chromeModifierKeyMap: {
    Alt: "alt",
    Command: "accel",
    Ctrl: "accel",
    MacCtrl: "control",
    Shift: "shift",
  },

  getModifiersAttribute(chromeModifiers) {
    return Array.from(chromeModifiers, modifier => {
      return ShortcutUtils.chromeModifierKeyMap[modifier];
    })
      .sort()
      .join(",");
  },

  validate(string, { extensionManifest = false } = {}) {
    const MEDIA_KEYS =
      /^(MediaNextTrack|MediaPlayPause|MediaPrevTrack|MediaStop)$/;
    const BASIC_KEYS =
      /^([A-Z0-9]|Comma|Period|Home|End|PageUp|PageDown|Space|Insert|Delete|Up|Down|Left|Right)$/;
    const FUNCTION_KEYS_BASIC = /^(F[1-9]|F1[0-2])$/;
    const FUNCTION_KEYS_EXTENDED = /^(F[1-9]|F1[0-9])$/;
    const FUNCTION_KEYS = extensionManifest
      ? FUNCTION_KEYS_BASIC
      : FUNCTION_KEYS_EXTENDED;

    if (MEDIA_KEYS.test(string.trim())) {
      return this.IS_VALID;
    }

    let modifiers = string.split("+").map(s => s.trim());
    let key = modifiers.pop();

    let chromeModifiers = modifiers.map(
      m => ShortcutUtils.chromeModifierKeyMap[m]
    );
    if (chromeModifiers.some(modifier => !modifier)) {
      return this.INVALID_MODIFIER;
    }

    if (
      FUNCTION_KEYS === FUNCTION_KEYS_BASIC &&
      !FUNCTION_KEYS_BASIC.test(key) &&
      FUNCTION_KEYS_EXTENDED.test(key)
    ) {
      return this.INVALID_KEY_IN_EXTENSION_MANIFEST;
    }

    switch (modifiers.length) {
      case 0:
        if (!FUNCTION_KEYS.test(key)) {
          return this.MODIFIER_REQUIRED;
        }
        break;
      case 1:
        if (chromeModifiers[0] == "shift" && !FUNCTION_KEYS.test(key)) {
          return this.MODIFIER_REQUIRED;
        }
        break;
      case 2:
        if (chromeModifiers[0] == chromeModifiers[1]) {
          return this.DUPLICATE_MODIFIER;
        }
        break;
      default:
        return this.INVALID_COMBINATION;
    }

    if (!BASIC_KEYS.test(key) && !FUNCTION_KEYS.test(key)) {
      return this.INVALID_KEY;
    }

    return this.IS_VALID;
  },

  isSystem(win, value) {
    let modifiers = value.split("+");
    let chromeKey = modifiers.pop();
    let modifiersString = this.getModifiersAttribute(modifiers);
    let keycode = this.getKeycodeAttribute(chromeKey);

    let baseSelector = "key";
    if (modifiers.length) {
      baseSelector += `[modifiers="${modifiersString}"]`;
    }

    let keyEl = win.document.querySelector(
      [
        `${baseSelector}[key="${chromeKey}"]`,
        `${baseSelector}[key="${chromeKey.toLowerCase()}"]`,
        `${baseSelector}[keycode="${keycode}"]`,
      ].join(",")
    );
    return keyEl && !keyEl.closest("keyset").id.startsWith("ext-keyset-id");
  },

  getSystemActionForEvent(event, { rtl } = {}) {
    const meaningfulMetaKey = event.metaKey && AppConstants.platform != "win";
    const ctrlOnly =
      event.ctrlKey && !event.shiftKey && !event.altKey && !meaningfulMetaKey;
    const ctrlShift =
      event.ctrlKey && event.shiftKey && !event.altKey && !meaningfulMetaKey;

    const metaAltAccel =
      event.metaKey &&
      this.metaKeyIsCommandKey() &&
      event.altKey &&
      !event.shiftKey &&
      !event.ctrlKey;

    switch (event.keyCode) {
      case event.DOM_VK_TAB:
        if (event.ctrlKey && !event.altKey && !meaningfulMetaKey) {
          return ShortcutUtils.CYCLE_TABS;
        }
        break;
      case event.DOM_VK_F7:
        if (!event.shiftKey) {
          return ShortcutUtils.TOGGLE_CARET_BROWSING;
        }
        break;
      case event.DOM_VK_PAGE_UP:
        if (ctrlOnly) {
          return ShortcutUtils.PREVIOUS_TAB;
        }
        if (ctrlShift) {
          return ShortcutUtils.MOVE_TAB_BACKWARD;
        }
        break;
      case event.DOM_VK_PAGE_DOWN:
        if (ctrlOnly) {
          return ShortcutUtils.NEXT_TAB;
        }
        if (ctrlShift) {
          return ShortcutUtils.MOVE_TAB_FORWARD;
        }
        break;
      case event.DOM_VK_HOME:
        if (ctrlShift) {
          return ShortcutUtils.MOVE_TAB_TO_START;
        }
        break;
      case event.DOM_VK_END:
        if (ctrlShift) {
          return ShortcutUtils.MOVE_TAB_TO_END;
        }
        break;
      case event.DOM_VK_UP: // fall through
      case event.DOM_VK_LEFT:
        if (metaAltAccel) {
          return ShortcutUtils.PREVIOUS_TAB;
        }
        break;
      case event.DOM_VK_DOWN: // fall through
      case event.DOM_VK_RIGHT:
        if (metaAltAccel) {
          return ShortcutUtils.NEXT_TAB;
        }
        break;
    }

    if (AppConstants.platform == "macosx") {
      if (!event.altKey && event.metaKey) {
        switch (event.charCode) {
          case "}".charCodeAt(0):
            if (rtl) {
              return ShortcutUtils.PREVIOUS_TAB;
            }
            return ShortcutUtils.NEXT_TAB;
          case "{".charCodeAt(0):
            if (rtl) {
              return ShortcutUtils.NEXT_TAB;
            }
            return ShortcutUtils.PREVIOUS_TAB;
        }
      }
    }
    if (AppConstants.platform != "macosx") {
      if (ctrlOnly && event.keyCode == KeyEvent.DOM_VK_F4) {
        return ShortcutUtils.CLOSE_TAB;
      }
    }

    return null;
  },
};

Object.freeze(ShortcutUtils);
