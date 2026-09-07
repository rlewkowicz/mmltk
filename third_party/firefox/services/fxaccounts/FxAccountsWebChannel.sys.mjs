/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

import {
  COMMAND_PROFILE_CHANGE,
  COMMAND_LOGIN,
  COMMAND_LOGOUT,
  COMMAND_OAUTH,
  COMMAND_DELETE,
  COMMAND_CAN_LINK_ACCOUNT,
  COMMAND_SYNC_PREFERENCES,
  COMMAND_CHANGE_PASSWORD,
  COMMAND_FXA_STATUS,
  COMMAND_PAIR_HEARTBEAT,
  COMMAND_PAIR_SUPP_METADATA,
  COMMAND_PAIR_AUTHORIZE,
  COMMAND_PAIR_DECLINE,
  COMMAND_PAIR_COMPLETE,
  COMMAND_PAIR_PREFERENCES,
  COMMAND_FIREFOX_VIEW,
  COMMAND_OAUTH_FLOW_IS_ACTIVE,
  COMMAND_OAUTH_FLOW_BEGIN,
  OAUTH_CLIENT_ID,
  ON_PROFILE_CHANGE_NOTIFICATION,
  ON_SERVICE_ENABLED_NOTIFICATION,
  PREF_LAST_FXA_USER_UID,
  PREF_LAST_FXA_USER_EMAIL,
  WEBCHANNEL_ID,
  log,
  logPII,
} from "resource://gre/modules/FxAccountsCommon.sys.mjs";
import { SyncDisconnect } from "resource://services-sync/SyncDisconnect.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  CryptoUtils: "moz-src:///services/crypto/modules/utils.sys.mjs",
  FxAccountsPairingFlow: "resource://gre/modules/FxAccountsPairing.sys.mjs",
  FxAccountsStorageManagerCanStoreField:
    "resource://gre/modules/FxAccountsStorage.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  Weave: "resource://services-sync/main.sys.mjs",
  WebChannel: "resource://gre/modules/WebChannel.sys.mjs",
});
ChromeUtils.defineLazyGetter(lazy, "SelectableProfileService", () => {
  try {
    return ChromeUtils.importESModule(
      "resource:///modules/profiles/SelectableProfileService.sys.mjs"
    ).SelectableProfileService;
  } catch (ex) {
    return null;
  }
});
ChromeUtils.defineLazyGetter(lazy, "fxAccounts", () => {
  return ChromeUtils.importESModule(
    "resource://gre/modules/FxAccounts.sys.mjs"
  ).getFxAccountsSingleton();
});
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "pairingEnabled",
  "identity.fxaccounts.pairing.enabled"
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "separatePrivilegedMozillaWebContentProcess",
  "browser.tabs.remote.separatePrivilegedMozillaWebContentProcess",
  false
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "separatedMozillaDomains",
  "browser.tabs.remote.separatedMozillaDomains",
  "",
  false,
  val => val.split(",")
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "accountServer",
  "identity.fxaccounts.remote.root",
  null,
  false,
  val => Services.io.newURI(val)
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "allowSyncMerge",
  "browser.profiles.sync.allow-danger-merge",
  false
);

ChromeUtils.defineLazyGetter(lazy, "l10n", function () {
  return new Localization(["browser/sync.ftl", "branding/brand.ftl"], true);
});

const CHOOSE_WHAT_TO_SYNC_ALWAYS_AVAILABLE = [
  "addons",
  "bookmarks",
  "history",
  "passwords",
  "prefs",
  "tabs",
];

const CHOOSE_WHAT_TO_SYNC_OPTIONALLY_AVAILABLE = ["addresses", "creditcards"];

