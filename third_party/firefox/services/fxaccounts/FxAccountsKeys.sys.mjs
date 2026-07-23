/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { CommonUtils } from "resource://services-common/utils.sys.mjs";

import { CryptoUtils } from "moz-src:///services/crypto/modules/utils.sys.mjs";

import {
  SCOPE_APP_SYNC,
  OAUTH_CLIENT_ID,
  log,
  logPII,
} from "resource://gre/modules/FxAccountsCommon.sys.mjs";

const DEPRECATED_DERIVED_KEYS_NAMES = [
  "kSync",
  "kXCS",
  "kExtSync",
  "kExtKbHash",
  "ecosystemUserId",
  "ecosystemAnonId",
];

const DEPRECATED_SCOPE_WEBEXT_SYNC = "sync:addon_storage";

const LEGACY_DERIVED_KEY_SCOPES = [SCOPE_APP_SYNC];

const DEPRECATED_KEY_SCOPES = [
  DEPRECATED_SCOPE_WEBEXT_SYNC,
];

export class FxAccountsKeys {
  constructor(fxAccountsInternal) {
    this._fxai = fxAccountsInternal;
  }

  canGetKeyForScope(scope) {
    return this._fxai.withCurrentAccountState(async currentState => {
      let userData = await currentState.getUserAccountData();
      if (!userData) {
        throw new Error("Can't possibly get keys; User is not signed in");
      }
      if (!userData.verified) {
        log.info("Can't get keys; user is not verified");
        return false;
      }
      return userData.scopedKeys && userData.scopedKeys.hasOwnProperty(scope);
    });
  }

  hasKeysForScope(scope) {
    return this._fxai.withCurrentAccountState(async currentState => {
      let userData = await currentState.getUserAccountData();
      if (!userData) {
        return false;
      }
      return !!(
        userData.scopedKeys && userData.scopedKeys.hasOwnProperty(scope)
      );
    });
  }

  async getKeyForScope(scope) {
    const { scopedKeys } = await this._loadOrFetchKeys();
    if (!scopedKeys.hasOwnProperty(scope)) {
      throw new Error(`Key not available for scope "${scope}"`);
    }
    return {
      scope,
      ...scopedKeys[scope],
    };
  }

  validScopedKeys(scopedKeys) {
    for (const expectedScope of Object.keys(scopedKeys)) {
      const key = scopedKeys[expectedScope];
      if (
        !key.hasOwnProperty("scope") ||
        !key.hasOwnProperty("kid") ||
        !key.hasOwnProperty("kty") ||
        !key.hasOwnProperty("k")
      ) {
        return false;
      }
      const { scope, kid, kty, k } = key;
      if (scope != expectedScope || kty != "oct") {
        return false;
      }
      if (!kid.includes("-")) {
        return false;
      }
      const dashIndex = kid.indexOf("-");
      const keyRotationTimestamp = kid.substring(0, dashIndex);
      const fingerprint = kid.substring(dashIndex + 1);
      const keyRotationTimestampNum = Number(keyRotationTimestamp);
      if (!keyRotationTimestampNum) {
        return false;
      }
      const date = new Date(keyRotationTimestampNum);
      if (isNaN(date.getTime()) || date.getTime() <= 0) {
        return false;
      }

      const validB64String = b64String => {
        let decoded;
        try {
          decoded = ChromeUtils.base64URLDecode(b64String, {
            padding: "reject",
          });
        } catch (e) {
          return false;
        }
        return !!decoded;
      };
      if (!validB64String(fingerprint) || !validB64String(k)) {
        return false;
      }
    }
    return true;
  }

  kidAsHex(jwk) {
    const idx = jwk.kid.indexOf("-") + 1;
    if (idx <= 1) {
      throw new Error(`Invalid kid: ${jwk.kid}`);
    }
    return CommonUtils.base64urlToHex(jwk.kid.slice(idx));
  }

  async _loadOrFetchKeys() {
    return this._fxai.withCurrentAccountState(async currentState => {
      try {
        let userData = await currentState.getUserAccountData();
        if (!userData) {
          throw new Error("Can't get keys; User is not signed in");
        }
        if (userData.scopedKeys) {
          if (
            LEGACY_DERIVED_KEY_SCOPES.every(scope =>
              userData.scopedKeys.hasOwnProperty(scope)
            ) &&
            !DEPRECATED_KEY_SCOPES.some(scope =>
              userData.scopedKeys.hasOwnProperty(scope)
            ) &&
            !DEPRECATED_DERIVED_KEYS_NAMES.some(keyName =>
              userData.hasOwnProperty(keyName)
            )
          ) {
            return userData;
          }
        }
        if (!currentState.whenKeysReadyDeferred) {
          currentState.whenKeysReadyDeferred = Promise.withResolvers();
          this._migrateOrFetchKeys(currentState, userData).then(
            dataWithKeys => {
              currentState.whenKeysReadyDeferred.resolve(dataWithKeys);
              currentState.whenKeysReadyDeferred = null;
            },
            err => {
              currentState.whenKeysReadyDeferred.reject(err);
              currentState.whenKeysReadyDeferred = null;
            }
          );
        }
        return await currentState.whenKeysReadyDeferred.promise;
      } catch (err) {
        return this._fxai._handleTokenError(err);
      }
    });
  }

