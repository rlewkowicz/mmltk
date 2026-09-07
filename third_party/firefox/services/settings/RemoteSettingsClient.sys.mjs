/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

import { Downloader } from "resource://services-settings/Attachments.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ClientEnvironmentBase:
    "resource://gre/modules/components-utils/ClientEnvironment.sys.mjs",
  Database: "resource://services-settings/Database.sys.mjs",
  IDBHelpers: "resource://services-settings/IDBHelpers.sys.mjs",
  KintoHttpClient: "resource://services-common/kinto-http-client.sys.mjs",
  ObjectUtils: "resource://gre/modules/ObjectUtils.sys.mjs",
  RemoteSettingsWorker:
    "resource://services-settings/RemoteSettingsWorker.sys.mjs",
  RemoteSettings: "resource://services-settings/remote-settings.sys.mjs",
  SharedUtils: "resource://services-settings/SharedUtils.sys.mjs",
  Utils: "resource://services-settings/Utils.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "console", () => lazy.Utils.log);

class EventEmitter {
  constructor(events) {
    this._listeners = new Map();
    for (const event of events) {
      this._listeners.set(event, []);
    }
  }

  async emit(event, payload) {
    const callbacks = this._listeners.get(event);
    let lastError;
    for (const cb of callbacks) {
      try {
        await cb(payload);
      } catch (e) {
        lastError = e;
      }
    }
    if (lastError) {
      throw lastError;
    }
  }

  hasListeners(event) {
    return this._listeners.has(event) && !!this._listeners.get(event).length;
  }

  on(event, callback) {
    if (!this._listeners.has(event)) {
      throw new Error(`Unknown event type ${event}`);
    }
    this._listeners.get(event).push(callback);
  }

  off(event, callback) {
    if (!this._listeners.has(event)) {
      throw new Error(`Unknown event type ${event}`);
    }
    const callbacks = this._listeners.get(event);
    const i = callbacks.indexOf(callback);
    if (i < 0) {
      throw new Error(`Unknown callback`);
    } else {
      callbacks.splice(i, 1);
    }
  }
}

class APIError extends Error {}

class NetworkError extends APIError {
  constructor(e) {
    super(`Network error: ${e}`, { cause: e });
    this.name = "NetworkError";
  }
}

class NetworkOfflineError extends APIError {
  constructor() {
    super("Network is offline");
    this.name = "NetworkOfflineError";
  }
}

class ServerContentParseError extends APIError {
  constructor(e) {
    super(`Cannot parse server content: ${e}`, { cause: e });
    this.name = "ServerContentParseError";
  }
}

class BackendError extends APIError {
  constructor(e) {
    super(`Backend error: ${e}`, { cause: e });
    this.name = "BackendError";
  }
}

class BackoffError extends APIError {
  constructor(e) {
    super(`Server backoff: ${e}`, { cause: e });
    this.name = "BackoffError";
  }
}

class TimeoutError extends APIError {
  constructor(e) {
    super(`API timeout: ${e}`, { cause: e });
    this.name = "TimeoutError";
  }
}

class StorageError extends Error {
  constructor(e) {
    super(`Storage error: ${e}`, { cause: e });
    this.name = "StorageError";
  }
}

class InvalidSignatureError extends Error {
  constructor(cid, x5u, signerName) {
    let message = `Invalid content signature (${cid})`;
    if (x5u) {
      const chain = x5u.split("/").pop();
      message += ` using '${chain}' and signer ${signerName}`;
    }
    super(message);
    this.name = "InvalidSignatureError";
  }
}

class MissingSignatureError extends InvalidSignatureError {
  constructor(cid) {
    super(cid);
    this.message = `Missing signature (${cid})`;
    this.name = "MissingSignatureError";
  }
}

class CorruptedDataError extends InvalidSignatureError {
  constructor(cid) {
    super(cid);
    this.message = `Corrupted local data (${cid})`;
    this.name = "CorruptedDataError";
  }
}

class UnknownCollectionError extends Error {
  constructor(cid) {
    super(`Unknown Collection "${cid}"`);
    this.name = "UnknownCollectionError";
  }
}

