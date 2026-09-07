/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import {
  CLIENT_IS_THUNDERBIRD,
  COMMAND_SENDTAB,
  COMMAND_SENDTAB_TAIL,
  COMMAND_CLOSETAB,
  COMMAND_CLOSETAB_TAIL,
  SCOPE_OLD_SYNC,
  log,
} from "resource://gre/modules/FxAccountsCommon.sys.mjs";

import { clearTimeout, setTimeout } from "resource://gre/modules/Timer.sys.mjs";

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

import { Observers } from "resource://services-common/observers.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AsyncShutdown: "resource://gre/modules/AsyncShutdown.sys.mjs",
  BulkKeyBundle: "resource://services-sync/keys.sys.mjs",
  CryptoWrapper: "resource://services-sync/record.sys.mjs",
  PushCrypto: "resource://gre/modules/PushCrypto.sys.mjs",
  getRemoteCommandStore: "resource://services-sync/TabsStore.sys.mjs",
  RemoteCommand: "resource://services-sync/TabsStore.sys.mjs",
  Resource: "resource://services-sync/resource.sys.mjs",
  Utils: "resource://services-sync/util.sys.mjs",
  NimbusFeatures: "resource://nimbus/ExperimentAPI.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "INVALID_SHAREABLE_SCHEMES",
  "services.sync.engine.tabs.filteredSchemes",
  "",
  null,
  val => {
    return new Set(val.split("|"));
  }
);

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "idleService",
  "@mozilla.org/widget/useridleservice;1",
  Ci.nsIUserIdleService
);

const TOPIC_TABS_CHANGED = "services.sync.tabs.changed";
const COMMAND_MAX_PAYLOAD_SIZE = 16 * 1024;

export class FxAccountsCommands {
  constructor(fxAccountsInternal) {
    this._fxai = fxAccountsInternal;
    this.sendTab = new SendTab(this, fxAccountsInternal);
    this.closeTab = new CloseRemoteTab(this, fxAccountsInternal);
    this.commandQueue = new CommandQueue(this, fxAccountsInternal);
    this._invokeRateLimitExpiry = 0;
  }

  async availableCommands() {
    let commands = {};

    if (!CLIENT_IS_THUNDERBIRD) {
      const encryptedSendTabKeys = await this.sendTab.getEncryptedCommandKeys();

      if (encryptedSendTabKeys) {
        commands[COMMAND_SENDTAB] = encryptedSendTabKeys;
      }

      const encryptedCloseTabKeys =
        await this.closeTab.getEncryptedCommandKeys();
      if (encryptedCloseTabKeys) {
        commands[COMMAND_CLOSETAB] = encryptedCloseTabKeys;
      }
    }

    return commands;
  }

  async invoke(command, device, payload) {
    const { sessionToken } = await this._fxai.getUserAccountData([
      "sessionToken",
    ]);
    const client = this._fxai.fxAccountsClient;
    const now = Date.now();
    if (now < this._invokeRateLimitExpiry) {
      const remaining = (this._invokeRateLimitExpiry - now) / 1000;
      throw new Error(
        `Invoke for ${command} is rate-limited for ${remaining} seconds.`
      );
    }
    try {
      let info = await client.invokeCommand(
        sessionToken,
        command,
        device.id,
        payload
      );
      if (!info.enqueued || !info.notified) {
        log.error("Sending was only partially successful", info);
      } else {
        log.info("Successfully sent", info);
      }
    } catch (err) {
      if (err.code && err.code === 429 && err.retryAfter) {
        this._invokeRateLimitExpiry = Date.now() + err.retryAfter * 1000;
      }
      throw err;
    }
    log.info(`Payload sent to device ${device.id}.`);
  }