  async setScopedKeys(scopedKeys) {
    return this._fxai.withCurrentAccountState(async currentState => {
      const userData = await currentState.getUserAccountData();
      if (!userData) {
        throw new Error("Cannot persist keys, no user signed in");
      }
      await currentState.updateUserAccountData({
        scopedKeys,
      });
    });
  }

  async _migrateOrFetchKeys(currentState, userData) {
    if (
      userData.scopedKeys &&
      LEGACY_DERIVED_KEY_SCOPES.every(scope =>
        userData.scopedKeys.hasOwnProperty(scope)
      )
    ) {
      return this._removeDeprecatedKeys(currentState, userData);
    }

    if (!userData.sessionToken) {
      throw new Error("No sessionToken");
    }
    if (!userData.keyFetchToken) {
      throw new Error("No keyFetchToken");
    }
    return this._fetchAndUnwrapAndDeriveKeys(
      currentState,
      userData.sessionToken,
      userData.keyFetchToken
    );
  }

  async _removeDeprecatedKeys(currentState, userData) {
    const keysToRemove = DEPRECATED_DERIVED_KEYS_NAMES.filter(keyName =>
      userData.hasOwnProperty(keyName)
    );
    if (keysToRemove.length) {
      const removedKeys = {};
      for (const keyName of keysToRemove) {
        removedKeys[keyName] = null;
      }
      await currentState.updateUserAccountData({
        ...removedKeys,
      });
      userData = await currentState.getUserAccountData();
    }
    const scopesToRemove = DEPRECATED_KEY_SCOPES.filter(scope =>
      userData.scopedKeys.hasOwnProperty(scope)
    );
    if (scopesToRemove.length) {
      const updatedScopedKeys = {
        ...userData.scopedKeys,
      };
      for (const scope of scopesToRemove) {
        delete updatedScopedKeys[scope];
      }
      await currentState.updateUserAccountData({
        scopedKeys: updatedScopedKeys,
      });
      userData = await currentState.getUserAccountData();
    }
    return userData;
  }

  async _fetchAndUnwrapAndDeriveKeys(
    currentState,
    sessionToken,
    keyFetchToken
  ) {
    if (logPII()) {
      log.debug(
        `fetchAndUnwrapKeys: sessionToken: ${sessionToken}, keyFetchToken: ${keyFetchToken}`
      );
    }

    if (!sessionToken || !keyFetchToken) {
      log.warn("improper _fetchAndUnwrapKeys() call: token missing");
      await this._fxai.signOut();
      return null;
    }

    const scopedKeysMetadata =
      await this._fetchScopedKeysMetadata(sessionToken);

    let { wrapKB } = await this._fetchKeys(keyFetchToken);

    let data = await currentState.getUserAccountData();

    if (data.keyFetchToken !== keyFetchToken) {
      throw new Error("Signed in user changed while fetching keys!");
    }

    let kBbytes = CryptoUtils.xor(
      CommonUtils.hexToBytes(data.unwrapBKey),
      wrapKB
    );

    if (logPII()) {
      log.debug("kBbytes: " + kBbytes);
    }

    let updateData = {
      ...(await this._deriveKeys(data.uid, kBbytes, scopedKeysMetadata)),
      keyFetchToken: null, 
      unwrapBKey: null,
    };

    if (logPII()) {
      log.debug(`Keys Obtained: ${updateData.scopedKeys}`);
    } else {
      log.debug(
        "Keys Obtained: " + Object.keys(updateData.scopedKeys).join(", ")
      );
    }

    if (!updateData.scopedKeys) {
      throw new Error(`user data missing: scopedKeys`);
    }

    await currentState.updateUserAccountData(updateData);
    return currentState.getUserAccountData();
  }

  _fetchKeys(keyFetchToken) {
    let client = this._fxai.fxAccountsClient;
    log.debug(
      `Fetching keys with token ${!!keyFetchToken} from ${client.host}`
    );
    if (logPII()) {
      log.debug("fetchKeys - the token is " + keyFetchToken);
    }
    return client.accountKeys(keyFetchToken);
  }

  async _fetchScopedKeysMetadata(sessionToken) {
    const scopes = [SCOPE_APP_SYNC].join(" ");
    const scopedKeysMetadata =
      await this._fxai.fxAccountsClient.getScopedKeyData(
        sessionToken,
        OAUTH_CLIENT_ID,
        scopes
      );
    if (!scopedKeysMetadata.hasOwnProperty(SCOPE_APP_SYNC)) {
      log.warn(
        "The FxA server did not grant Firefox the sync scope; this is most unexpected!" +
          ` scopes were: ${Object.keys(scopedKeysMetadata)}`
      );
      throw new Error("The FxA server did not grant Firefox the sync scope");
    }
    return scopedKeysMetadata;
  }

