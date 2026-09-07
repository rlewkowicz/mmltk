/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  UpdateUtils: "resource://gre/modules/UpdateUtils.sys.mjs",
});
XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "nativeOSKeyStore",
  "@mozilla.org/security/oskeystore;1",
  Ci.nsIOSKeyStore
);
XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "osReauthenticator",
  "@mozilla.org/security/osreauthenticator;1",
  Ci.nsIOSReauthenticator
);

const TEST_ONLY_REAUTH = "toolkit.osKeyStore.unofficialBuildOnlyLogin";

export var OSKeyStore = {
  STORE_LABEL: AppConstants.MOZ_APP_BASENAME + " Encrypted Storage",

  _isLocked: true,

  _pendingUnlockPromise: null,

  get isLoggedIn() {
    return !this._isLocked;
  },

  get isUIBusy() {
    return !!this._pendingUnlockPromise;
  },

  canReauth() {
    if (AppConstants.platform == "win" || AppConstants.platform == "macosx") {
      lazy.log.debug(
        "canReauth, returning true, this._testReauth:",
        this._testReauth
      );
      return true;
    }
    lazy.log.debug("canReauth, returning false");
    return false;
  },

  async _reauthInTests() {
    lazy.log.debug("_reauthInTests: _testReauth: ", this._testReauth);
    switch (this._testReauth) {
      case "pass":
        Services.obs.notifyObservers(
          null,
          "oskeystore-testonly-reauth",
          "pass"
        );
        return { authenticated: true, auth_details: "success" };
      case "cancel":
        Services.obs.notifyObservers(
          null,
          "oskeystore-testonly-reauth",
          "cancel"
        );
        throw new Components.Exception(
          "Simulating user cancelling login dialog",
          Cr.NS_ERROR_FAILURE
        );
      default:
        throw new Components.Exception(
          "Unknown test pref value",
          Cr.NS_ERROR_FAILURE
        );
    }
  },

  async ensureLoggedIn(
    reauth = false,
    dialogCaption = "",
    parentWindow = null,
    generateKeyIfNotAvailable = true
  ) {
    if (
      (typeof reauth != "boolean" && typeof reauth != "string") ||
      reauth === true ||
      reauth === ""
    ) {
      throw new Error(
        "reauth is required to either be `false` or a non-empty string"
      );
    }

    if (this._pendingUnlockPromise) {
      lazy.log.debug("ensureLoggedIn: Has a pending unlock operation");
      return this._pendingUnlockPromise;
    }
    lazy.log.debug(
      "ensureLoggedIn: Creating new pending unlock promise. reauth: ",
      reauth
    );

    let unlockPromise;
    if (typeof reauth == "string") {
      if (
        lazy.UpdateUtils.getUpdateChannel(false) == "default" &&
        this._testReauth
      ) {
        unlockPromise = this._reauthInTests();
      } else if (this.canReauth()) {
        unlockPromise = lazy.osReauthenticator
          .asyncReauthenticateUser(reauth, dialogCaption, parentWindow)
          .then(reauthResult => {
            let auth_details_extra = {};
            if (reauthResult.length > 3) {
              auth_details_extra.auto_admin = "" + !!reauthResult[2];
              auth_details_extra.require_signon = "" + !!reauthResult[3];
            }
            if (!reauthResult[0]) {
              throw new Components.Exception(
                "User canceled OS reauth entry",
                Cr.NS_ERROR_FAILURE,
                null,
                auth_details_extra
              );
            }
            let result = {
              authenticated: true,
              auth_details: "success",
              auth_details_extra,
            };
            if (reauthResult.length > 1 && reauthResult[1]) {
              result.auth_details += "_no_password";
            }
            return result;
          });
      } else {
        lazy.log.debug(
          "ensureLoggedIn: Skipping reauth on unsupported platforms"
        );
        unlockPromise = Promise.resolve({
          authenticated: true,
          auth_details: "success_unsupported_platform",
        });
      }
    } else {
      unlockPromise = Promise.resolve({ authenticated: true });
    }

    if (generateKeyIfNotAvailable) {
      unlockPromise = unlockPromise.then(async reauthResult => {
        try {
          if (
            !(await lazy.nativeOSKeyStore.asyncSecretAvailable(
              this.STORE_LABEL
            ))
          ) {
            lazy.log.debug(
              "ensureLoggedIn: Secret unavailable, attempt to generate new secret."
            );
            let recoveryPhrase =
              await lazy.nativeOSKeyStore.asyncGenerateSecret(this.STORE_LABEL);
            lazy.log.debug(
              "ensureLoggedIn: Secret generated. Recovery phrase length: " +
                recoveryPhrase.length
            );
          }
        } catch (e) {
          lazy.log.debug(
            `ensureLoggedIn: asyncSecretAvailable failed: ${e.result}`
          );
          throw e;
        }
        return reauthResult;
      });
    }

    unlockPromise = unlockPromise.then(
      reauthResult => {
        lazy.log.debug("ensureLoggedIn: Logged in");
        this._pendingUnlockPromise = null;
        this._isLocked = false;

        return reauthResult;
      },
      err => {
        lazy.log.debug("ensureLoggedIn: Not logged in", err);
        this._pendingUnlockPromise = null;
        this._isLocked = true;

        return {
          authenticated: false,
          auth_details: "fail",
          auth_details_extra: err.data?.QueryInterface(Ci.nsISupports)
            .wrappedJSObject,
        };
      }
    );

    this._pendingUnlockPromise = unlockPromise;

    return this._pendingUnlockPromise;
  },

  async decrypt(cipherText, trigger, reauth = false) {
    let errorResult = 0;
    try {
      if (!(await this.ensureLoggedIn(reauth)).authenticated) {
        lazy.log.warn("User canceled encryption login");
        throw Components.Exception(
          "User canceled OS unlock entry",
          Cr.NS_ERROR_ABORT
        );
      }
      let bytes = await lazy.nativeOSKeyStore.asyncDecryptBytes(
        this.STORE_LABEL,
        cipherText
      );
      return String.fromCharCode.apply(String, bytes);
    } catch (e) {
      errorResult = e.result;
      lazy.log.warn(`Decryption failed with result: ${e.result}`);
      throw e;
    } finally {
    }
  },

  async encrypt(plainText) {
    if (!(await this.ensureLoggedIn()).authenticated) {
      throw Components.Exception(
        "User canceled OS unlock entry",
        Cr.NS_ERROR_ABORT
      );
    }

    plainText = unescape(encodeURIComponent(plainText));

    let textArr = [];
    for (let char of plainText) {
      textArr.push(char.charCodeAt(0));
    }

    let rawEncryptedText = await lazy.nativeOSKeyStore.asyncEncryptBytes(
      this.STORE_LABEL,
      textArr
    );

    return rawEncryptedText;
  },

  async exportRecoveryPhrase() {
    if (!(await this.ensureLoggedIn()).authenticated) {
      throw Components.Exception(
        "User canceled OS unlock entry",
        Cr.NS_ERROR_ABORT
      );
    }

    return await lazy.nativeOSKeyStore.asyncGetRecoveryPhrase(this.STORE_LABEL);
  },

  async cleanup() {
    return lazy.nativeOSKeyStore.asyncDeleteSecret(this.STORE_LABEL);
  },
};

ChromeUtils.defineLazyGetter(lazy, "log", () => {
  return console.createInstance({
    maxLogLevelPref: "toolkit.osKeyStore.loglevel",
    prefix: "OSKeyStore",
  });
});

XPCOMUtils.defineLazyPreferenceGetter(
  OSKeyStore,
  "_testReauth",
  TEST_ONLY_REAUTH,
  ""
);