  async pollDeviceCommands(notifiedIndex = 0) {
    log.info(`Polling device commands.`);
    await this._fxai.withCurrentAccountState(async state => {
      const { device } = await state.getUserAccountData(["device"]);
      if (!device) {
        throw new Error("No device registration.");
      }
      const lastCommandIndex = device.lastCommandIndex + 1 || notifiedIndex;
      if (notifiedIndex > 0 && notifiedIndex < lastCommandIndex) {
        return;
      }
      const { index, messages } =
        await this._fetchDeviceCommands(lastCommandIndex);
      if (messages.length) {
        await state.updateUserAccountData({
          device: { ...device, lastCommandIndex: index },
        });
        log.info(`Handling ${messages.length} messages`);
        await this._handleCommands(messages, notifiedIndex);
      }
    });
    return true;
  }

  async _fetchDeviceCommands(index, limit = null) {
    const userData = await this._fxai.getUserAccountData();
    if (!userData) {
      throw new Error("No user.");
    }
    const { sessionToken } = userData;
    if (!sessionToken) {
      throw new Error("No session token.");
    }
    const client = this._fxai.fxAccountsClient;
    const opts = { index };
    if (limit != null) {
      opts.limit = limit;
    }
    return client.getCommands(sessionToken, opts);
  }

  _getReason(notifiedIndex, messageIndex) {
    if (notifiedIndex == 0) {
      return "poll";
    } else if (notifiedIndex > messageIndex) {
      return "push-missed";
    }
    return "push";
  }

  async _handleCommands(messages, notifiedIndex) {
    try {
      await this._fxai.device.refreshDeviceList();
    } catch (e) {
      log.warn("Error refreshing device list", e);
    }
    const tabsReceived = [];
    const tabsToClose = [];
    for (const { index, data } of messages) {
      const { command, payload, sender: senderId } = data;
      const reason = this._getReason(notifiedIndex, index);
      const sender =
        senderId && this._fxai.device.recentDeviceList
          ? this._fxai.device.recentDeviceList.find(d => d.id == senderId)
          : null;
      if (!sender) {
        log.warn(
          "Incoming command is from an unknown device (maybe disconnected?)"
        );
      }
      switch (command) {
        case COMMAND_CLOSETAB:
          try {
            const { urls } = await this.closeTab.handleTabClose(
              senderId,
              payload,
              reason
            );
            log.info(
              `Close Tab received with FxA commands: "${urls.length} tabs"
               from ${sender ? sender.name : "Unknown device"}.`
            );
            log.trace(`Close Remote Tabs received URLs: ${urls}`);
            tabsToClose.push({ urls, sender });
          } catch (e) {
            log.error(`Error while handling incoming Close Tab payload.`, e);
          }
          break;
        case COMMAND_SENDTAB:
          try {
            const {
              title,
              uri,
              private: isPrivate,
            } = await this.sendTab.handle(senderId, payload, reason);
            log.info(
              `Tab received with FxA commands: "${
                title || "<no title>"
              }" (private=${isPrivate}) from ${sender ? sender.name : "Unknown device"}.`
            );
            log.trace(`Tab received URL: ${uri}`);
            const scheme = Services.io.newURI(uri).scheme;
            if (lazy.INVALID_SHAREABLE_SCHEMES.has(scheme)) {
              throw new Error("Invalid scheme found for received URI.");
            }
            tabsReceived.push({ title, uri, private: isPrivate, sender });
          } catch (e) {
            log.error(`Error while handling incoming Send Tab payload.`, e);
          }
          break;
        default:
          log.info(`Unknown command: ${command}.`);
      }
    }
    if (tabsReceived.length) {
      this._notifyFxATabsReceived(tabsReceived);
    }
    if (tabsToClose.length) {
      this._notifyFxATabsClosed(tabsToClose);
    }
  }

  _notifyFxATabsReceived(tabsReceived) {
    Observers.notify("fxaccounts:commands:open-uri", tabsReceived);
  }

  _notifyFxATabsClosed(tabsToClose) {
    Observers.notify("fxaccounts:commands:close-uri", tabsToClose);
  }
}


export class Command {
  constructor(commands, fxAccountsInternal) {
    this._commands = commands;
    this._fxai = fxAccountsInternal;
  }

  deviceCapability; 
  keyFieldName; 
  encryptedKeyFieldName; 