function getErrorDetails(error) {
  let cleanMessage = String(error)
    .replace(/\\.*\\/gm, "[REDACTED]")
    .replace(/\/.*\//gm, "[REDACTED]");
  let details = { message: cleanMessage, stack: null };

  if (error.stack) {
    let frames = [];
    for (let frame = error.stack; frame; frame = frame.caller) {
      frames.push(String(frame).padStart(4));
    }
    details.stack = frames.join("\n");
  }

  return details;
}

export function FxAccountsWebChannel(options) {
  if (!options) {
    throw new Error("Missing configuration options");
  }
  if (!options.content_uri) {
    throw new Error("Missing 'content_uri' option");
  }
  this._contentUri = options.content_uri;

  if (!options.channel_id) {
    throw new Error("Missing 'channel_id' option");
  }
  this._webChannelId = options.channel_id;

  ChromeUtils.defineLazyGetter(this, "_helpers", () => {
    return options.helpers || new FxAccountsWebChannelHelpers(options);
  });

  this._setupChannel();
}

FxAccountsWebChannel.prototype = {
  _channel: null,

  _helpers: null,

  _webChannelId: null,
  _webChannelOrigin: null,

  _lastPromise: null,

  tearDown() {
    this._channel.stopListening();
    this._channel = null;
    this._channelCallback = null;
  },

  _setupChannel() {
    try {
      this._webChannelOrigin = Services.io.newURI(this._contentUri);
      this._registerChannel();
    } catch (e) {
      log.error(e);
      throw e;
    }
  },

  _receiveMessage(message, sendingContext) {
    log.trace(`_receiveMessage for command ${message.command}`);
    let shouldCheckRemoteType =
      lazy.separatePrivilegedMozillaWebContentProcess &&
      lazy.separatedMozillaDomains.some(function (val) {
        return (
          lazy.accountServer.asciiHost == val ||
          lazy.accountServer.asciiHost.endsWith("." + val)
        );
      });
    if (
      shouldCheckRemoteType &&
      sendingContext.remoteType != "privilegedmozilla"
    ) {
      log.error(
        `Rejected FxA webchannel message from remoteType = ${sendingContext.remoteType}`
      );
      return;
    }

    let lastPromise = this._lastPromise || Promise.resolve();
    this._lastPromise = lastPromise
      .then(() => {
        return this._promiseMessage(message, sendingContext);
      })
      .catch(e => {
        log.error("Handling webchannel message failed", e);
        this._sendError(e, message, sendingContext);
      })
      .finally(() => {
        this._lastPromise = null;
      });
  },

  async _promiseMessage(message, sendingContext) {
    const { command, data } = message;
    let browser = sendingContext.browsingContext.top.embedderElement;
    switch (command) {
      case COMMAND_PROFILE_CHANGE:
        Services.obs.notifyObservers(
          null,
          ON_PROFILE_CHANGE_NOTIFICATION,
          data.uid
        );
        break;
      case COMMAND_LOGIN:
        await this._helpers.login(data);
        await this._channel.send(
          { command, messageId: message.messageId, data: { ok: true } },
          sendingContext
        );
        break;
      case COMMAND_OAUTH:
        await this._helpers.oauthLogin(data);
        await this._channel.send(
          { command, messageId: message.messageId, data: { ok: true } },
          sendingContext
        );
        break;
      case COMMAND_OAUTH_FLOW_IS_ACTIVE: {
        const isActive = this._helpers.oauthFlowIsActive();
        await this._channel.send(
          { command, messageId: message.messageId, data: { isActive } },
          sendingContext
        );
        break;
      }
      case COMMAND_OAUTH_FLOW_BEGIN: {
        let params = await this._helpers.oauthBegin(data.scopes);
        await this._channel.send(
          { command, messageId: message.messageId, data: params },
          sendingContext
        );
        break;
      }
      case COMMAND_LOGOUT:
      case COMMAND_DELETE:
        await this._helpers.logout(data.uid);
        await this._channel.send(
          { command, messageId: message.messageId, data: { ok: true } },
          sendingContext
        );
        break;
      case COMMAND_CAN_LINK_ACCOUNT:
        {
          let response = { command, messageId: message.messageId };
          if (!this._helpers._selectableProfilesEnabled()) {
            response.data = { ok: this._helpers.shouldAllowRelink(data) };
            this._channel.send(response, sendingContext);
            break;
          }
          let result =
            await this._helpers.promptProfileSyncWarningIfNeeded(data);
          switch (result.action) {
            case "create-profile":
              lazy.SelectableProfileService.createNewProfile(
                true,
                null,
                "sync-warning"
              );
              response.data = { ok: false };
              break;
            case "switch-profile":
              lazy.SelectableProfileService.launchInstance(result.data);
              response.data = { ok: false };
              break;
            case "continue":
              response.data = { ok: true };
              break;
            case "cancel":
              response.data = { ok: false };
              break;
            default:
              log.error(
                "Invalid FxAccountsWebChannel dialog response: ",
                result.action
              );
              response.data = { ok: false };
              break;
          }
          log.debug("FxAccountsWebChannel response", response);
          this._channel.send(response, sendingContext);
        }
        break;
      case COMMAND_SYNC_PREFERENCES:
        this._helpers.openSyncPreferences(browser, data.entryPoint);
        this._channel.send(
          { command, messageId: message.messageId, data: { ok: true } },
          sendingContext
        );
        break;
      case COMMAND_PAIR_PREFERENCES:
        if (lazy.pairingEnabled) {
          let win = browser.documentGlobal;
          this._channel.send(
            { command, messageId: message.messageId, data: { ok: true } },
            sendingContext
          );
          win.openTrustedLinkIn(
            "about:preferences?action=pair#sync",
            "current"
          );
        }
        break;
      case COMMAND_FIREFOX_VIEW:
        this._helpers.openFirefoxView(browser, data.entryPoint);
        this._channel.send(
          { command, messageId: message.messageId, data: { ok: true } },
          sendingContext
        );
        break;
      case COMMAND_CHANGE_PASSWORD:
        await this._helpers.changePassword(data);
        await this._channel.send(
          { command, messageId: message.messageId, data: { ok: true } },
          sendingContext
        );
        break;
      case COMMAND_FXA_STATUS: {
        log.debug("fxa_status received");
        const service = data && data.service;
        const isPairing = data && data.isPairing;
        const context = data && data.context;
        await this._helpers
          .getFxaStatus(service, sendingContext, isPairing, context)
          .then(fxaStatus => {
            let response = {
              command,
              messageId: message.messageId,
              data: fxaStatus,
            };
            this._channel.send(response, sendingContext);
          });
        break;
      }
      case COMMAND_PAIR_HEARTBEAT:
      case COMMAND_PAIR_SUPP_METADATA:
      case COMMAND_PAIR_AUTHORIZE:
      case COMMAND_PAIR_DECLINE:
      case COMMAND_PAIR_COMPLETE: {
        log.debug(`Pairing command ${command} received`);
        const { channel_id: channelId } = data;
        delete data.channel_id;
        const flow = lazy.FxAccountsPairingFlow.get(channelId);
        if (!flow) {
          log.warn(`Could not find a pairing flow for ${channelId}`);
          return;
        }
        flow.onWebChannelMessage(command, data).then(replyData => {
          this._channel.send(
            {
              command,
              messageId: message.messageId,
              data: replyData,
            },
            sendingContext
          );
        });
        break;
      }
      default: {
        let errorMessage = "Unrecognized FxAccountsWebChannel command";
        log.warn(errorMessage, command);
        this._channel.send({
          command,
          messageId: message.messageId,
          data: { error: errorMessage },
        });
        lazy.FxAccountsPairingFlow.finalizeAll();
        break;
      }
    }
  },

  _sendError(error, incomingMessage, sendingContext) {
    log.error("Failed to handle FxAccountsWebChannel message", error);
    this._channel.send(
      {
        command: incomingMessage.command,
        messageId: incomingMessage.messageId,
        data: {
          error: getErrorDetails(error),
        },
      },
      sendingContext
    );
  },

  _registerChannel() {
    let listener = (webChannelId, message, sendingContext) => {
      if (message) {
        log.debug("FxAccountsWebChannel message received", message.command);
        if (logPII()) {
          log.debug("FxAccountsWebChannel message details", message);
        }
        try {
          this._receiveMessage(message, sendingContext);
        } catch (error) {
          log.error(
            "Unexpected webchannel error escaped from promise error handlers"
          );
          this._sendError(error, message, sendingContext);
        }
      }
    };

    this._channelCallback = listener;
    this._channel = new lazy.WebChannel(
      this._webChannelId,
      this._webChannelOrigin
    );
    this._channel.listen(listener);
    log.debug(
      "FxAccountsWebChannel registered: " +
        this._webChannelId +
        " with origin " +
        this._webChannelOrigin.prePath
    );
  },
};

export function FxAccountsWebChannelHelpers(options) {
  options = options || {};

  this._fxAccounts = options.fxAccounts || lazy.fxAccounts;
  this._weaveXPCOM = options.weaveXPCOM || null;
  this._privateBrowsingUtils =
    options.privateBrowsingUtils || lazy.PrivateBrowsingUtils;
}

FxAccountsWebChannelHelpers.prototype = {
  shouldAllowRelink(acctData) {
    return (
      !this._needRelinkWarning(acctData) ||
      this._promptForRelink(acctData.email)
    );
  },

  async promptProfileSyncWarningIfNeeded(acctData) {
    let profileLinkedWithAcct = acctData.uid
      ? await this._getProfileAssociatedWithAcct(acctData.uid)
      : null;
    if (this._needRelinkWarning(acctData) || profileLinkedWithAcct) {
      return this._promptForProfileSyncWarning(
        acctData.email,
        profileLinkedWithAcct
      );
    }
    return { action: "continue" };
  },

  async _initializeSync() {
    let xps =
      this._weaveXPCOM ||
      Cc["@mozilla.org/weave/service;1"].getService(Ci.nsISupports)
        .wrappedJSObject;
    await xps.whenLoaded();
    return xps;
  },

  _setEnabledEngines(offeredEngines, declinedEngines) {
    if (offeredEngines && declinedEngines) {
      log.debug("Received offered engines", offeredEngines);
      CHOOSE_WHAT_TO_SYNC_OPTIONALLY_AVAILABLE.forEach(engine => {
        if (
          offeredEngines.includes(engine) &&
          !declinedEngines.includes(engine)
        ) {
          log.debug(`Enabling optional engine '${engine}'`);
          Services.prefs.setBoolPref(`services.sync.engine.${engine}`, true);
        }
      });
      log.debug("Received declined engines", declinedEngines);
      lazy.Weave.Service.engineManager.setDeclined(declinedEngines);
      declinedEngines.forEach(engine => {
        Services.prefs.setBoolPref(`services.sync.engine.${engine}`, false);
      });
    } else {
      log.debug("Did not receive any engine selection information");
    }
  },

  async _enableRequestedServices(requestedServices) {
    if (!requestedServices) {
      log.warn(
        "fxa login completed but we don't have a record of which services were enabled."
      );
      return;
    }
    let services = Object.keys(requestedServices);
    log.debug(`services requested are ${services}`);
    if (requestedServices.sync) {
      const xps = await this._initializeSync();
      const { offeredEngines, declinedEngines } = requestedServices.sync;
      this._setEnabledEngines(offeredEngines, declinedEngines);
      log.debug("Webchannel is enabling sync");
      await xps.Weave.Service.configure();
    }
    for (let service of services) {
      Services.obs.notifyObservers(
        null,
        ON_SERVICE_ENABLED_NOTIFICATION,
        service
      );
    }
  },

  async login(accountData) {
    const signedInUser = await this._fxAccounts.getSignedInUser([
      "requestedServices",
    ]);
    let existingServices;
    if (signedInUser) {
      if (signedInUser.uid != accountData.uid) {
        log.warn(
          "the webchannel found a different user signed in - signing them out."
        );
        await this._disconnect();
      } else {
        existingServices = signedInUser.requestedServices
          ? JSON.parse(signedInUser.requestedServices)
          : {};
        log.debug(
          "Webchannel is updating the info for an already logged in user."
        );
      }
    } else {
      log.debug("Webchannel is logging new a user in.");
    }
    delete accountData.customizeSync;
    delete accountData.verifiedCanLinkAccount;
    delete accountData.keyFetchToken;
    delete accountData.unwrapBKey;

    const requestedServices = {
      ...(accountData.services ?? {}),
      ...existingServices,
    };
    delete accountData.services;
    log.debug(`storing info for services ${Object.keys(requestedServices)}`);
    accountData.requestedServices = JSON.stringify(requestedServices);

    this.setPreviousAccountHashPref(accountData.uid);

    if (signedInUser && signedInUser.uid === accountData.uid) {
      await this._fxAccounts._internal.updateUserAccountData(accountData);
      log.debug("Webchannel finished updating already logged in user.");
    } else {
      await this._fxAccounts._internal.setSignedInUser(accountData);
      log.debug("Webchannel finished logging a user in.");
    }
  },

  async oauthLogin(oauthData) {
    log.debug("Webchannel is completing the oauth flow");
    const { uid, sessionToken, requestedServices } =
      await this._fxAccounts._internal.getUserAccountData([
        "uid",
        "sessionToken",
        "requestedServices",
      ]);
    const { scopedKeys, refreshToken } =
      await this._fxAccounts._internal.completeOAuthFlow(
        sessionToken,
        oauthData.code,
        oauthData.state
      );

    await this._fxAccounts._internal.destroyOAuthToken({ token: refreshToken });

    this.setPreviousAccountHashPref(uid);

    if (!scopedKeys) {
      log.info(
        "OAuth login completed without scoped keys; skipping Sync key storage"
      );
    } else {
      await this._fxAccounts._internal.setScopedKeys(scopedKeys);
    }

    try {
      let parsedRequestedServices;
      if (requestedServices) {
        parsedRequestedServices = JSON.parse(requestedServices);
      }
      await this._enableRequestedServices(parsedRequestedServices);
    } finally {
      await this._fxAccounts._internal.updateUserAccountData({
        uid,
        requestedServices: null,
      });
    }

    await this._fxAccounts._internal.setUserVerified();
    log.debug("Webchannel completed oauth flows");
  },

  async oauthBegin(scopes) {
    log.debug(`Webchannel is starting a new oauth flow for scopes ${scopes}`);
    return await this._fxAccounts._internal.oauth.beginOAuthFlow(scopes);
  },

  oauthFlowIsActive() {
    return this._fxAccounts._internal.oauth.numOfFlows() != 0;
  },

  _disconnect() {
    return SyncDisconnect.disconnect(false);
  },

  async logout(uid) {
    let fxa = this._fxAccounts;
    let userData = await fxa._internal.getUserAccountData(["uid"]);
    if (userData && userData.uid === uid) {
      await fxa.signOut(true);
    }
  },

  isPrivateBrowsingMode(sendingContext) {
    if (!sendingContext) {
      log.error(
        "Unable to check for private browsing mode (no sending context), assuming true"
      );
      return true;
    }

    let browser = sendingContext.browsingContext.top.embedderElement;
    if (!browser) {
      log.error(
        "Unable to check for private browsing mode (no browser), assuming true"
      );
      return true;
    }
    const isPrivateBrowsing =
      this._privateBrowsingUtils.isBrowserPrivate(browser);
    return isPrivateBrowsing;
  },

  shouldAllowFxaStatus(service, sendingContext, isPairing, context) {
    let pb = this.isPrivateBrowsingMode(sendingContext);
    let ok = !pb || service === "sync" || isPairing;
    log.debug(
      `fxa status ok=${ok} - private=${pb}, service=${service}, context=${context}, pairing=${isPairing}`
    );
    return ok;
  },

  async getFxaStatus(service, sendingContext, isPairing, context) {
    let signedInUser = null;

    if (
      this.shouldAllowFxaStatus(service, sendingContext, isPairing, context)
    ) {
      const userData = await this._fxAccounts._internal.getUserAccountData([
        "email",
        "sessionToken",
        "uid",
        "verified",
      ]);
      if (userData) {
        signedInUser = {
          email: userData.email,
          sessionToken: userData.sessionToken,
          uid: userData.uid,
          verified: userData.verified,
        };
      }
    }

    const capabilities = this._getCapabilities();

    return {
      signedInUser,
      clientId: OAUTH_CLIENT_ID,
      capabilities,
    };
  },

  _getCapabilities() {
    let engines = Array.from(CHOOSE_WHAT_TO_SYNC_ALWAYS_AVAILABLE);
    for (let optionalEngine of CHOOSE_WHAT_TO_SYNC_OPTIONALLY_AVAILABLE) {
      if (
        Services.prefs.getBoolPref(
          `services.sync.engine.${optionalEngine}.available`,
          false
        )
      ) {
        engines.push(optionalEngine);
      }
    }
    return {
      multiService: true,
      pairing: lazy.pairingEnabled,
      choose_what_to_sync: true,
      keys_optional: true,
      can_link_account_uid: true,
      engines,
    };
  },

  async changePassword(credentials) {
    let newCredentials = {
      device: null, 
      encryptedSendTabKeys: null,
    };
    for (let name of Object.keys(credentials)) {
      if (
        name == "email" ||
        name == "uid" ||
        lazy.FxAccountsStorageManagerCanStoreField(name)
      ) {
        newCredentials[name] = credentials[name];
      } else {
        log.info("changePassword ignoring unsupported field", name);
      }
    }
    await this._fxAccounts._internal.updateUserAccountData(newCredentials);
    await this._fxAccounts._internal.updateDeviceRegistration();
  },

  setPreviousAccountHashPref(uid) {
    if (!uid) {
      throw new Error("No uid specified");
    }
    Services.prefs.setStringPref(
      PREF_LAST_FXA_USER_UID,
      lazy.CryptoUtils.sha256Base64(uid)
    );
    Services.prefs.clearUserPref(PREF_LAST_FXA_USER_EMAIL);
  },

  openSyncPreferences(browser, entryPoint) {
    let uri = "about:preferences";
    if (entryPoint) {
      uri += "?entrypoint=" + encodeURIComponent(entryPoint);
    }
    uri += "#sync";

    browser.loadURI(Services.io.newURI(uri), {
      triggeringPrincipal: Services.scriptSecurityManager.getSystemPrincipal(),
    });
  },

  openFirefoxView(browser) {
    browser.documentGlobal.FirefoxViewHandler.openTab("syncedtabs");
  },

  _needRelinkWarning(acctData) {
    const lastUid = Services.prefs.getStringPref(PREF_LAST_FXA_USER_UID, "");
    if (lastUid) {
      return (
        !acctData.uid || lastUid != lazy.CryptoUtils.sha256Base64(acctData.uid)
      );
    }

    const lastEmail = Services.prefs.getStringPref(
      PREF_LAST_FXA_USER_EMAIL,
      ""
    );
    return (
      lastEmail && lastEmail != lazy.CryptoUtils.sha256Base64(acctData.email)
    );
  },

  _selectableProfilesEnabled() {
    return (
      lazy.SelectableProfileService?.isEnabled &&
      lazy.SelectableProfileService?.hasCreatedSelectableProfiles()
    );
  },

  _getCurrentProfileName() {
    return lazy.SelectableProfileService?.currentProfile?.name;
  },

  async _getAllProfiles() {
    return await lazy.SelectableProfileService.getAllProfiles();
  },

  async _getProfileAssociatedWithAcct(acctUid) {
    let profiles = await this._getAllProfiles();
    let currentProfileName = await this._getCurrentProfileName();
    for (let profile of profiles) {
      if (profile.name === currentProfileName) {
        continue; 
      }

      let profilePath = profile.path;
      let signedInUserPath = PathUtils.join(profilePath, "signedInUser.json");
      let signedInUser = await this._readJSONFileAsync(signedInUserPath);
      if (
        signedInUser?.accountData &&
        signedInUser.accountData.uid === acctUid
      ) {
        return profile;
      }
    }
    return null;
  },

  async _readJSONFileAsync(filePath) {
    try {
      let data = await IOUtils.readJSON(filePath);
      if (data && data.version !== 1) {
        throw new Error(
          `Unsupported signedInUser.json version: ${data.version}`
        );
      }
      return data;
    } catch (e) {
      return null;
    }
  },

  _promptForRelink(acctEmail) {
    let [continueLabel, title, heading, description] =
      lazy.l10n.formatValuesSync([
        { id: "sync-setup-verify-continue" },
        { id: "sync-setup-verify-title" },
        { id: "sync-setup-verify-heading" },
        {
          id: "sync-setup-verify-description",
          args: {
            email: acctEmail,
          },
        },
      ]);
    let body = heading + "\n\n" + description;
    let ps = Services.prompt;
    let buttonFlags =
      ps.BUTTON_POS_0 * ps.BUTTON_TITLE_IS_STRING +
      ps.BUTTON_POS_1 * ps.BUTTON_TITLE_CANCEL +
      ps.BUTTON_POS_1_DEFAULT;

    let pressed = Services.prompt.confirmEx(
      null,
      title,
      body,
      buttonFlags,
      continueLabel,
      null,
      null,
      null,
      {}
    );
    return pressed === 0; 
  },

  _promptForProfileSyncWarning(acctEmail, profileLinkedWithAcct) {
    let currentProfile = this._getCurrentProfileName();
    let title, heading, description, mergeLabel, switchLabel;
    if (profileLinkedWithAcct) {
      [title, heading, description, mergeLabel, switchLabel] =
        lazy.l10n.formatValuesSync([
          { id: "sync-account-in-use-header" },
          {
            id: lazy.allowSyncMerge
              ? "sync-account-already-signed-in-header"
              : "sync-account-in-use-header-merge",
            args: {
              acctEmail,
              otherProfile: profileLinkedWithAcct.name,
            },
          },
          {
            id: lazy.allowSyncMerge
              ? "sync-account-in-use-description-merge"
              : "sync-account-in-use-description",
            args: {
              acctEmail,
              currentProfile,
              otherProfile: profileLinkedWithAcct.name,
            },
          },
          {
            id: "sync-button-sync-profile",
            args: { profileName: currentProfile },
          },
          {
            id: "sync-button-switch-profile",
            args: { profileName: profileLinkedWithAcct.name },
          },
        ]);
    } else {
      [title, heading, description, mergeLabel, switchLabel] =
        lazy.l10n.formatValuesSync([
          {
            id: lazy.allowSyncMerge
              ? "sync-profile-different-account-title-merge"
              : "sync-profile-different-account-title",
          },
          {
            id: "sync-profile-different-account-header",
          },
          {
            id: lazy.allowSyncMerge
              ? "sync-profile-different-account-description-merge"
              : "sync-profile-different-account-description",
            args: {
              acctEmail,
              profileName: currentProfile,
            },
          },
          { id: "sync-button-sync-and-merge" },
          { id: "sync-button-create-profile" },
        ]);
    }
    let result = this.showWarningPrompt({
      title,
      body: `${heading}\n\n${description}`,
      btnLabel1: lazy.allowSyncMerge ? mergeLabel : switchLabel,
      btnLabel2: lazy.allowSyncMerge ? switchLabel : null,
      isAccountLoggedIntoAnotherProfile: !!profileLinkedWithAcct,
    });

    if (result === "switch-profile") {
      return { action: result, data: profileLinkedWithAcct };
    }

    return { action: result };
  },

  showWarningPrompt({
    title,
    body,
    btnLabel1,
    btnLabel2,
    isAccountLoggedIntoAnotherProfile,
  }) {
    let ps = Services.prompt;
    let buttonFlags;
    let pressed;
    let actionMap = {};

    if (lazy.allowSyncMerge) {
      buttonFlags =
        ps.BUTTON_POS_0 * ps.BUTTON_TITLE_IS_STRING +
        ps.BUTTON_POS_1 * ps.BUTTON_TITLE_IS_STRING +
        ps.BUTTON_POS_2 * ps.BUTTON_TITLE_CANCEL +
        ps.BUTTON_POS_2_DEFAULT;

      if (isAccountLoggedIntoAnotherProfile) {
        actionMap = {
          0: "continue", 
          1: "switch-profile",
          2: "cancel",
        };
      } else {
        actionMap = {
          0: "continue", 
          1: "create-profile",
          2: "cancel",
        };
      }

      pressed = ps.confirmEx(
        null,
        title,
        body,
        buttonFlags,
        btnLabel1,
        btnLabel2,
        null,
        null,
        {}
      );
    } else {
      buttonFlags =
        ps.BUTTON_POS_0 * ps.BUTTON_TITLE_IS_STRING +
        ps.BUTTON_POS_1 * ps.BUTTON_TITLE_CANCEL +
        ps.BUTTON_POS_1_DEFAULT;

      if (isAccountLoggedIntoAnotherProfile) {
        actionMap = {
          0: "switch-profile",
          1: "cancel",
        };
      } else {
        actionMap = {
          0: "create-profile",
          1: "cancel",
        };
      }

      pressed = ps.confirmEx(
        null,
        title,
        body,
        buttonFlags,
        btnLabel1,
        null,
        null,
        null,
        {}
      );
    }

    return actionMap[pressed] || "unknown";
  },
};

var singleton;

export var EnsureFxAccountsWebChannel = () => {
  let contentUri = Services.urlFormatter.formatURLPref(
    "identity.fxaccounts.remote.root"
  );
  if (singleton && singleton._contentUri !== contentUri) {
    singleton.tearDown();
    singleton = null;
  }
  if (!singleton) {
    try {
      if (contentUri) {
        singleton = new FxAccountsWebChannel({
          content_uri: contentUri,
          channel_id: WEBCHANNEL_ID,
        });
      } else {
        log.warn("FxA WebChannel functionaly is disabled due to no URI pref.");
      }
    } catch (ex) {
      log.error("Failed to create FxA WebChannel", ex);
    }
  }
};
