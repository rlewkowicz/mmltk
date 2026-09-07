/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

import { FxAccountsStorageManager } from "resource://gre/modules/FxAccountsStorage.sys.mjs";

import {
  ATTACHED_CLIENTS_CACHE_DURATION,
  ERRNO_INVALID_AUTH_TOKEN,
  ERROR_AUTH_ERROR,
  ERROR_INVALID_PARAMETER,
  ERROR_NO_ACCOUNT,
  ERROR_TO_GENERAL_ERROR_CLASS,
  ERROR_UNKNOWN,
  ERROR_UNVERIFIED_ACCOUNT,
  FXA_PWDMGR_PLAINTEXT_FIELDS,
  FXA_PWDMGR_REAUTH_ALLOWLIST,
  FXA_PWDMGR_SECURE_FIELDS,
  OAUTH_CLIENT_ID,
  ON_ACCOUNT_STATE_CHANGE_NOTIFICATION,
  ONLOGIN_NOTIFICATION,
  ONLOGOUT_NOTIFICATION,
  ON_PRELOGOUT_NOTIFICATION,
  ONVERIFIED_NOTIFICATION,
  ON_DEVICE_DISCONNECTED_NOTIFICATION,
  POLL_SESSION,
  PREF_ACCOUNT_ROOT,
  PREF_LAST_FXA_USER_EMAIL,
  PREF_LAST_FXA_USER_UID,
  SERVER_ERRNO_TO_ERROR,
  log,
  logPII,
  logManager,
} from "resource://gre/modules/FxAccountsCommon.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  CryptoUtils: "moz-src:///services/crypto/modules/utils.sys.mjs",
  FxAccountsClient: "resource://gre/modules/FxAccountsClient.sys.mjs",
  FxAccountsCommands: "resource://gre/modules/FxAccountsCommands.sys.mjs",
  FxAccountsConfig: "resource://gre/modules/FxAccountsConfig.sys.mjs",
  FxAccountsDevice: "resource://gre/modules/FxAccountsDevice.sys.mjs",
  FxAccountsKeys: "resource://gre/modules/FxAccountsKeys.sys.mjs",
  FxAccountsOAuth: "resource://gre/modules/FxAccountsOAuth.sys.mjs",
  FxAccountsProfile: "resource://gre/modules/FxAccountsProfile.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "mpLocked", () => {
  return ChromeUtils.importESModule("resource://services-sync/util.sys.mjs")
    .Utils.mpLocked;
});

