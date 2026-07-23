/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { JSONFile } from "resource://gre/modules/JSONFile.sys.mjs";

const ATTRS = ["data-l10n-id", "key", "keycode", "modifiers"];

const original = {};
const windows = new Map();

const config = new JSONFile({
  path: PathUtils.join(PathUtils.profileDir, "customKeys.json"),
});

function applyToKeyEl(window, keyId, keysets) {
  const keyEl = window.document.getElementById(keyId);
  if (!keyEl || keyEl.tagName != "key") {
    return;
  }
  if (!original[keyId]) {
    const orig = (original[keyId] = {});
    for (const attr of ATTRS) {
      const val = keyEl.getAttribute(attr);
      if (val) {
        orig[attr] = val;
      }
    }
  }
  for (const attr of ATTRS) {
    keyEl.removeAttribute(attr);
  }
  const data = config.data[keyId];
  for (const attr of ["modifiers", "key", "keycode"]) {
    const val = data[attr];
    if (val) {
      keyEl.setAttribute(attr, val);
    }
  }
  keysets.add(keyEl.parentElement);
}

function resetKeyEl(window, keyId, keysets) {
  const keyEl = window.document.getElementById(keyId);
  if (!keyEl) {
    return;
  }
  const orig = original[keyId];
  for (const attr of ATTRS) {
    keyEl.removeAttribute(attr);
    const val = orig[attr];
    if (val !== undefined) {
      keyEl.setAttribute(attr, val);
    }
  }
  keysets.add(keyEl.parentElement);
}

async function applyToNewWindow(window) {
  await config.load();
  const keysets = new Set();
  for (const keyId in config.data) {
    applyToKeyEl(window, keyId, keysets);
  }
  refreshKeysets(window, keysets);
  observe(window);
}

function refreshKeysets(window, keysets) {
  if (keysets.size == 0) {
    return;
  }
  const observer = windows.get(window);
  if (observer) {
    observer.disconnect();
  }
  for (const keyset of keysets) {
    const parent = keyset.parentElement;
    keyset.remove();
    parent.append(keyset);
  }
  if (observer) {
    observe(window);
  }
}

function observe(window) {
  let observer = windows.get(window);
  if (!observer) {
    observer = new window.MutationObserver(mutations => {
      const keysets = new Set();
      for (const mutation of mutations) {
        for (const node of mutation.addedNodes) {
          if (node.tagName != "keyset") {
            continue;
          }
          for (const key of node.children) {
            if (key.tagName != "key" || !config.data[key.id]) {
              continue;
            }
            applyToKeyEl(window, key.id, keysets);
          }
        }
      }
      refreshKeysets(window, keysets);
    });
    windows.set(window, observer);
  }
  for (const node of [window.document, window.document.body]) {
    observer.observe(node, { childList: true });
  }
  return observer;
}

export const CustomKeys = {
  changeKey(id, { modifiers, key, keycode }) {
    const existing = config.data[id];
    if (
      existing &&
      modifiers == existing.modifiers &&
      key == existing.key &&
      keycode == existing.keycode
    ) {
      return; 
    }
    const defaultKey = this.getDefaultKey(id);
    if (
      defaultKey &&
      modifiers == defaultKey.modifiers &&
      key == defaultKey.key &&
      keycode == defaultKey.keycode
    ) {
      this.resetKey(id);
      return;
    }
    const data = (config.data[id] = {});
    if (modifiers) {
      data.modifiers = modifiers;
    }
    if (key) {
      data.key = key;
    }
    if (keycode) {
      data.keycode = keycode;
    }
    for (const window of windows.keys()) {
      const keysets = new Set();
      applyToKeyEl(window, id, keysets);
      refreshKeysets(window, keysets);
    }
    config.saveSoon();
  },

  resetKey(id) {
    if (!config.data[id]) {
      return; 
    }
    delete config.data[id];
    for (const window of windows.keys()) {
      const keysets = new Set();
      resetKeyEl(window, id, keysets);
      refreshKeysets(window, keysets);
    }
    delete original[id];
    config.saveSoon();
  },

  clearKey(id) {
    this.changeKey(id, {});
  },

  resetAll() {
    config.data = {};
    for (const window of windows.keys()) {
      const keysets = new Set();
      for (const id in original) {
        resetKeyEl(window, id, keysets);
      }
      refreshKeysets(window, keysets);
    }
    for (const id of Object.keys(original)) {
      delete original[id];
    }
    config.saveSoon();
  },

  initWindow(window) {
    applyToNewWindow(window);
  },

  uninitWindow(window) {
    windows.get(window).disconnect();
    windows.delete(window);
  },

  getDefaultKey(keyId) {
    const origKey = original[keyId];
    if (!origKey) {
      return null;
    }
    const data = {};
    if (origKey.modifiers) {
      data.modifiers = origKey.modifiers;
    }
    if (origKey.key) {
      data.key = origKey.key;
    }
    if (origKey.keycode) {
      data.keycode = origKey.keycode;
    }
    return data;
  },
};

Object.freeze(CustomKeys);
