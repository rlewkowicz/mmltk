/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import { Log } from "resource://gre/modules/Log.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

import { RESTRequest } from "resource://services-common/rest.sys.mjs";
import { CommonUtils } from "resource://services-common/utils.sys.mjs";
import { Credentials } from "resource://gre/modules/Credentials.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  CryptoUtils: "moz-src:///services/crypto/modules/utils.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "useBearerAuth",
  "identity.fxaccounts.auth.useBearer",
  true
);

const BEARER_TOKEN_PREFIXES = {
  sessionToken: "fxs",
  keyFetchToken: "fxk",
};


export var HAWKAuthenticatedRESTRequest = function HawkAuthenticatedRESTRequest(
  uri,
  credentials,
  extra = {}
) {
  RESTRequest.call(this, uri);

  this.credentials = credentials;
  this.now = extra.now || Date.now();
  this.localtimeOffsetMsec = extra.localtimeOffsetMsec || 0;
  this._log.trace(
    "local time, offset: " + this.now + ", " + this.localtimeOffsetMsec
  );
  this.extraHeaders = extra.headers || {};

  this._intl = getIntl();
};

HAWKAuthenticatedRESTRequest.prototype = {
  async dispatch(method, data) {
    let contentType = "text/plain";
    if (method == "POST" || method == "PUT" || method == "PATCH") {
      contentType = "application/json";
    }
    if (this.credentials) {
      if (lazy.useBearerAuth && this.credentials.bearerPrefix) {
        this.setHeader(
          "Authorization",
          `Bearer ${this.credentials.bearerPrefix}_${this.credentials.id}`
        );
      } else {
        let options = {
          now: this.now,
          localtimeOffsetMsec: this.localtimeOffsetMsec,
          credentials: this.credentials,
          payload: (data && JSON.stringify(data)) || "",
          contentType,
        };
        let header = await lazy.CryptoUtils.computeHAWK(
          this.uri,
          method,
          options
        );
        this.setHeader("Authorization", header.field);
      }
    }

    for (let header in this.extraHeaders) {
      this.setHeader(header, this.extraHeaders[header]);
    }

    this.setHeader("Content-Type", contentType);

    this.setHeader("Accept-Language", this._intl.accept_languages);

    return super.dispatch(method, data);
  },
};

Object.setPrototypeOf(
  HAWKAuthenticatedRESTRequest.prototype,
  RESTRequest.prototype
);

export async function deriveHawkCredentials(tokenHex, context, size = 96) {
  let token = CommonUtils.hexToBytes(tokenHex);
  let out = await lazy.CryptoUtils.hkdfLegacy(
    token,
    undefined,
    Credentials.keyWord(context),
    size
  );

  let result = {
    key: out.slice(32, 64),
    id: CommonUtils.bytesAsHex(out.slice(0, 32)),
  };
  if (size > 64) {
    result.extra = out.slice(64);
  }
  let bearerPrefix = BEARER_TOKEN_PREFIXES[context];
  if (bearerPrefix) {
    result.bearerPrefix = bearerPrefix;
  }

  return result;
}

class HawkIntl {
  #accepted = "";
  #everRead = false;

  constructor() {
    Services.prefs.addObserver("intl.accept_languages", this);
  }

  uninit() {
    Services.prefs.removeObserver("intl.accept_languages", this);
  }

  observe() {
    this.readPref();
  }

  readPref() {
    this.#everRead = true;
    try {
      this.#accepted = Services.locale.acceptLanguages;
    } catch (err) {
      let log = Log.repository.getLogger("Services.Common.RESTRequest");
      log.error("Error reading Services.locale.acceptLanguages", err);
    }
  }

  get accept_languages() {
    if (!this.#everRead) {
      this.readPref();
    }
    return this.#accepted;
  }
}

var intl = null;
function getIntl() {
  intl ??= new HawkIntl();
  return intl;
}
