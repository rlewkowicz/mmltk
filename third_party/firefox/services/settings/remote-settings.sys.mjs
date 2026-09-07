/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ClientEnvironmentBase:
    "resource://gre/modules/components-utils/ClientEnvironment.sys.mjs",
  Database: "resource://services-settings/Database.sys.mjs",
  FilterExpressions:
    "resource://gre/modules/components-utils/FilterExpressions.sys.mjs",
  pushBroadcastService: "resource://gre/modules/PushBroadcastService.sys.mjs",
  RemoteSettingsClient:
    "resource://services-settings/RemoteSettingsClient.sys.mjs",
  SyncHistory: "resource://services-settings/SyncHistory.sys.mjs",
  Utils: "resource://services-settings/Utils.sys.mjs",
});

const PREF_SETTINGS_BRANCH = "services.settings.";
const PREF_SETTINGS_SERVER_BACKOFF = "server.backoff";
const PREF_SETTINGS_LAST_UPDATE = "last_update_seconds";
const PREF_SETTINGS_LAST_ETAG = "last_etag";
const PREF_SETTINGS_CLOCK_SKEW_SECONDS = "clock_skew_seconds";
const PREF_SETTINGS_SYNC_HISTORY_SIZE = "sync_history_size";
const PREF_SETTINGS_SYNC_HISTORY_ERROR_THRESHOLD =
  "sync_history_error_threshold";

const TELEMETRY_SOURCE_POLL = "settings-changes-monitoring";
const TELEMETRY_SOURCE_SYNC = "settings-sync";

const BROADCAST_ID = "remote-settings/monitor_changes";

const DEFAULT_SIGNER = "remote-settings.content-signature.mozilla.org";
const SIGNERS_BY_BUCKET = {
  "security-state": "onecrl.content-signature.mozilla.org",
  "security-state-preview": "onecrl.content-signature.mozilla.org",
};

ChromeUtils.defineLazyGetter(lazy, "gPrefs", () => {
  return Services.prefs.getBranch(PREF_SETTINGS_BRANCH);
});
ChromeUtils.defineLazyGetter(lazy, "console", () => lazy.Utils.log);

