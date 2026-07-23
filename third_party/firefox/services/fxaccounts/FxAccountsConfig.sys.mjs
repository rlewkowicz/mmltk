/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { RESTRequest } from "resource://services-common/rest.sys.mjs";

import {
  log,
  SCOPE_APP_SYNC,
  SCOPE_PROFILE,
} from "resource://gre/modules/FxAccountsCommon.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "fxAccounts", () => {
  return ChromeUtils.importESModule(
    "resource://gre/modules/FxAccounts.sys.mjs"
  ).getFxAccountsSingleton();
});

ChromeUtils.defineESModuleGetters(lazy, {
  EnsureFxAccountsWebChannel:
    "resource://gre/modules/FxAccountsWebChannel.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "ROOT_URL",
  "identity.fxaccounts.remote.root"
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "CONTEXT_PARAM",
  "identity.fxaccounts.contextParam"
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "REQUIRES_HTTPS",
  "identity.fxaccounts.allowHttp",
  false,
  null,
  val => !val
);

const CONFIG_PREFS = [
  "identity.fxaccounts.remote.root",
  "identity.fxaccounts.auth.uri",
  "identity.fxaccounts.remote.oauth.uri",
  "identity.fxaccounts.remote.profile.uri",
  "identity.fxaccounts.remote.pairing.uri",
  "identity.sync.tokenserver.uri",
];
const SYNC_PARAM = "sync";

export var FxAccountsConfig = {
  async promiseEmailURI(email, entrypoint, extraParams = {}) {
    return this._buildURL("", {
      includeAuthParams: true,
      extraParams: {
        entrypoint,
        email,
        service: SYNC_PARAM,
        ...extraParams,
      },
    });
  },

  async promiseConnectAccountURI(entrypoint, extraParams = {}) {
    return this._buildURL("", {
      includeAuthParams: true,
      extraParams: {
        entrypoint,
        action: "email",
        service: SYNC_PARAM,
        ...extraParams,
      },
    });
  },

  async promiseManageURI(entrypoint, extraParams = {}) {
    return this._buildURL("settings", {
      extraParams: { entrypoint, ...extraParams },
      addAccountIdentifiers: true,
    });
  },

  async promiseChangeAvatarURI(entrypoint, extraParams = {}) {
    return this._buildURL("settings/avatar/change", {
      extraParams: { entrypoint, ...extraParams },
      addAccountIdentifiers: true,
    });
  },

  async promiseManageDevicesURI(entrypoint, extraParams = {}) {
    return this._buildURL("settings/clients", {
      extraParams: { entrypoint, ...extraParams },
      addAccountIdentifiers: true,
    });
  },

  async promiseConnectDeviceURI(entrypoint, extraParams = {}) {
    return this._buildURL("connect_another_device", {
      extraParams: { entrypoint, service: SYNC_PARAM, ...extraParams },
      addAccountIdentifiers: true,
    });
  },

  async promiseSetPasswordURI(entrypoint, extraParams = {}) {
    const authParams = await this._getAuthParams();
    return this._buildURL("post_verify/third_party_auth/set_password", {
      extraParams: {
        entrypoint,
        ...authParams,
        ...extraParams,
      },
      addAccountIdentifiers: true,
    });
  },

  async promisePairingURI(extraParams = {}) {
    return this._buildURL("pair", {
      extraParams,
      includeDefaultParams: false,
    });
  },

  async promiseOAuthURI(extraParams = {}) {
    return this._buildURL("oauth", {
      extraParams,
      includeDefaultParams: false,
    });
  },

  async promiseMetricsFlowURI(entrypoint, extraParams = {}) {
    return this._buildURL("metrics-flow", {
      extraParams: { entrypoint, ...extraParams },
      includeDefaultParams: false,
    });
  },

  get defaultParams() {
    return { context: lazy.CONTEXT_PARAM };
  },

  async _buildURL(
    path,
    {
      includeDefaultParams = true,
      includeAuthParams = false,
      extraParams = {},
      addAccountIdentifiers = false,
    }
  ) {
    await this.ensureConfigured();
    const url = new URL(path, lazy.ROOT_URL);
    this.ensureHTTPS(url.protocol);
    const authParams = includeAuthParams ? await this._getAuthParams() : {};
    const params = {
      ...(includeDefaultParams ? this.defaultParams : null),
      ...extraParams,
      ...authParams,
    };
    for (let [k, v] of Object.entries(params)) {
      url.searchParams.append(k, v);
    }
    if (addAccountIdentifiers) {
      const accountData = await this.getSignedInUser();
      if (!accountData) {
        return null;
      }
      url.searchParams.append("uid", accountData.uid);
      url.searchParams.append("email", accountData.email);
    }
    return url.href;
  },

  ensureHTTPS(protocol) {
    if (lazy.REQUIRES_HTTPS && protocol != "https:") {
      throw new Error("Firefox Accounts server must use HTTPS");
    }
  },

  async _buildURLFromString(href, extraParams = {}) {
    const url = new URL(href);
    for (let [k, v] of Object.entries(extraParams)) {
      url.searchParams.append(k, v);
    }
    return url.href;
  },

  resetConfigURLs() {
    for (let pref of CONFIG_PREFS) {
      Services.prefs.clearUserPref(pref);
    }
    lazy.fxAccounts.resetFxAccountsClient();

    lazy.EnsureFxAccountsWebChannel();
  },

  getAutoConfigURL() {
    let pref = Services.prefs.getStringPref(
      "identity.fxaccounts.autoconfig.uri",
      ""
    );
    if (!pref) {
      return "";
    }
    let rootURL = Services.urlFormatter.formatURL(pref);
    if (rootURL.endsWith("/")) {
      rootURL = rootURL.slice(0, -1);
    }
    return rootURL;
  },

  async ensureConfigured() {
    let isSignedIn = !!(await this.getSignedInUser());
    if (!isSignedIn) {
      await this.updateConfigURLs();
    }
  },

  isProductionConfig() {
    if (this.getAutoConfigURL()) {
      return false;
    }
    for (let pref of CONFIG_PREFS) {
      if (Services.prefs.prefHasUserValue(pref)) {
        return false;
      }
    }
    return true;
  },

  async updateConfigURLs() {
    let rootURL = this.getAutoConfigURL();
    if (!rootURL) {
      return;
    }
    const config = await this.fetchConfigDocument(rootURL);
    try {
      let authServerBase = config.auth_server_base_url;
      if (!authServerBase.endsWith("/v1")) {
        authServerBase += "/v1";
      }
      Services.prefs.setStringPref(
        "identity.fxaccounts.auth.uri",
        authServerBase
      );
      Services.prefs.setStringPref(
        "identity.fxaccounts.remote.oauth.uri",
        config.oauth_server_base_url + "/v1"
      );
      if (config.pairing_server_base_uri) {
        Services.prefs.setStringPref(
          "identity.fxaccounts.remote.pairing.uri",
          config.pairing_server_base_uri
        );
      }
      Services.prefs.setStringPref(
        "identity.fxaccounts.remote.profile.uri",
        config.profile_server_base_url + "/v1"
      );
      Services.prefs.setStringPref(
        "identity.sync.tokenserver.uri",
        config.sync_tokenserver_base_url + "/1.0/sync/1.5"
      );
      Services.prefs.setStringPref("identity.fxaccounts.remote.root", rootURL);

      lazy.fxAccounts.resetFxAccountsClient();

      lazy.EnsureFxAccountsWebChannel();
    } catch (e) {
      log.error(
        "Failed to initialize configuration preferences from autoconfig object",
        e
      );
      throw e;
    }
  },

  async fetchConfigDocument(rootURL = null) {
    if (!rootURL) {
      rootURL = lazy.ROOT_URL;
    }
    let configURL = rootURL + "/.well-known/fxa-client-configuration";
    let request = new RESTRequest(configURL);
    request.setHeader("Accept", "application/json");

    let resp = await request.get().catch(e => {
      log.error(`Failed to get configuration object from "${configURL}"`, e);
      throw e;
    });
    if (!resp.success) {
      log.error(
        `Received HTTP response code ${resp.status} from configuration object request:
        ${resp.body}`
      );
      throw new Error(
        `HTTP status ${resp.status} from configuration object request`
      );
    }
    log.debug("Got successful configuration response", resp.body);
    try {
      return JSON.parse(resp.body);
    } catch (e) {
      log.error(
        `Failed to parse configuration preferences from ${configURL}`,
        e
      );
      throw e;
    }
  },

  getSignedInUser() {
    return lazy.fxAccounts.getSignedInUser();
  },

  async _getAuthParams() {
    let params = {};
    const scopes = [SCOPE_APP_SYNC, SCOPE_PROFILE];
    Object.assign(
      params,
      await lazy.fxAccounts._internal.beginOAuthFlow(scopes)
    );
    return params;
  },
};