  isDeviceCompatible(device) {
    return (
      device.availableCommands &&
      device.availableCommands[this.deviceCapability]
    );
  }

  async _encrypt(bytes, device) {
    let bundle = device.availableCommands[this.deviceCapability];
    if (!bundle) {
      throw new Error(`Device ${device.id} does not have send tab keys.`);
    }
    const oldsyncKey = await this._fxai.keys.getKeyForScope(SCOPE_OLD_SYNC);
    const ourKid = this._fxai.keys.kidAsHex(oldsyncKey);
    const { kid: theirKid } = JSON.parse(
      device.availableCommands[this.deviceCapability]
    );
    if (theirKid != ourKid) {
      throw new Error("Target Send Tab key ID is different from ours");
    }
    const json = JSON.parse(bundle);
    const wrapper = new lazy.CryptoWrapper();
    wrapper.deserialize({ payload: json });
    const syncKeyBundle = lazy.BulkKeyBundle.fromJWK(oldsyncKey);
    let { publicKey, authSecret } = await wrapper.decrypt(syncKeyBundle);
    authSecret = urlsafeBase64Decode(authSecret);
    publicKey = urlsafeBase64Decode(publicKey);

    const { ciphertext: encrypted } = await lazy.PushCrypto.encrypt(
      bytes,
      publicKey,
      authSecret
    );
    return urlsafeBase64Encode(encrypted);
  }

  async _decrypt(ciphertext) {
    let { privateKey, publicKey, authSecret } =
      await this._getPersistedCommandKeys();
    publicKey = urlsafeBase64Decode(publicKey);
    authSecret = urlsafeBase64Decode(authSecret);
    ciphertext = new Uint8Array(urlsafeBase64Decode(ciphertext));
    return lazy.PushCrypto.decrypt(
      privateKey,
      publicKey,
      authSecret,
      { encoding: "aes128gcm" },
      ciphertext
    );
  }

  async _getPersistedCommandKeys() {
    const { device } = await this._fxai.getUserAccountData(["device"]);
    return device && device[this.keyFieldName];
  }

  async _generateAndPersistCommandKeys() {
    let [publicKey, privateKey] = await lazy.PushCrypto.generateKeys();
    publicKey = urlsafeBase64Encode(publicKey);
    let authSecret = lazy.PushCrypto.generateAuthenticationSecret();
    authSecret = urlsafeBase64Encode(authSecret);
    const sendTabKeys = {
      publicKey,
      privateKey,
      authSecret,
    };
    await this._fxai.withCurrentAccountState(async state => {
      const { device = {} } = await state.getUserAccountData(["device"]);
      device[this.keyFieldName] = sendTabKeys;
      log.trace(
        `writing to ${this.keyFieldName} for command ${this.deviceCapability}`
      );
      await state.updateUserAccountData({
        device,
      });
    });
    return sendTabKeys;
  }

  async _getPersistedEncryptedCommandKey() {
    const data = await this._fxai.getUserAccountData([
      this.encryptedKeyFieldName,
    ]);
    return data[this.encryptedKeyFieldName];
  }

  async _generateAndPersistEncryptedCommandKey() {
    if (!(await this._fxai.keys.canGetKeyForScope(SCOPE_OLD_SYNC))) {
      log.info("Can't fetch keys, so unable to determine command keys");
      return null;
    }
    let sendTabKeys = await this._getPersistedCommandKeys();
    if (!sendTabKeys) {
      log.info("Could not find command keys, generating them");
      sendTabKeys = await this._generateAndPersistCommandKeys();
    }
    const keyToEncrypt = {
      publicKey: sendTabKeys.publicKey,
      authSecret: sendTabKeys.authSecret,
    };
    let oldsyncKey;
    try {
      oldsyncKey = await this._fxai.keys.getKeyForScope(SCOPE_OLD_SYNC);
    } catch (ex) {
      log.warn("Failed to fetch keys, so unable to determine command keys", ex);
      return null;
    }
    const wrapper = new lazy.CryptoWrapper();
    wrapper.cleartext = keyToEncrypt;
    const keyBundle = lazy.BulkKeyBundle.fromJWK(oldsyncKey);
    await wrapper.encrypt(keyBundle);
    const encryptedSendTabKeys = JSON.stringify({
      kid: this._fxai.keys.kidAsHex(oldsyncKey),
      IV: wrapper.IV,
      hmac: wrapper.hmac,
      ciphertext: wrapper.ciphertext,
    });
    await this._fxai.withCurrentAccountState(async state => {
      let data = {};
      data[this.encryptedKeyFieldName] = encryptedSendTabKeys;
      await state.updateUserAccountData(data);
    });
    return encryptedSendTabKeys;
  }