ChromeUtils.defineLazyGetter(lazy, "ensureMPUnlocked", () => {
  return ChromeUtils.importESModule("resource://services-sync/util.sys.mjs")
    .Utils.ensureMPUnlocked;
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "FXA_ENABLED",
  "identity.fxaccounts.enabled",
  true
);

export const ERROR_INVALID_ACCOUNT_STATE = "ERROR_INVALID_ACCOUNT_STATE";

const OAUTH_MIN_TIME_LEFT_SECS = 60;

export function AccountState(storageManager) {
  this.storageManager = storageManager;
  this.inFlightTokenRequests = new Map();
  this.promiseInitialized = this.storageManager
    .getAccountData()
    .then(data => {
      this.oauthTokens = data && data.oauthTokens ? data.oauthTokens : {};
    })
    .catch(err => {
      log.error("Failed to initialize the storage manager", err);
    });
}

AccountState.prototype = {
  oauthTokens: null,
  whenKeysReadyDeferred: null,

  get isCurrent() {
    return this.storageManager != null;
  },

  abort() {
    if (this.whenKeysReadyDeferred) {
      this.whenKeysReadyDeferred.reject(
        new Error("Key fetching aborted; Another user signing in")
      );
      this.whenKeysReadyDeferred = null;
    }
    this.inFlightTokenRequests.clear();
    return this.signOut();
  },

  async signOut() {
    this.cert = null;
    this.keyPair = null;
    this.oauthTokens = null;
    this.inFlightTokenRequests.clear();

    if (!this.storageManager) {
      return;
    }
    const storageManager = this.storageManager;
    this.storageManager = null;

    await storageManager.deleteAccountData();
    await storageManager.finalize();
  },

  getUserAccountData(fieldNames = null) {
    if (!this.isCurrent) {
      return Promise.reject(new Error("Another user has signed in"));
    }
    return this.storageManager.getAccountData(fieldNames).then(result => {
      return this.resolve(result);
    });
  },

  async updateUserAccountData(updatedFields) {
    if ("uid" in updatedFields) {
      const existing = await this.getUserAccountData(["uid"]);
      if (existing.uid != updatedFields.uid) {
        throw new Error(
          "The specified credentials aren't for the current user"
        );
      }
      updatedFields = Cu.cloneInto(updatedFields, {}); 
      delete updatedFields.uid;
    }
    if (!this.isCurrent) {
      return Promise.reject(new Error(ERROR_INVALID_ACCOUNT_STATE));
    }
    return this.storageManager.updateAccountData(updatedFields);
  },

  resolve(result) {
    if (!this.isCurrent) {
      log.info(
        "An accountState promise was resolved, but was actually rejected" +
          " due to the account state changing. This can happen if a new account signed in, or" +
          " the account was signed out. Originally resolved with, ",
        result
      );
      return Promise.reject(new Error(ERROR_INVALID_ACCOUNT_STATE));
    }
    return Promise.resolve(result);
  },

  reject(error) {
    if (!this.isCurrent) {
      log.info(
        "An accountState promise was rejected, but we are ignoring that" +
          " reason and rejecting it due to the account state changing. This can happen if" +
          " a different account signed in or the account was signed out" +
          " originally resolved with, ",
        error
      );
      return Promise.reject(new Error(ERROR_INVALID_ACCOUNT_STATE));
    }
    return Promise.reject(error);
  },


  _cachePreamble() {
    if (!this.isCurrent) {
      throw new Error(ERROR_INVALID_ACCOUNT_STATE);
    }
  },

  setCachedToken(scopeArray, tokenData) {
    this._cachePreamble();
    if (!tokenData.token) {
      throw new Error("No token");
    }
    let key = getScopeKey(scopeArray);
    this.oauthTokens[key] = tokenData;
    this._persistCachedTokens();
  },

  getCachedToken(scopeArray) {
    this._cachePreamble();
    let key = getScopeKey(scopeArray);
    let result = this.oauthTokens[key];
    if (result) {
      if (result.expiresAt != null) {
        const nowSecs = Math.floor(Date.now() / 1000);
        if (result.expiresAt <= nowSecs + OAUTH_MIN_TIME_LEFT_SECS) {
          log.debug("getCachedToken returning null for expired token");
          return null;
        }
      }
      log.trace("getCachedToken returning cached token");
      return result;
    }
    return null;
  },

  removeCachedToken(token) {
    this._cachePreamble();
    let data = this.oauthTokens;
    for (let [key, tokenValue] of Object.entries(data)) {
      if (tokenValue.token == token) {
        delete data[key];
        this._persistCachedTokens();
        return tokenValue;
      }
    }
    return null;
  },

  _persistCachedTokens() {
    this._cachePreamble();
    return this.updateUserAccountData({ oauthTokens: this.oauthTokens }).catch(
      err => {
        log.error("Failed to update cached tokens", err);
      }
    );
  },
};

function getScopeKey(scopeArray) {
  let normalizedScopes = scopeArray.map(item => item.toLowerCase());
  return normalizedScopes.sort().join("|");
}

function getPropertyDescriptor(obj, prop) {
  return (
    Object.getOwnPropertyDescriptor(obj, prop) ||
    getPropertyDescriptor(Object.getPrototypeOf(obj), prop)
  );
}

function copyObjectProperties(from, to, thisObj, keys) {
  for (let prop of keys) {
    let desc = getPropertyDescriptor(from, prop);

    if (typeof desc.value == "function") {
      desc.value = desc.value.bind(thisObj);
    }

    if (desc.get) {
      desc.get = desc.get.bind(thisObj);
    }

    if (desc.set) {
      desc.set = desc.set.bind(thisObj);
    }

    Object.defineProperty(to, prop, desc);
  }
}

export class FxAccounts {
  constructor(mocks = null) {
    this._internal = new FxAccountsInternal();
    if (mocks) {
      copyObjectProperties(
        mocks,
        this._internal,
        this._internal,
        Object.keys(mocks).filter(key => !["device", "commands"].includes(key))
      );
    }
    this._internal.initialize();
    if (mocks) {
      for (let subobject of [
        "currentAccountState",
        "keys",
        "fxaPushService",
        "device",
        "commands",
      ]) {
        if (typeof mocks[subobject] == "object") {
          copyObjectProperties(
            mocks[subobject],
            this._internal[subobject],
            this._internal[subobject],
            Object.keys(mocks[subobject])
          );
        }
      }
    }
  }

  get commands() {
    return this._internal.commands;
  }

  static get config() {
    return lazy.FxAccountsConfig;
  }

  get device() {
    return this._internal.device;
  }

  get keys() {
    return this._internal.keys;
  }

  _withCurrentAccountState(func) {
    return this._internal.withCurrentAccountState(func);
  }

  _withVerifiedAccountState(func) {
    return this._internal.withVerifiedAccountState(func);
  }

  _withSessionToken(func, mustBeVerified = true) {
    return this._internal.withSessionToken(func, mustBeVerified);
  }

  async listAttachedOAuthClients(forceRefresh = false) {
    const now = Date.now();

    if (
      this._cachedClients &&
      now - this._cachedClientsTimestamp < ATTACHED_CLIENTS_CACHE_DURATION &&
      !forceRefresh
    ) {
      return this._cachedClients;
    }

    let clients = null;
    try {
      clients = await this._fetchAttachedOAuthClients();
      this._cachedClients = clients;
      this._cachedClientsTimestamp = now;
    } catch (error) {
      log.error("Could not update attached clients list ", error);
      clients = [];
    }

    return clients;
  }

  async _fetchAttachedOAuthClients() {
    const ONE_DAY = 24 * 60 * 60 * 1000;

    return this._withSessionToken(async sessionToken => {
      const response =
        await this._internal.fxAccountsClient.attachedClients(sessionToken);
      const attachedClients = response.body;
      const timestamp = response.headers["x-timestamp"];
      const now =
        timestamp !== undefined
          ? new Date(parseInt(timestamp, 10))
          : Date.now();
      return attachedClients.map(client => {
        const daysAgo = client.lastAccessTime
          ? Math.max(Math.floor((now - client.lastAccessTime) / ONE_DAY), 0)
          : null;
        return {
          id: client.clientId,
          lastAccessedDaysAgo: daysAgo,
        };
      });
    });
  }

  async getOAuthToken(options = {}) {
    try {
      return await this._internal.getOAuthToken(options);
    } catch (err) {
      throw this._internal._errorToErrorClass(err);
    }
  }

  getOAuthTokenAndKey(options = {}) {
    return this._withCurrentAccountState(async () => {
      const key = await this.keys.getKeyForScope(options.scope);
      const token = await this.getOAuthToken(options);
      return { token, key };
    });
  }

  resetFxAccountsClient() {
    this._internal.resetFxAccountsClient();
  }

  removeCachedOAuthToken(options) {
    return this._internal.removeCachedOAuthToken(options);
  }

  getSignedInUser(addnFields = []) {
    const ACCT_DATA_FIELDS = [
      "email",
      "uid",
      "verified",
      "scopedKeys",
      "sessionToken",
    ];
    const PROFILE_FIELDS = ["displayName", "avatar", "avatarDefault"];
    return this._withCurrentAccountState(async currentState => {
      const data = await currentState.getUserAccountData(
        ACCT_DATA_FIELDS.concat(addnFields)
      );
      if (!data) {
        return null;
      }
      if (!lazy.FXA_ENABLED) {
        await this.signOut();
        return null;
      }
      delete data.scopedKeys;

      let profileData = null;
      if (data.sessionToken) {
        delete data.sessionToken;
        try {
          profileData = await this._internal.profile.getProfile();
        } catch (error) {
          log.error("Could not retrieve profile data", error);
        }
      }
      for (let field of PROFILE_FIELDS) {
        data[field] = profileData ? profileData[field] : null;
      }
      if (profileData && profileData.email) {
        data.email = profileData.email;
      }
      return data;
    });
  }

  checkAccountStatus() {
    let state = this._internal.currentAccountState;
    return this._internal.checkAccountStatus(state);
  }

  hasLocalSession() {
    return this._withCurrentAccountState(async state => {
      let data = await state.getUserAccountData(["sessionToken"]);
      return !!(data && data.sessionToken);
    });
  }

  static canConnectAccount() {
    return Promise.resolve(!lazy.mpLocked() || lazy.ensureMPUnlocked());
  }

  notifyDevices(deviceIds, excludedIds, payload, TTL) {
    return this._internal.notifyDevices(deviceIds, excludedIds, payload, TTL);
  }

  resendVerificationEmail() {
    return this._withSessionToken((token, _currentState) => {
      return this._internal.fxAccountsClient.resendVerificationEmail(token);
    }, false);
  }

  async signOut(localOnly) {
    return this._internal.signOut(localOnly);
  }

  updateDeviceRegistration() {
    return this._withCurrentAccountState(_ => {
      return this._internal.updateDeviceRegistration();
    });
  }

  async flushLogFile() {
    const logType = await logManager.resetFileLog();
    if (logType == logManager.ERROR_LOG_WRITTEN) {
      console.error(
        "FxA encountered an error - see about:sync-log for the log file."
      );
    }
    Services.obs.notifyObservers(null, "service:log-manager:flush-log-file");
  }
}

var FxAccountsInternal = function () {};

FxAccountsInternal.prototype = {
  POLL_SESSION,

  VERIFICATION_POLL_TIMEOUT_INITIAL: 60000, 
  VERIFICATION_POLL_TIMEOUT_SUBSEQUENT: 5 * 60000, 
  VERIFICATION_POLL_START_SLOWDOWN_THRESHOLD: 5,

  _fxAccountsClient: null,

  initialize() {
    ChromeUtils.defineLazyGetter(this, "fxaPushService", function () {
      return Cc["@mozilla.org/fxaccounts/push;1"].getService(Ci.nsISupports)
        .wrappedJSObject;
    });

    this.keys = new lazy.FxAccountsKeys(this);

    if (!this.observerPreloads) {
      this.observerPreloads = [
        () => {
          let { Weave } = ChromeUtils.importESModule(
            "resource://services-sync/main.sys.mjs"
          );
          return Weave.Service.promiseInitialized;
        },
      ];
    }

    this._cachedAttachedClients = null;
    this._cachedAttachedClientsTimestamp = 0;
    this.currentAccountState = this.newAccountState();
  },

  async withCurrentAccountState(func) {
    const state = this.currentAccountState;
    let result;
    try {
      result = await func(state);
    } catch (ex) {
      return state.reject(ex);
    }
    return state.resolve(result);
  },

  async withVerifiedAccountState(func) {
    return this.withCurrentAccountState(async state => {
      let data = await state.getUserAccountData();
      if (!data) {
        throw this._error(ERROR_NO_ACCOUNT);
      }

      if (!data.verified) {
        throw this._error(ERROR_UNVERIFIED_ACCOUNT);
      }
      return func(state);
    });
  },

  async withSessionToken(func, mustBeVerified = true) {
    const state = this.currentAccountState;
    let data = await state.getUserAccountData();
    if (!data) {
      throw this._error(ERROR_NO_ACCOUNT);
    }

    if (mustBeVerified && !data.verified) {
      throw this._error(ERROR_UNVERIFIED_ACCOUNT);
    }

    if (!data.sessionToken) {
      throw this._error(ERROR_AUTH_ERROR, "no session token");
    }
    try {
      let result = await func(data.sessionToken, state);
      return state.resolve(result);
    } catch (err) {
      return this._handleTokenError(err);
    }
  },

  resetFxAccountsClient() {
    this._fxAccountsClient = null;
    this._oauth = null;
  },

  get fxAccountsClient() {
    if (!this._fxAccountsClient) {
      this._fxAccountsClient = new lazy.FxAccountsClient();
    }
    return this._fxAccountsClient;
  },

  _profile: null,
  get profile() {
    if (!this._profile) {
      let profileServerUrl = Services.urlFormatter.formatURLPref(
        "identity.fxaccounts.remote.profile.uri"
      );
      this._profile = new lazy.FxAccountsProfile({
        fxa: this,
        profileServerUrl,
      });
    }
    return this._profile;
  },

  _commands: null,
  get commands() {
    if (!this._commands) {
      if (
        !Services.startup.isInOrBeyondShutdownPhase(
          Ci.nsIAppStartup.SHUTDOWN_PHASE_APPSHUTDOWNCONFIRMED
        )
      ) {
        this._commands = new lazy.FxAccountsCommands(this);
      }
    }
    return this._commands;
  },

  _device: null,
  get device() {
    if (!this._device) {
      this._device = new lazy.FxAccountsDevice(this);
    }
    return this._device;
  },

  _oauth: null,
  get oauth() {
    if (!this._oauth) {
      this._oauth = new lazy.FxAccountsOAuth(this.fxAccountsClient, this.keys);
    }
    return this._oauth;
  },

  beginOAuthFlow(scopes) {
    return this.oauth.beginOAuthFlow(scopes);
  },

  completeOAuthFlow(sessionToken, code, state) {
    return this.oauth.completeOAuthFlow(sessionToken, code, state);
  },

  setScopedKeys(scopedKeys) {
    return this.keys.setScopedKeys(scopedKeys);
  },

  newAccountState(credentials) {
    let storage = new FxAccountsStorageManager();
    storage.initialize(credentials);
    return new AccountState(storage);
  },

  notifyDevices(deviceIds, excludedIds, payload, TTL) {
    if (typeof deviceIds == "string") {
      deviceIds = [deviceIds];
    }
    return this.withSessionToken(sessionToken => {
      return this.fxAccountsClient.notifyDevices(
        sessionToken,
        deviceIds,
        excludedIds,
        payload,
        TTL
      );
    });
  },

  now() {
    return this.fxAccountsClient.now();
  },

  get localtimeOffsetMsec() {
    return this.fxAccountsClient.localtimeOffsetMsec;
  },

  checkEmailStatus: function checkEmailStatus(sessionToken, options = {}) {
    if (!sessionToken) {
      return Promise.reject(
        new Error("checkEmailStatus called without a session token")
      );
    }
    return this.fxAccountsClient
      .recoveryEmailStatus(sessionToken, options)
      .catch(error => this._handleTokenError(error));
  },


  async setSignedInUser(credentials) {
    if (!lazy.FXA_ENABLED) {
      throw new Error("Cannot call setSignedInUser when FxA is disabled.");
    }
    for (const pref of Services.prefs.getChildList(PREF_ACCOUNT_ROOT)) {
      Services.prefs.clearUserPref(pref);
    }
    log.debug("setSignedInUser - aborting any existing flows");
    const signedInUser = await this.currentAccountState.getUserAccountData();
    if (signedInUser) {
      await this._signOutServer(
        signedInUser.sessionToken,
        signedInUser.oauthTokens
      );
    }
    await this.abortExistingFlow();
    const currentAccountState = (this.currentAccountState =
      this.newAccountState(
        Cu.cloneInto(credentials, {}) 
      ));
    await currentAccountState.promiseInitialized;
    await this.notifyObservers(ONLOGIN_NOTIFICATION);
    await this.updateDeviceRegistration();
    return currentAccountState.resolve();
  },

  updateUserAccountData(credentials) {
    log.debug(
      "updateUserAccountData called with fields",
      Object.keys(credentials)
    );
    if (logPII()) {
      log.debug("updateUserAccountData called with data", credentials);
    }
    let currentAccountState = this.currentAccountState;
    return currentAccountState.promiseInitialized.then(() => {
      if (!credentials.uid) {
        throw new Error("The specified credentials have no uid");
      }
      return currentAccountState.updateUserAccountData(credentials);
    });
  },

  abortExistingFlow() {
    if (this._profile) {
      this._profile.tearDown();
      this._profile = null;
    }
    if (this._commands) {
      this._commands = null;
    }
    if (this._device) {
      this._device.reset();
    }
    return this.currentAccountState.abort();
  },

  destroyOAuthToken(tokenData) {
    return this.fxAccountsClient.oauthDestroy(OAUTH_CLIENT_ID, tokenData.token);
  },

  _destroyAllOAuthTokens(tokenInfos) {
    if (!tokenInfos) {
      return Promise.resolve();
    }
    let promises = [];
    for (let tokenInfo of Object.values(tokenInfos)) {
      promises.push(this.destroyOAuthToken(tokenInfo));
    }
    return Promise.all(promises);
  },

  _migratePreviousAccountNameHashPref(uid) {
    if (Services.prefs.prefHasUserValue(PREF_LAST_FXA_USER_EMAIL)) {
      Services.prefs.setStringPref(
        PREF_LAST_FXA_USER_UID,
        lazy.CryptoUtils.sha256Base64(uid)
      );
      Services.prefs.clearUserPref(PREF_LAST_FXA_USER_EMAIL);
    }
  },

  async signOut(localOnly) {
    let sessionToken;
    let tokensToRevoke;
    const data = await this.currentAccountState.getUserAccountData();
    if (data) {
      sessionToken = data.sessionToken;
      tokensToRevoke = data.oauthTokens;
      this._migratePreviousAccountNameHashPref(data.uid);
    }
    await this.notifyObservers(ON_PRELOGOUT_NOTIFICATION);
    await this._signOutLocal();
    if (!localOnly) {
      Services.tm.dispatchToMainThread(async () => {
        await this._signOutServer(sessionToken, tokensToRevoke);
        lazy.FxAccountsConfig.resetConfigURLs();
        this.notifyObservers("testhelper-fxa-signout-complete");
      });
    } else {
      lazy.FxAccountsConfig.resetConfigURLs();
    }
    return this.notifyObservers(ONLOGOUT_NOTIFICATION);
  },

  async _signOutLocal() {
    for (const pref of Services.prefs.getChildList(PREF_ACCOUNT_ROOT)) {
      Services.prefs.clearUserPref(pref);
    }
    await this.currentAccountState.signOut();
    await this.abortExistingFlow();
    this.currentAccountState = this.newAccountState();
    return this.currentAccountState.promiseInitialized;
  },

  async _signOutServer(sessionToken, tokensToRevoke) {
    log.debug("Unsubscribing from FxA push.");
    try {
      await this.fxaPushService.unsubscribe();
    } catch (err) {
      log.error("Could not unsubscribe from push.", err);
    }
    if (sessionToken) {
      log.debug("Destroying session and device.");
      try {
        await this.fxAccountsClient.signOut(sessionToken, { service: "sync" });
      } catch (err) {
        log.error("Error during remote sign out of Firefox Accounts", err);
      }
    } else {
      log.warn("Missing session token; skipping remote sign out");
    }
    log.debug("Destroying all OAuth tokens.");
    try {
      await this._destroyAllOAuthTokens(tokensToRevoke);
    } catch (err) {
      log.error("Error during destruction of oauth tokens during signout", err);
    }
  },

  getUserAccountData(fieldNames = null) {
    return this.currentAccountState.getUserAccountData(fieldNames);
  },

  async notifyObservers(topic, data) {
    for (let f of this.observerPreloads) {
      try {
        await f();
      } catch (O_o) {}
    }
    log.debug("Notifying observers of " + topic);
    Services.obs.notifyObservers(null, topic, data);
  },

  async _doTokenFetchWithSessionToken(sessionToken, scopeString, ttl) {
    const result = await this.fxAccountsClient.accessTokenWithSessionToken(
      sessionToken,
      OAUTH_CLIENT_ID,
      scopeString,
      ttl
    );
    return {
      token: result.access_token,
      expiresAt: result.expires_in
        ? Math.floor(Date.now() / 1000) + result.expires_in
        : null,
    };
  },

  getOAuthToken(options = {}) {
    log.debug("getOAuthToken enter");
    let scope = options.scope;
    if (typeof scope === "string") {
      scope = [scope];
    }

    if (!scope || !scope.length) {
      return Promise.reject(
        this._error(
          ERROR_INVALID_PARAMETER,
          "Missing or invalid 'scope' option"
        )
      );
    }

    return this.withSessionToken(async (sessionToken, currentState) => {
      let cached = currentState.getCachedToken(scope);
      if (cached) {
        log.debug("getOAuthToken returning a cached token");
        return cached.token;
      }

      let scopeString = scope.sort().join(" ");

      let maybeInFlight = currentState.inFlightTokenRequests.get(scopeString);
      if (maybeInFlight) {
        log.debug("getOAuthToken has an in-flight request for this scope");
        return maybeInFlight;
      }

      let promise = this._doTokenFetchWithSessionToken(
        sessionToken,
        scopeString,
        options.ttl
      )
        .then(tokenInfo => {
          if (currentState.getCachedToken(scope)) {
            log.error(`detected a race for oauth token with scope ${scope}`);
          }
          if (tokenInfo.token) {
            let entry = { token: tokenInfo.token };
            if (tokenInfo.expiresAt != null) {
              entry.expiresAt = tokenInfo.expiresAt;
            }
            currentState.setCachedToken(scope, entry);
          }
          return tokenInfo.token;
        })
        .finally(() => {
          currentState.inFlightTokenRequests.delete(scopeString);
        });

      currentState.inFlightTokenRequests.set(scopeString, promise);
      return promise;
    });
  },

  removeCachedOAuthToken(options) {
    if (!options.token || typeof options.token !== "string") {
      throw this._error(
        ERROR_INVALID_PARAMETER,
        "Missing or invalid 'token' option"
      );
    }
    return this.withCurrentAccountState(currentState => {
      let existing = currentState.removeCachedToken(options.token);
      if (existing) {
        this.destroyOAuthToken(existing).catch(err => {
          log.warn("FxA failed to revoke a cached token", err);
        });
      }
    });
  },

  async setUserVerified() {
    await this.withCurrentAccountState(async currentState => {
      const userData = await currentState.getUserAccountData();
      if (!userData.verified) {
        await currentState.updateUserAccountData({ verified: true });
      }
    });
    await this.notifyObservers(ONVERIFIED_NOTIFICATION);
  },

  async _handleAccountDestroyed(uid) {
    let state = this.currentAccountState;
    const accountData = await state.getUserAccountData();
    const localUid = accountData ? accountData.uid : null;
    if (!localUid) {
      log.info(
        `Account destroyed push notification received, but we're already logged-out`
      );
      return null;
    }
    if (uid == localUid) {
      const data = JSON.stringify({ isLocalDevice: true });
      await this.notifyObservers(ON_DEVICE_DISCONNECTED_NOTIFICATION, data);
      return this.signOut(true);
    }
    log.info(
      `The destroyed account uid doesn't match with the local uid. ` +
        `Local: ${localUid}, account uid destroyed: ${uid}`
    );
    return null;
  },

  async _handleDeviceDisconnection(deviceId) {
    let state = this.currentAccountState;
    const accountData = await state.getUserAccountData();
    if (!accountData || !accountData.device) {
      return;
    }
    const localDeviceId = accountData.device.id;
    const isLocalDevice = deviceId == localDeviceId;
    if (isLocalDevice) {
      this.signOut(true);
    }
    const data = JSON.stringify({ isLocalDevice });
    await this.notifyObservers(ON_DEVICE_DISCONNECTED_NOTIFICATION, data);
  },

  async _handleEmailUpdated(newEmail) {
    await this.currentAccountState.updateUserAccountData({ email: newEmail });
  },

  _errorToErrorClass(aError) {
    if (aError.errno) {
      let error = SERVER_ERRNO_TO_ERROR[aError.errno];
      return this._error(
        ERROR_TO_GENERAL_ERROR_CLASS[error] || ERROR_UNKNOWN,
        aError
      );
    } else if (
      aError.message &&
      (aError.message === "INVALID_PARAMETER" ||
        aError.message === "NO_ACCOUNT" ||
        aError.message === "UNVERIFIED_ACCOUNT" ||
        aError.message === "AUTH_ERROR")
    ) {
      return aError;
    }
    return this._error(ERROR_UNKNOWN, aError);
  },

  _error(aError, aDetails) {
    const isExpected =
      aError === ERROR_NO_ACCOUNT || aError === ERROR_UNVERIFIED_ACCOUNT;
    const logFn = isExpected ? log.debug : log.error;
    if (aDetails) {
      logFn.call(
        log,
        "FxA rejecting with error ${aError}, details: ${aDetails}",
        { aError, aDetails }
      );
    } else {
      logFn.call(log, "FxA rejecting with error ${aError}", { aError });
    }
    let reason = new Error(aError);
    if (aDetails) {
      reason.details = aDetails;
    }
    return reason;
  },

  updateDeviceRegistration() {
    return this.device.updateDeviceRegistration();
  },

  dropCredentials(state) {
    let updateData = {};
    let clearField = field => {
      if (!FXA_PWDMGR_REAUTH_ALLOWLIST.has(field)) {
        updateData[field] = null;
      }
    };
    FXA_PWDMGR_PLAINTEXT_FIELDS.forEach(clearField);
    FXA_PWDMGR_SECURE_FIELDS.forEach(clearField);

    return state.updateUserAccountData(updateData);
  },

  async checkAccountStatus(state) {
    log.info("checking account status...");
    let data = await state.getUserAccountData(["uid", "sessionToken"]);
    if (!data) {
      log.info("account status: no user");
      return false;
    }
    if (data.sessionToken) {
      if (await this.fxAccountsClient.sessionStatus(data.sessionToken)) {
        log.info("account status: ok");
        return true;
      }
    }
    let exists = await this.fxAccountsClient.accountStatus(data.uid);
    if (!exists) {
      log.info("account status: deleted");
      await this._handleAccountDestroyed(data.uid);
    } else {
      log.info("account status: needs reauthentication");
      await this.dropCredentials(this.currentAccountState);
      await this.notifyObservers(ON_ACCOUNT_STATE_CHANGE_NOTIFICATION);
    }
    return false;
  },

  async _handleTokenError(err) {
    if (!err || err.code != 401 || err.errno != ERRNO_INVALID_AUTH_TOKEN) {
      throw err;
    }
    log.warn("handling invalid token error", err);
    let state = this.currentAccountState;
    let ok = await this.checkAccountStatus(state);
    if (ok) {
      log.warn("invalid token error, but account state appears ok?");
    }
    throw err;
  },
};

let fxAccountsSingleton = null;

export function getFxAccountsSingleton() {
  if (fxAccountsSingleton) {
    return fxAccountsSingleton;
  }

  fxAccountsSingleton = new FxAccounts();
  return fxAccountsSingleton;
}