ChromeUtils.defineLazyGetter(lazy, "gSyncHistory", () => {
  const prefSize = lazy.gPrefs.getIntPref(PREF_SETTINGS_SYNC_HISTORY_SIZE, 100);
  const size = Math.min(Math.max(prefSize, 1000), 10);
  return new lazy.SyncHistory(TELEMETRY_SOURCE_SYNC, { size });
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "gPrefBrokenSyncThreshold",
  PREF_SETTINGS_BRANCH + PREF_SETTINGS_SYNC_HISTORY_ERROR_THRESHOLD,
  10
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "gPrefDestroyBrokenEnabled",
  PREF_SETTINGS_BRANCH + "destroy_broken_db_enabled",
  true
);

function cacheProxy(target) {
  const cache = new Map();
  return new Proxy(target, {
    get(innerTarget, prop) {
      if (!cache.has(prop)) {
        cache.set(prop, innerTarget[prop]);
      }
      return cache.get(prop);
    },
  });
}

class JexlFilter {
  constructor(environment, collectionName) {
    this._environment = environment;
    this._collectionName = collectionName;
    this._cachedResultForExpression = new Map();
    this._context = {
      env: environment,
    };
  }

  async filterEntry(entry) {
    const { filter_expression } = entry;
    if (!filter_expression) {
      return entry;
    }
    let result = this._cachedResultForExpression.get(filter_expression);
    if (result === undefined) {
      try {
        result = Boolean(
          await lazy.FilterExpressions.eval(filter_expression, this._context)
        );
      } catch (e) {
        console.error(
          e,
          "Full expression: " + filter_expression,
          this._collectionName
        );
      }
      this._cachedResultForExpression.set(filter_expression, result);
    }
    return result ? entry : null;
  }
}

export async function jexlFilterCreator(environment, collectionName) {
  const cachedEnvironment = cacheProxy(environment);
  return new JexlFilter(cachedEnvironment, collectionName);
}

function remoteSettingsFunction() {
  const _clients = new Map();
  let _invalidatePolling = false;

  const defaultOptions = {
    signerName: DEFAULT_SIGNER,
    filterCreator: jexlFilterCreator,
  };

  const remoteSettings = function (collectionName, options) {
    if (!_clients.has(collectionName)) {
      const c = new lazy.RemoteSettingsClient(collectionName, {
        ...defaultOptions,
        ...options,
      });
      _clients.set(collectionName, c);
      _invalidatePolling = true;
      lazy.console.debug(`Instantiated new client ${c.identifier}`);
    }
    return _clients.get(collectionName);
  };

  async function _client(bucketName, collectionName) {
    const client = _clients.get(collectionName);
    if (client && client.bucketName == bucketName) {
      return client;
    }
    if (
      bucketName ==
      lazy.Utils.actualBucketName(AppConstants.REMOTE_SETTINGS_DEFAULT_BUCKET)
    ) {
      const c = new lazy.RemoteSettingsClient(collectionName, defaultOptions);
      const [dbExists, localDump] = await Promise.all([
        lazy.Utils.hasLocalData(c),
        lazy.Utils.hasLocalDump(bucketName, collectionName),
      ]);
      if (dbExists || localDump) {
        return c;
      }
    }
    lazy.console.debug(`No known client for ${bucketName}/${collectionName}`);
    return null;
  }

  async function isSynchronizationBroken() {
    const threshold = Math.min(lazy.gPrefBrokenSyncThreshold, 20);
    const pastEntries = await lazy.gSyncHistory.list();
    const lastSuccessIdx = pastEntries.findIndex(
      e => e.status == lazy.UptakeTelemetry.STATUS.SUCCESS
    );
    return (
      lastSuccessIdx >= threshold ||
      (lastSuccessIdx < 0 && pastEntries.length >= threshold)
    );
  }

  remoteSettings.pullStartupBundle = async () => {
    if (lazy.Utils.shouldSkipRemoteActivity) {
      return [];
    }

    if (remoteSettings._ongoingExtractBundlePromise) {
      return await remoteSettings._ongoingExtractBundlePromise;
    }

    const startedAt = new Date();
    let extractedAt;
    remoteSettings._ongoingExtractBundlePromise = (async () => {
      lazy.console.info("Download Remote Settings startup changesets bundle.");

      let changesets;
      try {
        changesets = await lazy.Utils.fetchChangesetsBundle();
      } catch (e) {
        lazy.console.error(
          `Remote Settings startup changesets bundle could not be extracted (${e})`
        );
        return [];
      }

      extractedAt = new Date();
      const pulled = [];
      for (const changeset of changesets) {
        const bucket = lazy.Utils.actualBucketName(changeset.metadata.bucket);
        const collection = changeset.metadata.id;
        const identifier = `${bucket}/${collection}`;

        if (pulled.includes(identifier)) {
          continue;
        }

        const { metadata, timestamp, changes: records } = changeset;

        const signerName = SIGNERS_BY_BUCKET[bucket] || DEFAULT_SIGNER;
        const client = RemoteSettings(collection, {
          bucketName: bucket,
          signerName,
        });
        if (client.verifySignature) {
          lazy.console.debug(
            `${identifier}: Verify signature of bundled changeset`
          );
          try {
            await client.validateCollectionSignature(
              records,
              timestamp,
              metadata
            );
          } catch (e) {
            lazy.console.error(
              `${identifier}: Signature of bundled changeset is invalid: ${e}.`
            );
            continue;
          }
        }
        await client.db.importChanges(metadata, timestamp, records, {
          clear: true,
        });
        lazy.console.debug(`${identifier} imported from changesets bundle`);
        pulled.push(identifier);
      }
      return pulled;
    })();
    const pulled = await RemoteSettings._ongoingExtractBundlePromise;
    const durationMilliseconds = new Date() - startedAt;
    const downloadMilliseconds = extractedAt - startedAt;
    const extractMilliseconds = durationMilliseconds - downloadMilliseconds;
    lazy.console.info(
      `Import of changesets bundle done (duration=${durationMilliseconds}ms, download=${downloadMilliseconds}ms, extraction=${extractMilliseconds}ms)`
    );
    return pulled;
  };

  remoteSettings.pollChanges = async ({
    expectedTimestamp,
    trigger = "manual",
    full = false,
    // eslint-disable-next-line complexity
  } = {}) => {
    if (lazy.Utils.shouldSkipRemoteActivity) {
      return;
    }
    if (full) {
      lazy.gPrefs.clearUserPref(PREF_SETTINGS_SERVER_BACKOFF);
      lazy.gPrefs.clearUserPref(PREF_SETTINGS_LAST_UPDATE);
      lazy.gPrefs.clearUserPref(PREF_SETTINGS_LAST_ETAG);
    }

    let pollTelemetryArgs = {
      source: TELEMETRY_SOURCE_POLL,
      trigger,
    };

    if (lazy.Utils.isOffline) {
      lazy.console.info("Network is offline. Give up.");
      await lazy.UptakeTelemetry.report(
        lazy.UptakeTelemetry.STATUS.NETWORK_OFFLINE_ERROR,
        pollTelemetryArgs
      );
      return;
    }

    const startedAt = new Date();

    if (lazy.gPrefs.prefHasUserValue(PREF_SETTINGS_SERVER_BACKOFF)) {
      const backoffReleaseTime = lazy.gPrefs.getStringPref(
        PREF_SETTINGS_SERVER_BACKOFF
      );
      const remainingMilliseconds =
        parseInt(backoffReleaseTime, 10) - Date.now();
      if (remainingMilliseconds > 0) {
        await lazy.UptakeTelemetry.report(
          lazy.UptakeTelemetry.STATUS.BACKOFF,
          pollTelemetryArgs
        );
        throw new Error(
          `Server is asking clients to back off; retry in ${Math.ceil(
            remainingMilliseconds / 1000
          )}s.`
        );
      } else {
        lazy.gPrefs.clearUserPref(PREF_SETTINGS_SERVER_BACKOFF);
      }
    }

    if (
      lazy.gPrefDestroyBrokenEnabled &&
      trigger == "timer" &&
      (await isSynchronizationBroken())
    ) {
      const lastStatus = await lazy.gSyncHistory.last();
      const lastErrorClass =
        lazy.RemoteSettingsClient[lastStatus?.infos?.errorName] || Error;
      const isLocalError = !(
        lastErrorClass.prototype instanceof lazy.RemoteSettingsClient.APIError
      );
      if (isLocalError) {
        console.warn(
          "Synchronization has failed consistently. Destroy database."
        );
        lazy.gPrefs.clearUserPref(PREF_SETTINGS_LAST_ETAG);
        await lazy.gSyncHistory.clear().catch(error => console.error(error));
        await lazy.Database.destroy().catch(error => console.error(error));
      } else {
        console.warn(
          `Synchronization is broken, but last error is ${lastStatus}`
        );
      }
    }

    const lastEtag = _invalidatePolling
      ? ""
      : lazy.gPrefs.getStringPref(PREF_SETTINGS_LAST_ETAG, "");

    if (
      trigger == "startup" &&
      lastEtag &&
      expectedTimestamp &&
      lastEtag > expectedTimestamp
    ) {
      await lazy.UptakeTelemetry.report(
        lazy.UptakeTelemetry.STATUS.STALE_EXPECTED,
        pollTelemetryArgs
      );
      return;
    }

    lazy.console.info(`Start polling for changes (trigger=${trigger})`);
    Services.obs.notifyObservers(
      null,
      "remote-settings:changes-poll-start",
      JSON.stringify({ expectedTimestamp })
    );

    let pollResult;
    try {
      pollResult = await lazy.Utils.fetchLatestChanges(lazy.Utils.SERVER_URL, {
        expectedTimestamp,
        lastEtag,
      });
    } catch (e) {
      let reportStatus;
      if (/JSON\.parse/.test(e.message)) {
        reportStatus = lazy.UptakeTelemetry.STATUS.PARSE_ERROR;
      } else if (/content-type/.test(e.message)) {
        reportStatus = lazy.UptakeTelemetry.STATUS.CONTENT_ERROR;
      } else if (/Server/.test(e.message)) {
        reportStatus = lazy.UptakeTelemetry.STATUS.SERVER_ERROR;
        lazy.gPrefs.clearUserPref(PREF_SETTINGS_LAST_ETAG);
      } else if (/Timeout/.test(e.message)) {
        reportStatus = lazy.UptakeTelemetry.STATUS.TIMEOUT_ERROR;
      } else if (/NetworkError/.test(e.message)) {
        reportStatus = lazy.UptakeTelemetry.STATUS.NETWORK_ERROR;
      } else {
        reportStatus = lazy.UptakeTelemetry.STATUS.UNKNOWN_ERROR;
      }
      await lazy.UptakeTelemetry.report(reportStatus, pollTelemetryArgs);
      throw new Error(`Polling for changes failed: ${e.message}.`);
    }

    const { serverTimeMillis, changes, timestamp, backoffSeconds, ageSeconds } =
      pollResult;

    pollTelemetryArgs = { age: ageSeconds, ...pollTelemetryArgs };

    const reportStatus =
      changes.length === 0
        ? lazy.UptakeTelemetry.STATUS.UP_TO_DATE
        : lazy.UptakeTelemetry.STATUS.SUCCESS;
    await lazy.UptakeTelemetry.report(reportStatus, pollTelemetryArgs);

    if (backoffSeconds) {
      lazy.console.info(
        "Server asks clients to backoff for ${backoffSeconds} seconds"
      );
      const backoffReleaseTime = Date.now() + backoffSeconds * 1000;
      lazy.gPrefs.setStringPref(
        PREF_SETTINGS_SERVER_BACKOFF,
        backoffReleaseTime
      );
    }

    const clockDifference = Math.floor((Date.now() - serverTimeMillis) / 1000);
    lazy.gPrefs.setIntPref(PREF_SETTINGS_CLOCK_SKEW_SECONDS, clockDifference);
    const checkedServerTimeInSeconds = Math.round(serverTimeMillis / 1000);
    lazy.gPrefs.setIntPref(
      PREF_SETTINGS_LAST_UPDATE,
      checkedServerTimeInSeconds
    );

    let firstError;
    for (const change of changes) {
      const { bucket, collection, last_modified } = change;

      const client = await _client(bucket, collection);
      if (!client) {
        continue;
      }
      try {
        await client.maybeSync(last_modified, { trigger });

        Services.prefs.setIntPref(
          client.lastCheckTimePref,
          checkedServerTimeInSeconds
        );
      } catch (e) {
        lazy.console.error(e);
        if (!firstError) {
          firstError = e;
          firstError.details = change;
        }
      }
    }

    _invalidatePolling = false;

    const durationMilliseconds = new Date() - startedAt;
    const syncTelemetryArgs = {
      source: TELEMETRY_SOURCE_SYNC,
      duration: durationMilliseconds,
      timestamp,
      trigger,
    };

    if (firstError) {
      const status = lazy.UptakeTelemetry.STATUS.SYNC_ERROR;
      await lazy.UptakeTelemetry.report(status, syncTelemetryArgs);
      await lazy.gSyncHistory
        .store(timestamp, status, {
          expectedTimestamp,
          errorName: firstError.name,
        })
        .catch(error => console.error(error));
      Services.obs.notifyObservers(
        { wrappedJSObject: { error: firstError } },
        "remote-settings:sync-error"
      );

      if (await isSynchronizationBroken()) {
        await lazy.UptakeTelemetry.report(
          lazy.UptakeTelemetry.STATUS.SYNC_BROKEN_ERROR,
          syncTelemetryArgs
        );

        Services.obs.notifyObservers(
          { wrappedJSObject: { error: firstError } },
          "remote-settings:broken-sync-error"
        );
      }

      throw firstError;
    }

    lazy.gPrefs.setStringPref(PREF_SETTINGS_LAST_ETAG, timestamp);

    const status = lazy.UptakeTelemetry.STATUS.SUCCESS;
    await lazy.UptakeTelemetry.report(status, syncTelemetryArgs);
    await lazy.gSyncHistory
      .store(timestamp, status)
      .catch(error => console.error(error));

    lazy.console.info(
      `Polling for changes done (duration=${durationMilliseconds}ms)`
    );
    Services.obs.notifyObservers(null, "remote-settings:changes-poll-end");
  };

  remoteSettings.enablePreviewMode = enabled => {
    lazy.Utils.enablePreviewMode(enabled);
    for (const client of _clients.values()) {
      client.refreshBucketName();
    }
  };

  remoteSettings.inspect = async (options = {}) => {
    const { localOnly = false } = options;

    let changes = [];
    let serverTimestamp = null;
    if (!localOnly) {
      const randomCacheBust = 99990000 + Math.floor(Math.random() * 9999);
      ({ changes, timestamp: serverTimestamp } =
        await lazy.Utils.fetchLatestChanges(lazy.Utils.SERVER_URL, {
          expected: randomCacheBust,
        }));
    }
    const collections = await Promise.all(
      changes.map(async change => {
        const { bucket, collection, last_modified: remoteTimestamp } = change;
        const client = await _client(bucket, collection);
        if (!client) {
          return null;
        }
        const localTimestamp = await client.getLastModified();
        const lastCheck = Services.prefs.getIntPref(
          client.lastCheckTimePref,
          0
        );
        return {
          bucket,
          collection,
          localTimestamp,
          serverTimestamp: remoteTimestamp,
          lastCheck,
          signerName: client.signerName,
        };
      })
    );

    const jexlContext = {
      ...["channel", "version", "locale", "country", "formFactor"].reduce(
        (acc, key) => {
          acc[key] = lazy.ClientEnvironmentBase[key];
          return acc;
        },
        {}
      ),
      os: ["name", "version"].reduce((acc, key) => {
        acc[key] = lazy.ClientEnvironmentBase.os?.[key];
        return acc;
      }, {}),
      appinfo: ["ID", "OS"].reduce((acc, key) => {
        acc[key] = lazy.ClientEnvironmentBase.appinfo?.[key];
        return acc;
      }, {}),
    };

    return {
      serverURL: lazy.Utils.SERVER_URL,
      pollingEndpoint: lazy.Utils.SERVER_URL + lazy.Utils.CHANGES_PATH,
      serverTimestamp,
      localTimestamp: lazy.gPrefs.getStringPref(PREF_SETTINGS_LAST_ETAG, null),
      lastCheck: lazy.gPrefs.getIntPref(PREF_SETTINGS_LAST_UPDATE, 0),
      mainBucket: lazy.Utils.actualBucketName(
        AppConstants.REMOTE_SETTINGS_DEFAULT_BUCKET
      ),
      defaultSigner: DEFAULT_SIGNER,
      previewMode: lazy.Utils.PREVIEW_MODE,
      collections: collections.filter(c => !!c),
      history: {
        [TELEMETRY_SOURCE_SYNC]: await lazy.gSyncHistory.list(),
      },
      isSynchronizationBroken: await isSynchronizationBroken(),
      jexlContext,
    };
  };

  remoteSettings.clearAll = async () => {
    const { collections } = await remoteSettings.inspect();
    await Promise.all(
      collections.map(async ({ collection }) => {
        const client = RemoteSettings(collection);
        await client.attachments.deleteAll();
        await client.db.clear();
        Services.prefs.clearUserPref(client.lastCheckTimePref);
      })
    );
  };

  remoteSettings.init = () => {
    lazy.console.info("Initialize Remote Settings");
    const currentVersion = lazy.gPrefs.getStringPref(
      PREF_SETTINGS_LAST_ETAG,
      '"0"'
    );

    const moduleInfo = {
      moduleURI: import.meta.url,
      symbolName: "remoteSettingsBroadcastHandler",
    };
    lazy.pushBroadcastService.addListener(
      BROADCAST_ID,
      currentVersion,
      moduleInfo
    );
  };

  return remoteSettings;
}

export var RemoteSettings = remoteSettingsFunction();

export var remoteSettingsBroadcastHandler = {
  async receivedBroadcastMessage(version, broadcastID, context) {
    const { phase } = context;
    const isStartup = [
      lazy.pushBroadcastService.PHASES.HELLO,
      lazy.pushBroadcastService.PHASES.REGISTER,
    ].includes(phase);

    lazy.console.info(
      `Push notification received (version=${version} phase=${phase})`
    );

    return RemoteSettings.pollChanges({
      expectedTimestamp: version.replaceAll('"', ""),
      trigger: isStartup ? "startup" : "broadcast",
    });
  },
};