  async getEncryptedCommandKeys() {
    log.trace("Getting command keys", this.deviceCapability);
    let encryptedSendTabKeys = await this._getPersistedEncryptedCommandKey();
    const sendTabKeys = await this._getPersistedCommandKeys();
    if (!encryptedSendTabKeys || !sendTabKeys) {
      log.info(
        `Generating and persisting encrypted key (${!!encryptedSendTabKeys}, ${!!sendTabKeys})`
      );
      encryptedSendTabKeys =
        await this._generateAndPersistEncryptedCommandKey();
    }
    return encryptedSendTabKeys;
  }
}

export class SendTab extends Command {
  deviceCapability = COMMAND_SENDTAB;
  keyFieldName = "sendTabKeys";
  encryptedKeyFieldName = "encryptedSendTabKeys";

  async send(to, tab) {
    log.info(
      `Sending a ${tab.private ? "private " : ""}tab to ${to.length} devices.`
    );
    const flowID = crypto.randomUUID();
    const encoder = new TextEncoder();
    const data = {
      entries: [{ title: tab.title, url: tab.url, private: tab.private }],
    };
    const report = {
      succeeded: [],
      failed: [],
    };
    for (let device of to) {
      try {
        const streamID = crypto.randomUUID();
        const targetData = Object.assign({ flowID, streamID }, data);
        const bytes = encoder.encode(JSON.stringify(targetData));
        const encrypted = await this._encrypt(bytes, device);
        const payload = { encrypted };
        await this._commands.invoke(COMMAND_SENDTAB, device, payload);
        report.succeeded.push(device);
      } catch (error) {
        log.error("Error while invoking a send tab command.", error);
        report.failed.push({ device, error });
      }
    }
    return report;
  }

  async handle(senderID, { encrypted }, reason) {
    const bytes = await this._decrypt(encrypted);
    const decoder = new TextDecoder("utf8");
    const data = JSON.parse(decoder.decode(bytes));
    const { flowID, streamID, entries } = data;
    const current = data.hasOwnProperty("current")
      ? data.current
      : entries.length - 1;
    const { title, url: uri, private: isPrivate } = entries[current];

    return {
      title,
      uri,
      private: isPrivate,
    };
  }
}

export class CloseRemoteTab extends Command {
  deviceCapability = COMMAND_CLOSETAB;
  keyFieldName = "closeTabKeys";
  encryptedKeyFieldName = "encryptedCloseTabKeys";

  async sendCloseTabsCommand(target, urls, flowID) {
    log.info(`Sending tab closures to ${target.id} device.`);
    const encoder = new TextEncoder();
    try {
      const streamID = crypto.randomUUID();
      const targetData = { flowID, streamID, urls };
      const bytes = encoder.encode(JSON.stringify(targetData));
      const encrypted = await this._encrypt(bytes, target);
      const payload = { encrypted };
      await this._commands.invoke(COMMAND_CLOSETAB, target, payload);
      return true;
    } catch (error) {
      log.error("Error while invoking a send tab command.", error);
      return false;
    }
  }

  isDeviceCompatible(device) {
    return (
      lazy.NimbusFeatures.remoteTabManagement.getVariable("closeTabsEnabled") &&
      super.isDeviceCompatible(device)
    );
  }