class AttachmentDownloader extends Downloader {
  constructor(client) {
    super(client.bucketName, client.collectionName);
    this._client = client;
  }

  get cacheImpl() {
    const cacheImpl = {
      get: async attachmentId => {
        return this._client.db.getAttachment(attachmentId);
      },
      set: async (attachmentId, attachment) => {
        return this._client.db.saveAttachment(attachmentId, attachment);
      },
      setMultiple: async attachmentsIdsBlobs => {
        return this._client.db.saveAttachments(attachmentsIdsBlobs);
      },
      delete: async attachmentId => {
        return this._client.db.saveAttachment(attachmentId, null);
      },
      deleteMultiple: async attachmentIds => {
        return this._client.db.saveAttachments(
          attachmentIds.map(id => [id, null])
        );
      },
      prune: async excludeIds => {
        return this._client.db.pruneAttachments(excludeIds);
      },
      hasData: async () => {
        return this._client.db.hasAttachments();
      },
    };
    Object.defineProperty(this, "cacheImpl", { value: cacheImpl });
    return cacheImpl;
  }

  async download(record, options) {
    await lazy.UptakeTelemetry.report(
      lazy.UptakeTelemetry.STATUS.DOWNLOAD_START,
      {
        source: this._client.identifier,
      }
    );
    try {
      return await super.download(record, options);
    } catch (err) {
      let status = lazy.UptakeTelemetry.STATUS.DOWNLOAD_ERROR;
      if (lazy.Utils.isOffline) {
        status = lazy.UptakeTelemetry.STATUS.NETWORK_OFFLINE_ERROR;
      } else if (/NetworkError/.test(err.message)) {
        status = lazy.UptakeTelemetry.STATUS.NETWORK_ERROR;
      }
      await lazy.UptakeTelemetry.report(status, {
        source: this._client.identifier,
        errorName: err.name,
      });
      throw err;
    }
  }

  async deleteAll() {
    let allRecords = await this._client.db.list();
    const idsToDelete = allRecords.filter(r => !!r.attachment).map(r => r.id);
    if (idsToDelete.length) {
      await this.cacheImpl.deleteMultiple(idsToDelete);
    }
  }
}

export class RemoteSettingsClient extends EventEmitter {
  static get APIError() {
    return APIError;
  }
  static get NetworkError() {
    return NetworkError;
  }
  static get NetworkOfflineError() {
    return NetworkOfflineError;
  }
  static get ServerContentParseError() {
    return ServerContentParseError;
  }
  static get BackendError() {
    return BackendError;
  }
  static get BackoffError() {
    return BackoffError;
  }
  static get TimeoutError() {
    return TimeoutError;
  }
  static get StorageError() {
    return StorageError;
  }
  static get InvalidSignatureError() {
    return InvalidSignatureError;
  }
  static get MissingSignatureError() {
    return MissingSignatureError;
  }
  static get CorruptedDataError() {
    return CorruptedDataError;
  }
  static get UnknownCollectionError() {
    return UnknownCollectionError;
  }
  static get EmptyDatabaseError() {
    return lazy.Database.EmptyDatabaseError;
  }

  constructor(
    collectionName,
    {
      bucketName = AppConstants.REMOTE_SETTINGS_DEFAULT_BUCKET,
      signerName,
      filterCreator,
      localFields = [],
      keepAttachmentsIds = [],
      lastCheckTimePref,
    } = {}
  ) {
    if (
      !AppConstants.RELEASE_OR_BETA &&
      Services.appinfo.processType !== Services.appinfo.PROCESS_TYPE_DEFAULT
    ) {
      throw new Error(
        "Cannot instantiate Remote Settings client in child processes."
      );
    }

    super(["sync"]); 

    this.collectionName = collectionName;
    this.bucketName = lazy.Utils.actualBucketName(bucketName);
    this.signerName = signerName;
    this.filterCreator = filterCreator;
    this.localFields = localFields;
    this.keepAttachmentsIds = keepAttachmentsIds;
    this._lastCheckTimePref = lastCheckTimePref;
    this._verifier = null;
    this._syncRunning = false;

    this.verifySignature = AppConstants.REMOTE_SETTINGS_VERIFY_SIGNATURE;
  }

