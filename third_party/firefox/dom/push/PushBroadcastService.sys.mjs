/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  JSONFile: "resource://gre/modules/JSONFile.sys.mjs",
  PushService: "resource://gre/modules/PushService.sys.mjs",
});

const DUMMY_VERSION_STRING = "____NOP____";

ChromeUtils.defineLazyGetter(lazy, "console", () => {
  return console.createInstance({
    maxLogLevelPref: "dom.push.loglevel",
    prefix: "BroadcastService",
  });
});

class InvalidSourceInfo extends Error {
  constructor(message) {
    super(message);
    this.name = "InvalidSourceInfo";
  }
}

const BROADCAST_SERVICE_VERSION = 1;

export var BroadcastService = class {
  constructor(pushService, path) {
    this.PHASES = {
      HELLO: "hello",
      REGISTER: "register",
      BROADCAST: "broadcast",
    };

    this.pushService = pushService;
    this.jsonFile = new lazy.JSONFile({
      path,
      dataPostProcessor: this._initializeJSONFile,
    });
    this.initializePromise = this.jsonFile.load();
  }

  async getListeners() {
    await this.initializePromise;
    return Object.entries(this.jsonFile.data.listeners).reduce(
      (acc, [k, v]) => {
        acc[k] = v.version;
        return acc;
      },
      {}
    );
  }

  _initializeJSONFile(data) {
    if (!data.version) {
      data.version = BROADCAST_SERVICE_VERSION;
    }
    if (!data.hasOwnProperty("listeners")) {
      data.listeners = {};
    }
    return data;
  }

  async _resetListeners() {
    await this.initializePromise;
    this.jsonFile.data = this._initializeJSONFile({});
    this.initializePromise = Promise.resolve();
  }

  _validateSourceInfo(sourceInfo) {
    const { moduleURI, symbolName } = sourceInfo;
    if (typeof moduleURI !== "string") {
      throw new InvalidSourceInfo(
        `moduleURI must be a string (got ${typeof moduleURI})`
      );
    }
    if (typeof symbolName !== "string") {
      throw new InvalidSourceInfo(
        `symbolName must be a string (got ${typeof symbolName})`
      );
    }
  }

  async addListener(broadcastId, version, sourceInfo) {
    lazy.console.info(
      "addListener: adding listener",
      broadcastId,
      version,
      sourceInfo
    );
    await this.initializePromise;
    this._validateSourceInfo(sourceInfo);
    if (typeof version !== "string") {
      throw new TypeError("version should be a string");
    }
    if (!version) {
      throw new TypeError("version should not be an empty string");
    }

    const isNew = !this.jsonFile.data.listeners.hasOwnProperty(broadcastId);
    const oldVersion =
      !isNew && this.jsonFile.data.listeners[broadcastId].version;
    if (!isNew && oldVersion != version) {
      lazy.console.warn(
        "Versions differ while adding listener for",
        broadcastId,
        ". Got",
        version,
        "but JSON file says",
        oldVersion,
        "."
      );
    }

    this.jsonFile.data.listeners[broadcastId] = {
      version: oldVersion || version,
      sourceInfo,
    };
    this.jsonFile.saveSoon();

    if (isNew) {
      await this.pushService.subscribeBroadcast(broadcastId, version);
    }
  }

  async receivedBroadcastMessage(broadcasts, context) {
    lazy.console.info("receivedBroadcastMessage:", broadcasts, context);
    await this.initializePromise;
    for (const broadcastId in broadcasts) {
      const version = broadcasts[broadcastId];
      if (version === DUMMY_VERSION_STRING) {
        lazy.console.info(
          "Ignoring",
          version,
          "because it's the dummy version"
        );
        continue;
      }
      if (!this.jsonFile.data.listeners.hasOwnProperty(broadcastId)) {
        lazy.console.warn(
          "receivedBroadcastMessage: unknown broadcastId",
          broadcastId
        );
        continue;
      }

      const { sourceInfo } = this.jsonFile.data.listeners[broadcastId];
      try {
        this._validateSourceInfo(sourceInfo);
      } catch (e) {
        lazy.console.error(
          "receivedBroadcastMessage: malformed sourceInfo",
          sourceInfo,
          e
        );
        continue;
      }

      const { moduleURI, symbolName } = sourceInfo;

      let module;
      try {
        module = ChromeUtils.importESModule(moduleURI);
      } catch (e) {
        lazy.console.error(
          "receivedBroadcastMessage: couldn't invoke",
          broadcastId,
          "because import of module",
          moduleURI,
          "failed",
          e
        );
        continue;
      }

      if (!module[symbolName]) {
        lazy.console.error(
          "receivedBroadcastMessage: couldn't invoke",
          broadcastId,
          "because module",
          moduleURI,
          "missing attribute",
          symbolName
        );
        continue;
      }

      const handler = module[symbolName];

      if (!handler.receivedBroadcastMessage) {
        lazy.console.error(
          "receivedBroadcastMessage: couldn't invoke",
          broadcastId,
          "because handler returned by",
          `${moduleURI}.${symbolName}`,
          "has no receivedBroadcastMessage method"
        );
        continue;
      }

      try {
        await handler.receivedBroadcastMessage(version, broadcastId, context);
      } catch (e) {
        lazy.console.error(
          "receivedBroadcastMessage: handler for",
          broadcastId,
          "threw error:",
          e
        );
        continue;
      }

      if (this.jsonFile.data.listeners[broadcastId].version != version) {
        this.jsonFile.data.listeners[broadcastId].version = version;
        this.jsonFile.saveSoon();
      }
    }
  }

  _saveImmediately() {
    return this.jsonFile._save();
  }
};

function initializeBroadcastService() {
  let path = "broadcast-listeners.json";
  try {
    if (PathUtils.profileDir) {
      path = PathUtils.join(PathUtils.profileDir, path);
    }
  } catch (e) {}
  return new BroadcastService(lazy.PushService, path);
}

export var pushBroadcastService = initializeBroadcastService();
