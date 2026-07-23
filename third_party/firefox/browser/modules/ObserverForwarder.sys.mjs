/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

/* eslint-disable mozilla/valid-lazy */
let lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  CanvasPermissionPromptHelper:
    "moz-src:///browser/modules/CanvasPermissionPromptHelper.sys.mjs",
  FilePickerCrashed: "resource:///modules/FilePickerCrashed.sys.mjs",
  WebAuthnPromptHelper:
    "moz-src:///toolkit/modules/WebAuthnPromptHelper.sys.mjs",
});

let gObservers = {
  "canvas-permissions-prompt": ["CanvasPermissionPromptHelper"],
  "canvas-permissions-prompt-hide-doorhanger": ["CanvasPermissionPromptHelper"],

  "file-picker-crashed": ["FilePickerCrashed"],

  "webauthn-prompt": ["WebAuthnPromptHelper"],
};

if (AppConstants.MOZ_UPDATER) {
  ChromeUtils.defineESModuleGetters(lazy, {
    UpdateListener: "resource://gre/modules/UpdateListener.sys.mjs",
  });

  gObservers["update-downloading"] = ["UpdateListener"];
  gObservers["update-staged"] = ["UpdateListener"];
  gObservers["update-downloaded"] = ["UpdateListener"];
  gObservers["update-available"] = ["UpdateListener"];
  gObservers["update-error"] = ["UpdateListener"];
  gObservers["update-swap"] = ["UpdateListener"];
}

export const ObserverForwarder = {
  init() {
    for (const topic of Object.keys(gObservers)) {
      Services.obs.addObserver(this, topic);
    }
  },

  observe(subject, topic, data) {
    for (let objectName of gObservers[topic]) {
      try {
        lazy[objectName].observe(subject, topic, data);
      } catch (e) {
        console.error(e);
      }
    }
  },
};
