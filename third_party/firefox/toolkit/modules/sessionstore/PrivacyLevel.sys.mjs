/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const PRIVACY_NONE = 0;
const PRIVACY_ENCRYPTED = 1;
const PRIVACY_FULL = 2;

export var PrivacyLevel = {
  check(url) {
    return PrivacyLevel.canSave(url.startsWith("https:"));
  },

  canSave(isHttps) {
    if (this.privacyLevel == PRIVACY_FULL) {
      return false;
    }

    if (isHttps && this.privacyLevel == PRIVACY_ENCRYPTED) {
      return false;
    }

    return true;
  },

  canSaveAnything() {
    return this.privacyLevel != PRIVACY_FULL;
  },

  shouldSaveEverything() {
    return this.privacyLevel == PRIVACY_NONE;
  },
};

XPCOMUtils.defineLazyPreferenceGetter(
  PrivacyLevel,
  "privacyLevel",
  "browser.sessionstore.privacy_level"
);