  async handleTabClose(senderID, { encrypted }, reason) {
    const bytes = await this._decrypt(encrypted);
    const decoder = new TextDecoder("utf8");
    const data = JSON.parse(decoder.decode(bytes));
    const { flowID, streamID, urls } = data;

    return {
      urls,
    };
  }
}

export class CommandQueue {
  DELAY = 5000;

  #timer = null;

  #onShutdownBound = null;

  closeTabNotificationCount = 0;
  hasPendingCloseTabNotification = false;

  #idleThresholdSeconds = 3;
  #isObservingTabSyncs = false;
  #flushQueuePromise = null;

  constructor(commands, fxAccountsInternal) {
    this._commands = commands;
    this._fxai = fxAccountsInternal;
    Services.obs.addObserver(this, "services.sync.tabs.command-queued");
    Services.obs.addObserver(this, "weave:engine:sync:finish");
    this.#isObservingTabSyncs = true;
    log.trace("Command queue observer created");
    this.#onShutdownBound = this.#onShutdown.bind(this);
    lazy.AsyncShutdown.appShutdownConfirmed.addBlocker(
      "FxAccountsCommands: flush command queue",
      this.#onShutdownBound
    );
  }

  shutdown() {
    if (this.#timer) {
      clearTimeout(this.#timer);
    }
    Services.obs.removeObserver(this, "services.sync.tabs.command-queued");
    if (this.#isObservingTabSyncs) {
      Services.obs.removeObserver(this, "weave:engine:sync:finish");
      this.#isObservingTabSyncs = false;
    }
    lazy.AsyncShutdown.appShutdownConfirmed.removeBlocker(
      this.#onShutdownBound
    );
    this.#onShutdownBound = null;
  }

  observe(subject, topic, data) {
    log.trace(
      `CommandQueue observed topic=${topic}, data=${data}, subject=${subject}`
    );
    switch (topic) {
      case "services.sync.tabs.command-queued":
        this.flushQueue().catch(e => {
          log.error("Failed to flush the outgoing queue", e);
        });
        break;

      case "weave:engine:sync:finish":
        if (data != "tabs") {
          return;
        }
        Services.obs.removeObserver(this, "weave:engine:sync:finish");
        this.#isObservingTabSyncs = false;
        this.#checkQueueAfterStartup();
        break;

      default:
        log.error(`unexpected observer topic: ${topic}`);
    }
  }

  _getIdleService() {
    return lazy.idleService;
  }

  async #checkQueueAfterStartup() {
    const idleService = this._getIdleService();
    const idleObserver = () => {
      idleService.removeIdleObserver(idleObserver, this.#idleThresholdSeconds);
      log.info("checking if the command queue is empty now we are idle");
      this.flushQueue()
        .then(didSend => {
          log.info(
            `pending command check had ${didSend ? "some" : "no"} commands`
          );
        })
        .catch(err => {
          log.error(
            "Checking for pending tab commands after first tab sync failed",
            err
          );
        });
    };
    idleService.addIdleObserver(idleObserver, this.#idleThresholdSeconds);
  }

  async flushQueue(isForShutdown = false) {
    if (this.#flushQueuePromise == null) {
      this.#flushQueuePromise = this.#flushQueue(isForShutdown);
    }
    try {
      return await this.#flushQueuePromise;
    } finally {
      this.#flushQueuePromise = null;
    }
  }

  async #flushQueue(isForShutdown) {
    let store = await lazy.getRemoteCommandStore();
    let pending = await store.getUnsentCommands();
    log.trace("flushQueue total queued items", pending.length);
    let now = this.now();
    const mustSendThreshold = isForShutdown ? Infinity : now - this.DELAY;
    const canSendThreshold = isForShutdown ? Infinity : now - this.DELAY * 2;
    let recentDevices = this._fxai.device.recentDeviceList;
    if (!recentDevices.length) {
      log.error("No devices available for queued tab commands");
      return false;
    }
    let deviceMap = new Map(recentDevices.map(d => [d.id, d]));
    let byDevice = Map.groupBy(pending, c => c.deviceId);
    let nextTime = Infinity;
    let didSend = false;
    for (let [deviceId, commands] of byDevice) {
      let device = deviceMap.get(deviceId);
      if (!device) {
        log.warn("Unknown device for queued tab commands", deviceId);
        await Promise.all(
          commands.map(command => store.removeRemoteCommand(deviceId, command))
        );
        continue;
      }
      let toSend = [];
      for (const command of commands.reverse()) {
        if (!(command.command instanceof lazy.RemoteCommand.CloseTab)) {
          log.error(`Ignoring unknown pending command ${command}`);
          continue;
        }
        if (command.timeRequested <= mustSendThreshold) {
          log.trace(
            `command for url ${command.command.url} is overdue, adding to send`
          );
          toSend.push(command);
        } else if (command.timeRequested <= canSendThreshold) {
          if (toSend.length) {
            log.trace(`command for url ${command.command.url} is due,
              since there others to be sent, also adding to send`);
            toSend.push(command);
          } else {
            nextTime = Math.min(nextTime, command.timeRequested + this.DELAY);
          }
        } else {
          nextTime = Math.min(
            nextTime,
            command.timeRequested + this.DELAY * 1.1
          );
          break;
        }
      }

      if (toSend.length) {
        let urlsToClose = toSend.map(c => c.command.url);
        const flowID = crypto.randomUUID();
        let chunks = this.chunkUrls(urlsToClose, COMMAND_MAX_PAYLOAD_SIZE);
        for (let chunk of chunks) {
          if (
            await this._commands.closeTab.sendCloseTabsCommand(
              device,
              chunk,
              flowID
            )
          ) {
            const urlChunkSet = new Set(chunk);
            for (let cmd of toSend.filter(c =>
              urlChunkSet.has(c.command.url)
            )) {
              log.trace(
                `Setting pending command for device ${deviceId} as sent`,
                cmd
              );
              await store.setPendingCommandSent(cmd);
              didSend = true;
            }
          } else {
            log.warn(
              `Failed to send close tab commands for device ${deviceId}`
            );
            nextTime = Math.min(nextTime, now + 60000);
          }
        }
      } else {
        log.trace(`Skipping send for device ${deviceId}`);
      }
    }

    if (didSend) {
      Services.obs.notifyObservers(null, TOPIC_TABS_CHANGED);
    }

    if (nextTime == Infinity) {
      log.info("No new close-tab timer needed");
    } else if (isForShutdown) {
      log.error(
        "logic error in command queue manager: flush for shutdown should never set a timer"
      );
    } else {
      let delay = nextTime - now + 10;
      log.trace(`Setting new close-tab timer for ${delay}ms`);
      this._ensureTimer(delay);
    }
    return didSend;
  }

  chunkUrls(urls, maxSize) {
    let chunks = [];

    urls.sort((a, b) => a.length - b.length);

    while (urls.length) {
      let chunk = lazy.Utils.tryFitItems(urls, maxSize);
      if (!chunk.length) {
        urls.forEach(url => {
          log.warn(`Skipping oversized URL: ${url}`);
        });
        break;
      }
      chunks.push(chunk);
      urls.splice(0, chunk.length);
    }
    return chunks;
  }

  async _ensureTimer(timeout) {
    log.info(
      `Setting a new close-tab timer with delay=${timeout} with existing timer=${!!this
        .#timer}`
    );

    if (this.#timer) {
      clearTimeout(this.#timer);
    }

    this.#timer = setTimeout(async () => {
      this.#timer = null;
      await this.flushQueue();
    }, timeout);
  }

  async #onShutdown() {
    log.debug(
      `CommandQueue shutdown is flushing the queue with a timer=${!!this
        .#timer}`
    );
    if (this.#timer) {
      clearTimeout(this.#timer);
      this.#timer = null;
      await this.flushQueue(true);
    }
  }

  now() {
    return Date.now();
  }
}

function urlsafeBase64Encode(buffer) {
  return ChromeUtils.base64URLEncode(new Uint8Array(buffer), { pad: false });
}

function urlsafeBase64Decode(str) {
  return ChromeUtils.base64URLDecode(str, { padding: "reject" });
}
