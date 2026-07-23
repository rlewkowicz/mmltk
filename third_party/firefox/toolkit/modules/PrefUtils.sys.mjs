/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const kPrefBranches = {
  user: Services.prefs,
  default: Services.prefs.getDefaultBranch(""),
};

export var PrefUtils = {
  getPref(pref, { branch = "user", defaultValue = null } = {}) {
    const branchObj = kPrefBranches[branch];
    if (!branchObj) {
      throw new this.UnexpectedPreferenceBranch(
        `"${branch}" is not a valid preference branch`
      );
    }
    const type = branchObj.getPrefType(pref);

    try {
      switch (type) {
        case Services.prefs.PREF_BOOL: {
          return branchObj.getBoolPref(pref);
        }
        case Services.prefs.PREF_STRING: {
          return branchObj.getStringPref(pref);
        }
        case Services.prefs.PREF_INT: {
          return branchObj.getIntPref(pref);
        }
        case Services.prefs.PREF_INVALID: {
          return defaultValue;
        }
      }
    } catch (e) {
      if (branch === "default" && e.result === Cr.NS_ERROR_UNEXPECTED) {
        return defaultValue;
      }
      throw e;
    }

    throw new TypeError(`Unknown preference type (${type}) for ${pref}.`);
  },

  getPrefStrict(pref, branch) {
    if (branch === "user" && !Services.prefs.prefHasUserValue(pref)) {
      return null;
    }

    return this.getPref(pref, { branch });
  },

  setPref(pref, value, { branch = "user" } = {}) {
    if (value === null) {
      this.clearPref(pref, { branch });
      return;
    }
    const branchObj = kPrefBranches[branch];
    if (!branchObj) {
      throw new this.UnexpectedPreferenceBranch(
        `"${branch}" is not a valid preference branch`
      );
    }
    switch (typeof value) {
      case "boolean": {
        branchObj.setBoolPref(pref, value);
        break;
      }
      case "string": {
        branchObj.setStringPref(pref, value);
        break;
      }
      case "number": {
        branchObj.setIntPref(pref, value);
        break;
      }
      default: {
        throw new TypeError(
          `Unexpected value type (${typeof value}) for ${pref}.`
        );
      }
    }
  },

  clearPref(pref, { branch = "user" } = {}) {
    if (branch === "user") {
      kPrefBranches.user.clearUserPref(pref);
    } else if (branch === "default") {
      const log = console.createInstance({
        prefix: "Toolkit.PrefUtils",
        maxLogLevel: "Warn",
      });
      log.warn(
        `Cannot reset pref ${pref} on the default branch. Pref will be cleared at next restart.`
      );
    } else {
      throw new this.UnexpectedPreferenceBranch(
        `"${branch}" is not a valid preference branch`
      );
    }
  },

  UnexpectedPreferenceType: class extends Error {},
  UnexpectedPreferenceBranch: class extends Error {},
};
