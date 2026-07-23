/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  jwcrypto: "moz-src:///services/crypto/modules/jwcrypto.sys.mjs",
});

import {
  OAUTH_CLIENT_ID,
  SCOPE_PROFILE,
  SCOPE_PROFILE_WRITE,
  SCOPE_APP_SYNC,
  log,
} from "resource://gre/modules/FxAccountsCommon.sys.mjs";

const VALID_SCOPES = [SCOPE_PROFILE, SCOPE_PROFILE_WRITE, SCOPE_APP_SYNC];

export const ERROR_INVALID_SCOPES = "INVALID_SCOPES";
export const ERROR_INVALID_STATE = "INVALID_STATE";
export const ERROR_SYNC_SCOPE_NOT_GRANTED = "ERROR_SYNC_SCOPE_NOT_GRANTED";
export const ERROR_NO_KEYS_JWE = "ERROR_NO_KEYS_JWE";
export const ERROR_OAUTH_FLOW_ABANDONED = "ERROR_OAUTH_FLOW_ABANDONED";
export const ERROR_INVALID_SCOPED_KEYS = "ERROR_INVALID_SCOPED_KEYS";

export class FxAccountsOAuth {
  #flow;
  #fxaClient;
  #fxaKeys;
  constructor(fxaClient, fxaKeys) {
    this.#flow = {};
    this.#fxaClient = fxaClient;
    this.#fxaKeys = fxaKeys;
  }

  addFlow(state, value) {
    this.#flow[state] = value;
  }

  clearAllFlows() {
    this.#flow = {};
  }

  getFlow(state) {
    return this.#flow[state];
  }

  numOfFlows() {
    return Object.keys(this.#flow).length;
  }

  async beginOAuthFlow(scopes) {
    if (
      !Array.isArray(scopes) ||
      scopes.some(scope => !VALID_SCOPES.includes(scope))
    ) {
      throw new Error(ERROR_INVALID_SCOPES);
    }
    const queryParams = {
      client_id: OAUTH_CLIENT_ID,
      action: "email",
      response_type: "code",
      access_type: "offline",
      scope: scopes.join(" "),
    };

    const state = new Uint8Array(16);
    crypto.getRandomValues(state);
    const stateB64 = ChromeUtils.base64URLEncode(state, { pad: false });
    queryParams.state = stateB64;

    const codeVerifier = new Uint8Array(32);
    crypto.getRandomValues(codeVerifier);
    const codeVerifierB64 = ChromeUtils.base64URLEncode(codeVerifier, {
      pad: false,
    });
    const challenge = await crypto.subtle.digest(
      "SHA-256",
      new TextEncoder().encode(codeVerifierB64)
    );
    const challengeB64 = ChromeUtils.base64URLEncode(challenge, { pad: false });
    queryParams.code_challenge = challengeB64;
    queryParams.code_challenge_method = "S256";

    const ECDH_KEY = { name: "ECDH", namedCurve: "P-256" };
    const key = await crypto.subtle.generateKey(ECDH_KEY, false, ["deriveKey"]);
    const publicKey = await crypto.subtle.exportKey("jwk", key.publicKey);
    const privateKey = key.privateKey;

    const encodedPublicKey = ChromeUtils.base64URLEncode(
      new TextEncoder().encode(JSON.stringify(publicKey)),
      { pad: false }
    );
    queryParams.keys_jwk = encodedPublicKey;

    this.addFlow(stateB64, {
      key: privateKey,
      verifier: codeVerifierB64,
      requestedScopes: scopes.join(" "),
    });
    return queryParams;
  }

  async completeOAuthFlow(sessionTokenHex, code, state) {
    const flow = this.getFlow(state);
    if (!flow) {
      throw new Error(ERROR_INVALID_STATE);
    }
    const { key, verifier, requestedScopes } = flow;
    const { keys_jwe, refresh_token, access_token, scope } =
      await this.#fxaClient.oauthToken(
        sessionTokenHex,
        code,
        verifier,
        OAUTH_CLIENT_ID
      );
    const requestedSync = requestedScopes.includes(SCOPE_APP_SYNC);
    const grantedSync = scope.includes(SCOPE_APP_SYNC);
    if (requestedSync && !grantedSync) {
      log.info("Requested Sync scope but was not granted sync!");
    }
    let scopedKeys;
    if (keys_jwe) {
      scopedKeys = JSON.parse(
        new TextDecoder().decode(await lazy.jwcrypto.decryptJWE(keys_jwe, key))
      );
      if (!this.#fxaKeys.validScopedKeys(scopedKeys)) {
        throw new Error(ERROR_INVALID_SCOPED_KEYS);
      }
    }

    if (!this.getFlow(state)) {
      throw new Error(ERROR_OAUTH_FLOW_ABANDONED);
    }

    this.clearAllFlows();
    return {
      scopedKeys,
      refreshToken: refresh_token,
      accessToken: access_token,
    };
  }
}