  async _deriveKeys(uid, kBbytes, scopedKeysMetadata) {
    const scopedKeys = await this._deriveScopedKeys(
      uid,
      kBbytes,
      scopedKeysMetadata
    );
    return {
      scopedKeys,
    };
  }

  async _deriveScopedKeys(uid, kBbytes, scopedKeysMetadata) {
    const scopedKeys = {};
    for (const scope in scopedKeysMetadata) {
      if (LEGACY_DERIVED_KEY_SCOPES.includes(scope)) {
        scopedKeys[scope] = await this._deriveLegacyScopedKey(
          uid,
          kBbytes,
          scope,
          scopedKeysMetadata[scope]
        );
      } else {
        scopedKeys[scope] = await this._deriveScopedKey(
          uid,
          kBbytes,
          scope,
          scopedKeysMetadata[scope]
        );
      }
    }
    return scopedKeys;
  }

  async _deriveScopedKey(uid, kBbytes, scope, scopedKeyMetadata) {
    kBbytes = CommonUtils.byteStringToArrayBuffer(kBbytes);

    const FINGERPRINT_LENGTH = 16;
    const KEY_LENGTH = 32;
    const VALID_UID = /^[0-9a-f]{32}$/i;
    const VALID_ROTATION_SECRET = /^[0-9a-f]{64}$/i;

    if (!VALID_UID.test(uid)) {
      throw new Error("uid must be a 32-character hex string");
    }
    if (kBbytes.length != 32) {
      throw new Error("kBbytes must be exactly 32 bytes");
    }
    if (
      typeof scopedKeyMetadata.identifier !== "string" ||
      scopedKeyMetadata.identifier.length < 10
    ) {
      throw new Error("identifier must be a string of length >= 10");
    }
    if (typeof scopedKeyMetadata.keyRotationTimestamp !== "number") {
      throw new Error("keyRotationTimestamp must be a number");
    }
    if (!VALID_ROTATION_SECRET.test(scopedKeyMetadata.keyRotationSecret)) {
      throw new Error("keyRotationSecret must be a 64-character hex string");
    }

    const keyRotationTimestamp =
      "" + Math.round(scopedKeyMetadata.keyRotationTimestamp / 1000);
    if (keyRotationTimestamp.length < 10) {
      throw new Error("keyRotationTimestamp must round to a 10-digit number");
    }

    const keyRotationSecret = CommonUtils.hexToArrayBuffer(
      scopedKeyMetadata.keyRotationSecret
    );
    const salt = CommonUtils.hexToArrayBuffer(uid);
    const context = new TextEncoder().encode(
      "identity.mozilla.com/picl/v1/scoped_key\n" + scopedKeyMetadata.identifier
    );

    const inputKey = new Uint8Array(64);
    inputKey.set(kBbytes, 0);
    inputKey.set(keyRotationSecret, 32);

    const derivedKeyMaterial = await CryptoUtils.hkdf(
      inputKey,
      salt,
      context,
      FINGERPRINT_LENGTH + KEY_LENGTH
    );
    const fingerprint = derivedKeyMaterial.slice(0, FINGERPRINT_LENGTH);
    const key = derivedKeyMaterial.slice(
      FINGERPRINT_LENGTH,
      FINGERPRINT_LENGTH + KEY_LENGTH
    );

    return {
      kid:
        keyRotationTimestamp +
        "-" +
        ChromeUtils.base64URLEncode(fingerprint, {
          pad: false,
        }),
      k: ChromeUtils.base64URLEncode(key, {
        pad: false,
      }),
      kty: "oct",
    };
  }

  async _deriveLegacyScopedKey(uid, kBbytes, scope, scopedKeyMetadata) {
    let kid, key;
    if (scope == SCOPE_APP_SYNC) {
      kid = await this._deriveXClientState(kBbytes);
      key = await this._deriveSyncKey(kBbytes);
    } else {
      throw new Error(`Unexpected legacy key-bearing scope: ${scope}`);
    }
    kid = CommonUtils.byteStringToArrayBuffer(kid);
    key = CommonUtils.byteStringToArrayBuffer(key);
    return this._formatLegacyScopedKey(kid, key, scope, scopedKeyMetadata);
  }

  _formatLegacyScopedKey(kid, key, scope, { keyRotationTimestamp }) {
    kid = ChromeUtils.base64URLEncode(kid, {
      pad: false,
    });
    key = ChromeUtils.base64URLEncode(key, {
      pad: false,
    });
    return {
      kid: `${keyRotationTimestamp}-${kid}`,
      k: key,
      kty: "oct",
    };
  }

  async _deriveSyncKey(kBbytes) {
    return CryptoUtils.hkdfLegacy(
      kBbytes,
      undefined,
      "identity.mozilla.com/picl/v1/oldsync",
      2 * 32
    );
  }

  async _deriveXClientState(kBbytes) {
    return this._sha256(kBbytes).slice(0, 16);
  }

  _sha256(bytes) {
    let hasher = Cc["@mozilla.org/security/hash;1"].createInstance(
      Ci.nsICryptoHash
    );
    hasher.init(hasher.SHA256);
    return CryptoUtils.digestBytes(bytes, hasher);
  }
}