  #lazy = XPCOMUtils.declareLazy({
    db: () => new lazy.Database(this.identifier),
    attachments: () => new AttachmentDownloader(this),
  });

  get db() {
    return this.#lazy.db;
  }

  get attachments() {
    return this.#lazy.attachments;
  }

  refreshBucketName() {
    this.bucketName = lazy.Utils.actualBucketName(this.bucketName);
    this.db.identifier = this.identifier;
  }

  get identifier() {
    return `${this.bucketName}/${this.collectionName}`;
  }

  get lastCheckTimePref() {
    return (
      this._lastCheckTimePref ||
      `services.settings.${this.bucketName}.${this.collectionName}.last_check`
    );
  }

  httpClient() {
    const api = new lazy.KintoHttpClient(lazy.Utils.SERVER_URL, {
      fetchFunc: lazy.Utils.fetch, 
    });
    return api.bucket(this.bucketName).collection(this.collectionName);
  }

  async getLastModified() {
    let timestamp = -1;
    try {
      timestamp = await this.db.getLastModified();
    } catch (err) {
      lazy.console.warn(
        `Error retrieving the getLastModified timestamp from ${this.identifier} RemoteSettingsClient`,
        err
      );
    }

    return timestamp;
  }

  // eslint-disable-next-line complexity
  async get(options = {}) {
    const {
      filters = {},
      order = "", 
      dumpFallback = true,
      emptyListFallback = true,
      forceSync = false,
      loadDumpIfNewer = true,
      syncIfEmpty = true,
    } = options;
    let { verifySignature = false } = options;

    const hasParallelCall = !!this._importingPromise;
    let data;
    try {
      let lastModified = forceSync ? null : await this.db.getLastModified();
      let hasLocalData = lastModified !== null;

      if (forceSync) {
        if (!this._importingPromise) {
          this._importingPromise = (async () => {
            await this.sync({ sendEvents: false, trigger: "forced" });
            return true; 
          })();
        }
      } else if (!syncIfEmpty && !hasLocalData && !emptyListFallback) {
        throw new RemoteSettingsClient.EmptyDatabaseError(this.identifier);
      } else if (!syncIfEmpty && !hasLocalData && verifySignature) {
        throw new RemoteSettingsClient.MissingSignatureError(this.identifier);
      } else if (syncIfEmpty && !hasLocalData) {
        if (!this._importingPromise) {
          this._importingPromise = (async () => {
            const importedFromDump = lazy.Utils.LOAD_DUMPS
              ? await this._importJSONDump()
              : -1;
            if (importedFromDump < 0) {
              lazy.console.debug(
                `${this.identifier} Local DB is empty, pull data from server`
              );
              const waitedAt = ChromeUtils.now();
              const pulled = await lazy.RemoteSettings.pullStartupBundle();
              if (!pulled.includes(this.identifier)) {
                lazy.console.debug(
                  `${this.identifier} was not part of startup bundle. Force a sync`
                );
                await this.sync({ loadDump: false, sendEvents: false });
              }
              const durationMilliseconds = ChromeUtils.now() - waitedAt;
              lazy.console.debug(
                `${this.identifier} Waited ${durationMilliseconds}ms for 'syncIfEmpty' in 'get()'`
              );
            }
            return true;
          })();
        } else {
          lazy.console.debug(`${this.identifier} Awaiting existing import.`);
        }
      } else if (hasLocalData && loadDumpIfNewer && lazy.Utils.LOAD_DUMPS) {
        let lastModifiedDump = await lazy.Utils.getLocalDumpLastModified(
          this.bucketName,
          this.collectionName
        );
        if (lastModified < lastModifiedDump) {
          lazy.console.debug(
            `${this.identifier} Local DB is stale (${lastModified}), using dump instead (${lastModifiedDump})`
          );
          if (!this._importingPromise) {
            this._importingPromise = (async () => {
              const importedFromDump = await this._importJSONDump();
              return importedFromDump >= 0;
            })();
          } else {
            lazy.console.debug(`${this.identifier} Awaiting existing import.`);
          }
        }
      }

      if (this._importingPromise) {
        try {
          if (await this._importingPromise) {
            verifySignature = false;
          }
        } catch (e) {
          if (!hasParallelCall) {
            throw e;
          }
          lazy.console.error(e);
        } finally {
          delete this._importingPromise;
        }
      }

      data = await this.db.list({ filters, order });
    } catch (e) {
      if (!dumpFallback) {
        throw e;
      }
      if (
        e instanceof RemoteSettingsClient.EmptyDatabaseError &&
        emptyListFallback
      ) {
        lazy.console.debug(e);
      } else {
        lazy.console.error(e);
      }
      ({ data } = await lazy.SharedUtils.loadJSONDump(
        this.bucketName,
        this.collectionName
      ));
      if (data !== null) {
        lazy.console.info(`${this.identifier} falling back to JSON dump`);
      } else if (emptyListFallback) {
        lazy.console.info(
          `${this.identifier} no dump fallback, return empty list`
        );
        data = [];
      } else {
        throw e;
      }
      if (!lazy.ObjectUtils.isEmpty(filters)) {
        data = data.filter(r => lazy.Utils.filterObject(filters, r));
      }
      if (order) {
        data = lazy.Utils.sortObjects(order, data);
      }
      return this._filterEntries(data);
    }

    if (this.verifySignature && verifySignature) {
      lazy.console.debug(
        `${this.identifier} verify signature of local data on read`
      );
      const allData = lazy.ObjectUtils.isEmpty(filters)
        ? data
        : await this.db.list();
      const localRecords = allData.map(r => this._cleanLocalFields(r));
      const timestamp = await this.db.getLastModified();
      let metadata = await this.db.getMetadata();
      if (syncIfEmpty && lazy.ObjectUtils.isEmpty(metadata)) {
        await this.sync({ loadDump: false, sendEvents: false });
        metadata = await this.db.getMetadata();
      }
      await this.validateCollectionSignature(localRecords, timestamp, metadata);
    }

    const final = await this._filterEntries(data);
    if (final.length != data.length) {
      lazy.console.debug(
        `${this.identifier} ${final.length}/${data.length} records after filtering.`
      );
    } else {
      lazy.console.debug(`${this.identifier} ${data.length} records.`);
    }
    return final;
  }

  async sync(options) {
    if (lazy.Utils.shouldSkipRemoteActivity) {
      lazy.console.debug(`${this.identifier} Skip remote sync.`);
      return;
    }

    const { changes } = await lazy.Utils.fetchLatestChanges(
      lazy.Utils.SERVER_URL,
      {
        filters: {
          collection: this.collectionName,
          bucket: this.bucketName,
        },
      }
    );
    if (changes.length === 0) {
      throw new RemoteSettingsClient.UnknownCollectionError(this.identifier);
    }
    const [{ last_modified: expectedTimestamp }] = changes;

    await this.maybeSync(expectedTimestamp, { ...options, trigger: "forced" });
  }

  // eslint-disable-next-line complexity
  async maybeSync(expectedTimestamp, options = {}) {
    const {
      loadDump = lazy.Utils.LOAD_DUMPS,
      trigger = "manual",
      sendEvents = true,
    } = options;

    if (this._syncRunning) {
      lazy.console.warn(`${this.identifier} sync already running`);
      return;
    }

    if (Services.startup.shuttingDown) {
      lazy.console.warn(`${this.identifier} sync interrupted by shutdown`);
      return;
    }

    this._syncRunning = true;

    await lazy.UptakeTelemetry.report(lazy.UptakeTelemetry.STATUS.SYNC_START, {
      source: this.identifier,
      trigger,
    });

    let importedFromDump = [];
    const startedAt = new Date();
    let reportStatus = null;
    let thrownError = null;
    try {
      if (lazy.Utils.isOffline) {
        throw new RemoteSettingsClient.NetworkOfflineError();
      }

      let collectionLastModified = await this.db.getLastModified();
      const hasLocalData = collectionLastModified !== null;
      let localRecords = hasLocalData
        ? (await this.db.list()).map(r => this._cleanLocalFields(r))
        : null;
      const localMetadata = await this.db.getMetadata();

      if (!hasLocalData && loadDump) {
        try {
          const imported = await this._importJSONDump();
          if (imported > 0) {
            lazy.console.debug(
              `${this.identifier} ${imported} records loaded from JSON dump`
            );
            importedFromDump = await this.db.list();
            localRecords = importedFromDump;
          }
          collectionLastModified = await this.db.getLastModified();
        } catch (e) {
          console.error(e);
        }
      }
      let syncResult;
      try {
        if (expectedTimestamp == collectionLastModified) {
          lazy.console.debug(`${this.identifier} local data is up-to-date`);
          reportStatus = lazy.UptakeTelemetry.STATUS.UP_TO_DATE;

          if (this.verifySignature && lazy.ObjectUtils.isEmpty(localMetadata)) {
            lazy.console.debug(`${this.identifier} pull collection metadata`);
            const { metadata } = await this._fetchChangeset(
              expectedTimestamp,
              expectedTimestamp
            );
            await this.db.importChanges(metadata);
            if (this.verifySignature && !importedFromDump.length) {
              lazy.console.debug(
                `${this.identifier} verify signature of local data`
              );
              await this.validateCollectionSignature(
                localRecords,
                collectionLastModified,
                metadata
              );
            }
          }

          if (!importedFromDump.length) {
            return;
          }
          syncResult = {
            current: importedFromDump,
            created: importedFromDump,
            updated: [],
            deleted: [],
          };
        } else {
          syncResult = await this._importChanges(
            localRecords,
            collectionLastModified,
            localMetadata,
            expectedTimestamp
          );
          if (sendEvents && this.hasListeners("sync")) {
            const importedById = importedFromDump.reduce((acc, r) => {
              acc.set(r.id, r);
              return acc;
            }, new Map());
            syncResult.deleted.forEach(r => importedById.delete(r.id));
            syncResult.updated.forEach(u => {
              if (importedById.has(u.old.id)) {
                importedById.set(u.old.id, u.new);
              }
            });
            syncResult.created = syncResult.created.concat(
              Array.from(importedById.values())
            );
          }

          if (trigger == "timer") {
            const deleted = await this.attachments.prune(
              this.keepAttachmentsIds
            );
            if (deleted > 0) {
              lazy.console.warn(
                `${this.identifier} Pruned ${deleted} obsolete attachments`
              );
            }
          }
        }
      } catch (e) {
        if (e instanceof InvalidSignatureError) {
          reportStatus =
            e instanceof CorruptedDataError
              ? lazy.UptakeTelemetry.STATUS.CORRUPTION_ERROR
              : lazy.UptakeTelemetry.STATUS.SIGNATURE_ERROR;
          try {
            lazy.console.warn(
              `${this.identifier} Signature verified failed. Retry from scratch`
            );
            syncResult = await this._importChanges(
              localRecords,
              collectionLastModified,
              localMetadata,
              expectedTimestamp,
              { retry: true }
            );
          } catch (ex) {
            reportStatus = lazy.UptakeTelemetry.STATUS.SIGNATURE_RETRY_ERROR;
            throw ex;
          }
        } else {
          const adjustedError = this._adjustedError(e);
          reportStatus = this._telemetryFromError(adjustedError, {
            default: lazy.UptakeTelemetry.STATUS.SYNC_ERROR,
          });
          throw adjustedError;
        }
      }
      if (sendEvents) {
        const filteredSyncResult = await this._filterSyncResult(syncResult);
        if (filteredSyncResult) {
          try {
            await this.emit("sync", { data: filteredSyncResult });
          } catch (e) {
            reportStatus = lazy.UptakeTelemetry.STATUS.APPLY_ERROR;
            throw e;
          }
        } else {
          const wasFiltered =
            syncResult.created.length +
              syncResult.updated.length +
              syncResult.deleted.length >
            0;
          if (wasFiltered) {
            lazy.console.info(
              `${this.identifier} All sync changes are filtered by JEXL expressions`
            );
          } else {
            lazy.console.info(`${this.identifier} No changes during sync`);
          }
        }
      }
    } catch (e) {
      thrownError = e;
      const adjustedError = this._adjustedError(e);
      if (Services.startup.shuttingDown) {
        reportStatus = lazy.UptakeTelemetry.STATUS.SHUTDOWN_ERROR;
      }
      else if (reportStatus == null) {
        reportStatus = this._telemetryFromError(adjustedError, {
          default: lazy.UptakeTelemetry.STATUS.UNKNOWN_ERROR,
        });
      }
      throw e;
    } finally {
      const durationMilliseconds = new Date() - startedAt;
      if (reportStatus === null) {
        reportStatus = lazy.UptakeTelemetry.STATUS.SUCCESS;
      }
      let reportArgs = {
        source: this.identifier,
        trigger,
        duration: durationMilliseconds,
      };
      if (
        thrownError !== null &&
        [
          lazy.UptakeTelemetry.STATUS.SYNC_ERROR,
          lazy.UptakeTelemetry.STATUS.CUSTOM_1_ERROR, 
          lazy.UptakeTelemetry.STATUS.UNKNOWN_ERROR,
          lazy.UptakeTelemetry.STATUS.SHUTDOWN_ERROR,
        ].includes(reportStatus)
      ) {
        reportArgs = { ...reportArgs, errorName: thrownError.name };
      }

      await lazy.UptakeTelemetry.report(reportStatus, reportArgs);

      lazy.console.debug(`${this.identifier} sync status is ${reportStatus}`);
      this._syncRunning = false;
    }
  }

  _adjustedError(e) {
    if (/unparseable/.test(e.message)) {
      return new RemoteSettingsClient.ServerContentParseError(e);
    }
    if (/NetworkError/.test(e.message)) {
      return new RemoteSettingsClient.NetworkError(e);
    }
    if (/Timeout/.test(e.message)) {
      return new RemoteSettingsClient.TimeoutError(e);
    }
    if (/HTTP 5??/.test(e.message)) {
      return new RemoteSettingsClient.BackendError(e);
    }
    if (/Backoff/.test(e.message)) {
      return new RemoteSettingsClient.BackoffError(e);
    }
    if (
      e instanceof lazy.IDBHelpers.IndexedDBError ||
      /IndexedDB/.test(e.message)
    ) {
      return new RemoteSettingsClient.StorageError(e);
    }
    return e;
  }

  _telemetryFromError(e, options = { default: null }) {
    let reportStatus = options.default;

    if (e instanceof RemoteSettingsClient.NetworkOfflineError) {
      reportStatus = lazy.UptakeTelemetry.STATUS.NETWORK_OFFLINE_ERROR;
    } else if (e instanceof lazy.IDBHelpers.ShutdownError) {
      reportStatus = lazy.UptakeTelemetry.STATUS.SHUTDOWN_ERROR;
    } else if (e instanceof RemoteSettingsClient.ServerContentParseError) {
      reportStatus = lazy.UptakeTelemetry.STATUS.PARSE_ERROR;
    } else if (e instanceof RemoteSettingsClient.NetworkError) {
      reportStatus = lazy.UptakeTelemetry.STATUS.NETWORK_ERROR;
    } else if (e instanceof RemoteSettingsClient.TimeoutError) {
      reportStatus = lazy.UptakeTelemetry.STATUS.TIMEOUT_ERROR;
    } else if (e instanceof RemoteSettingsClient.BackendError) {
      reportStatus = lazy.UptakeTelemetry.STATUS.SERVER_ERROR;
    } else if (e instanceof RemoteSettingsClient.BackoffError) {
      reportStatus = lazy.UptakeTelemetry.STATUS.BACKOFF;
    } else if (e instanceof RemoteSettingsClient.StorageError) {
      reportStatus = lazy.UptakeTelemetry.STATUS.CUSTOM_1_ERROR;
    }

    return reportStatus;
  }

  async _importJSONDump() {
    lazy.console.info(`${this.identifier} try to restore dump`);
    const result = await lazy.RemoteSettingsWorker.importJSONDump(
      this.bucketName,
      this.collectionName
    );
    if (result < 0) {
      lazy.console.debug(`${this.identifier} no dump available`);
    } else {
      lazy.console.info(
        `${this.identifier} imported ${result} records from dump`
      );
    }
    return result;
  }

  async validateCollectionSignature(records, timestamp, metadata) {
    if (
      !metadata?.signatures ||
      !Array.isArray(metadata.signatures) ||
      metadata.signatures.length === 0
    ) {
      throw new MissingSignatureError(this.identifier);
    }

    if (!this._verifier) {
      this._verifier = Cc[
        "@mozilla.org/security/contentsignatureverifier;1"
      ].createInstance(Ci.nsIContentSignatureVerifier);
    }

    const serialized = await lazy.RemoteSettingsWorker.canonicalStringify(
      records,
      timestamp
    );

    const thrownErrors = [];
    const { signatures } = metadata;
    for (const sig of signatures) {
      const { x5u, signature, mode } = sig;
      if (!x5u || !signature || (mode && mode !== "p384ecdsa")) {
        lazy.console.warn(
          `${this.identifier} ignore unsupported signature entry in metadata`
        );
        continue;
      }
      const certChain = await (await lazy.Utils.fetch(x5u)).text();
      lazy.console.debug(`${this.identifier} verify signature using ${x5u}`);

      if (
        await this._verifier.asyncVerifyContentSignature(
          serialized,
          "p384ecdsa=" + signature,
          certChain,
          this.signerName,
          lazy.Utils.CERT_CHAIN_ROOT_IDENTIFIER
        )
      ) {
        return;
      }
      thrownErrors.push(
        new InvalidSignatureError(this.identifier, x5u, this.signerName)
      );
    }

    throw thrownErrors[0];
  }

  async _importChanges(
    localRecords,
    localTimestamp,
    localMetadata,
    expectedTimestamp,
    options = {}
  ) {
    const hasLocalData = localTimestamp !== null;
    const { retry = false } = options;
    const since = retry || !hasLocalData ? undefined : localTimestamp;

    const verifySignatureLocalData = (resolve, reject) => {
      if (!hasLocalData) {
        resolve(false);
        return;
      }
      lazy.console.debug(
        `${this.identifier} verify local data before importing remote`
      );
      this.validateCollectionSignature(
        localRecords,
        localTimestamp,
        localMetadata
      )
        .then(() => resolve(true))
        .catch(err => {
          if (err instanceof InvalidSignatureError) {
            lazy.console.debug(`${this.identifier} previous data was invalid`);
            resolve(false);
          } else {
            reject(err);
          }
        });
    };

    lazy.console.debug(
      `${this.identifier} Fetch changes from server (expected=${expectedTimestamp}, since=${since})`
    );
    const { metadata, remoteTimestamp, remoteRecords } =
      await this._fetchChangeset(expectedTimestamp, since);

    lazy.console.debug(
      `${this.identifier} local timestamp: ${localTimestamp}, remote: ${remoteTimestamp} (expected: ${expectedTimestamp})`
    );

    if (remoteTimestamp < localTimestamp) {
      const localTrustworthy = await new Promise(verifySignatureLocalData);
      if (localTrustworthy) {
        lazy.console.info(`${this.identifier} CDN served staled data, ignore.`);
        return {
          current: localRecords,
          created: [],
          updated: [],
          deleted: [],
        };
      }
      lazy.console.warn(
        `${this.identifier} CDN served staled data, but local data is corrupted, import anyway.`
      );
    }

    await this.db.importChanges(metadata, remoteTimestamp, remoteRecords, {
      clear: retry,
    });

    const newLocal = await this.db.list();
    const newRecords = newLocal.map(r => this._cleanLocalFields(r));
    if (this.verifySignature) {
      try {
        await this.validateCollectionSignature(
          newRecords,
          remoteTimestamp,
          metadata
        );
      } catch (e) {
        lazy.console.error(
          `${this.identifier} Signature failed ${retry ? "again" : ""} ${e}`
        );
        if (!(e instanceof InvalidSignatureError)) {
          throw e;
        }

        if (!hasLocalData) {
          lazy.console.debug(`${this.identifier} No previous data to restore`);
        }
        const localTrustworthy =
          hasLocalData && (await new Promise(verifySignatureLocalData));
        if (!localTrustworthy && !retry) {
          lazy.console.debug(`${this.identifier} clear local data`);
          await this.db.clear();
          lazy.console.error(`${this.identifier} local data was corrupted`);
          throw new CorruptedDataError(this.identifier);
        } else if (retry) {
          if (localTrustworthy) {
            await this.db.importChanges(
              localMetadata,
              localTimestamp,
              localRecords,
              {
                clear: true, 
              }
            );
          } else {
            const imported = await this._importJSONDump();
            if (imported < 0) {
              await this.db.clear();
            }
          }
        }
        throw e;
      }
    } else {
      lazy.console.warn(`${this.identifier} has signature disabled`);
    }

    const syncResult = {
      current: localRecords,
      created: [],
      updated: [],
      deleted: [],
    };
    if (this.hasListeners("sync")) {
      syncResult.current = newRecords;
      const oldById = hasLocalData
        ? new Map(localRecords.map(e => [e.id, e]))
        : new Map();
      for (const r of newRecords) {
        const old = oldById.get(r.id);
        if (old) {
          oldById.delete(r.id);
          if (r.last_modified != old.last_modified) {
            syncResult.updated.push({ old, new: r });
          }
        } else {
          syncResult.created.push(r);
        }
      }
      syncResult.deleted = syncResult.deleted.concat(
        Array.from(oldById.values())
      );
      lazy.console.debug(
        `${this.identifier} ${syncResult.created.length} created. ${syncResult.updated.length} updated. ${syncResult.deleted.length} deleted.`
      );
    }

    return syncResult;
  }

  async _fetchChangeset(expectedTimestamp, since) {
    const client = this.httpClient();
    const {
      metadata,
      timestamp: remoteTimestamp,
      changes: remoteRecords,
    } = await client.execute(
      {
        path: `/buckets/${this.bucketName}/collections/${this.collectionName}/changeset`,
      },
      {
        query: {
          _expected: expectedTimestamp,
          _since: since,
        },
      }
    );
    return {
      remoteTimestamp,
      metadata,
      remoteRecords,
    };
  }

  async _filterSyncResult(syncResult) {
    const {
      current: allData,
      created: allCreated,
      updated: allUpdated,
      deleted: allDeleted,
    } = syncResult;
    const [created, deleted, updatedFiltered] = await Promise.all(
      [allCreated, allDeleted, allUpdated.map(e => e.new)].map(
        this._filterEntries.bind(this)
      )
    );
    const updatedFilteredIds = new Set(updatedFiltered.map(e => e.id));
    const updated = allUpdated.filter(({ new: { id } }) =>
      updatedFilteredIds.has(id)
    );

    if (!created.length && !updated.length && !deleted.length) {
      return null;
    }
    const current = await this._filterEntries(allData);
    return { created, updated, deleted, current };
  }

  async _filterEntries(data) {
    if (!this.filterCreator) {
      return data;
    }
    const filter = await this.filterCreator(
      lazy.ClientEnvironmentBase,
      this.identifier
    );
    const results = [];
    for (const entry of data) {
      const filteredEntry = await filter.filterEntry(entry);
      if (filteredEntry) {
        results.push(filteredEntry);
      }
    }
    return results;
  }

  _cleanLocalFields(record) {
    const keys = ["_status"].concat(this.localFields);
    const result = { ...record };
    for (const key of keys) {
      delete result[key];
    }
    return result;
  }
}
