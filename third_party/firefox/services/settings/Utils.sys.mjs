/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { ServiceRequest } from "resource://gre/modules/ServiceRequest.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import { setTimeout } from "resource://gre/modules/Timer.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  SharedUtils: "resource://services-settings/SharedUtils.sys.mjs",
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "CaptivePortalService",
  "@mozilla.org/network/captive-portal-service;1",
  Ci.nsICaptivePortalService
);
XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "gNetworkLinkService",
  "@mozilla.org/network/network-link-service;1",
  Ci.nsINetworkLinkService
);

const log = (() => {
  const { ConsoleAPI } = ChromeUtils.importESModule(
    "resource://gre/modules/Console.sys.mjs"
  );
  return new ConsoleAPI({
    maxLogLevel: "warn",
    maxLogLevelPref: "services.settings.loglevel",
    prefix: "services.settings",
  });
})();

ChromeUtils.defineLazyGetter(lazy, "allowServerURL", () => {
  if (!AppConstants.RELEASE_OR_BETA) {
    return true;
  }

  if (Services.env.get("MOZ_REMOTE_SETTINGS_DEVTOOLS") === "1") {
    return true;
  }

  // eslint-disable-next-line mozilla/valid-lazy
  if (AppConstants.REMOTE_SETTINGS_SERVER_URLS.includes(lazy.gServerURL)) {
    return true;
  }

  log.warn("Ignoring preference override of remote settings server");
  log.warn(
    "Allow by setting MOZ_REMOTE_SETTINGS_DEVTOOLS=1 in the environment"
  );
  return false;
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "gServerURL",
  "services.settings.server",
  AppConstants.REMOTE_SETTINGS_SERVER_URLS[0]
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "gPreviewEnabled",
  "services.settings.preview_enabled",
  false
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "gAttachmentsBaseUrl",
  "services.settings.base_attachments_url",
  ""
);

function _isUndefined(value) {
  return typeof value === "undefined";
}

export var Utils = {
  get SERVER_URL() {
    return lazy.allowServerURL
      ? // eslint-disable-next-line mozilla/valid-lazy
        lazy.gServerURL
      : AppConstants.REMOTE_SETTINGS_SERVER_URLS[0];
  },

  CHANGES_PATH: "/buckets/monitor/collections/changes/changeset",

  log,

  get shouldSkipRemoteActivity() {
    if (AppConstants.MOZ_MINIMAL_BROWSER) {
      return true;
    }
    return false;
  },

  get CERT_CHAIN_ROOT_IDENTIFIER() {
    if (
      this.SERVER_URL.match(
        /^https?:\/\/(remote-settings\.localhost|127\.0\.0\.1|localhost)(:\d+)?\/v1/
      )
    ) {
      return Ci.nsIContentSignatureVerifier.ContentSignatureLocalRoot;
    }
    if (this.SERVER_URL.includes("allizom.")) {
      return Ci.nsIContentSignatureVerifier.ContentSignatureStageRoot;
    }
    if (this.SERVER_URL.includes("dev.")) {
      return Ci.nsIContentSignatureVerifier.ContentSignatureDevRoot;
    }
    return Ci.nsIContentSignatureVerifier.ContentSignatureProdRoot;
  },

  get LOAD_DUMPS() {
    return AppConstants.REMOTE_SETTINGS_SERVER_URLS.includes(this.SERVER_URL);
  },

  get PREVIEW_MODE() {
    if (
      AppConstants.RELEASE_OR_BETA &&
      Services.env.get("MOZ_REMOTE_SETTINGS_DEVTOOLS") !== "1"
    ) {
      return false;
    }
    if (_isUndefined(this._previewModeEnabled)) {
      return lazy.gPreviewEnabled;
    }
    return !!this._previewModeEnabled;
  },

  enablePreviewMode(enabled) {
    const bool2str = v =>
      // eslint-disable-next-line no-nested-ternary
      _isUndefined(v) ? "unset" : v ? "enabled" : "disabled";
    this.log.debug(
      `Preview mode: ${bool2str(this._previewModeEnabled)} -> ${bool2str(
        enabled
      )}`
    );
    this._previewModeEnabled = enabled;
  },

  actualBucketName(bucketName) {
    let actual = bucketName.replace("-preview", "");
    if (this.PREVIEW_MODE) {
      actual += "-preview";
    }
    return actual;
  },

  get isOffline() {
    try {
      return (
        Services.io.offline ||
        lazy.CaptivePortalService.state ==
          lazy.CaptivePortalService.LOCKED_PORTAL ||
        !lazy.gNetworkLinkService.isLinkUp
      );
    } catch (ex) {
      log.warn("Could not determine network status.", ex);
    }
    return false;
  },

  async fetch(input, init = {}) {
    return new Promise(function (resolve, reject) {
      const request = new ServiceRequest();
      function fallbackOrReject(err) {
        if (
          bypassProxy ||
          Services.startup.shuttingDown ||
          Utils.isOffline ||
          !request.isProxied ||
          !request.bypassProxyEnabled
        ) {
          reject(err);
          return;
        }
        resolve(Utils.fetch(input, { ...init, bypassProxy: true }));
      }

      request.onerror = () =>
        fallbackOrReject(new TypeError("NetworkError: Network request failed"));
      request.ontimeout = () =>
        fallbackOrReject(new TypeError("Timeout: Network request failed"));
      request.onabort = () =>
        fallbackOrReject(new DOMException("Aborted", "AbortError"));
      request.onload = () => {
        const headers = new Headers();
        const rawHeaders = request.getAllResponseHeaders();
        rawHeaders
          .trim()
          .split(/[\r\n]+/)
          .forEach(line => {
            const parts = line.split(": ");
            const header = parts.shift();
            const value = parts.join(": ");
            headers.set(header, value);
          });

        const responseAttributes = {
          status: request.status,
          statusText: request.statusText,
          url: request.responseURL,
          headers,
        };
        resolve(new Response(request.response, responseAttributes));
      };

      const { method = "GET", headers = {}, bypassProxy = false } = init;

      request.open(method, input, { bypassProxy });
      request.responseType = "arraybuffer";

      for (const [name, value] of Object.entries(headers)) {
        request.setRequestHeader(name, value);
      }

      request.send();
    });
  },

  _baseAttachmentsURLPromise: null,

  async baseAttachmentsURL(options = {}) {
    if (Utils._baseAttachmentsURLPromise) {
      return Utils._baseAttachmentsURLPromise;
    }

    const [server_url = "", attachments_url = ""] =
      lazy.gAttachmentsBaseUrl.split("|", 2);
    if (
      server_url == Utils.SERVER_URL &&
      /^https?:\/\/(.+)\/$/.test(attachments_url)
    ) {
      return attachments_url;
    }

    const { retries = 0, retryWaitMsec = 1000 } = options;
    Utils._baseAttachmentsURLPromise = (async () => {
      let retried = 0;
      let serverInfo;
      while (retried <= retries) {
        try {
          const resp = await Utils.fetch(`${Utils.SERVER_URL}/`);
          if (!resp.ok) {
            throw new Error(`Failed to fetch server info: ${resp.status}`);
          }
          serverInfo = await resp.json();
          break;
        } catch (error) {
          retried++;
          if (retried > retries) {
            throw error;
          }
          await new Promise(resolve =>
            setTimeout(resolve, retryWaitMsec * retried)
          );
        }
      }
      const {
        capabilities: {
          attachments: { base_url },
        },
      } = serverInfo;
      Services.prefs.setStringPref(
        "services.settings.base_attachments_url",
        `${Utils.SERVER_URL}|${base_url}`
      );
      return base_url;
    })();

    try {
      return await Utils._baseAttachmentsURLPromise;
    } finally {
      Utils._baseAttachmentsURLPromise = null;
    }
  },

  async hasLocalData(client) {
    const timestamp = await client.db.getLastModified();
    return timestamp !== null;
  },

  async hasLocalDump(bucket, collection) {
    try {
      await fetch(
        `resource://app/defaults/settings/${bucket}/${collection}.json`,
        {
          method: "HEAD",
        }
      );
      return true;
    } catch (e) {
      return false;
    }
  },

  async getLocalDumpLastModified(bucket, collection) {
    if (!this._dumpStats) {
      if (!this._dumpStatsInitPromise) {
        this._dumpStatsInitPromise = (async () => {
          try {
            let res = await fetch(
              "resource://app/defaults/settings/last_modified.json"
            );
            this._dumpStats = await res.json();
          } catch (e) {
            log.warn(`Failed to load last_modified.json: ${e}`);
            this._dumpStats = {};
          }
          delete this._dumpStatsInitPromise;
        })();
      }
      await this._dumpStatsInitPromise;
    }
    const identifier = `${bucket}/${collection}`;
    let lastModified = this._dumpStats[identifier];
    if (lastModified === undefined) {
      const { timestamp: dumpTimestamp } = await lazy.SharedUtils.loadJSONDump(
        bucket,
        collection
      );
      lastModified = dumpTimestamp ?? -1;
      this._dumpStats[identifier] = lastModified;
    }
    return lastModified;
  },

  async fetchLatestChanges(serverUrl, options = {}) {
    const { expectedTimestamp, lastEtag = "", filters = {} } = options;

    let url = serverUrl + Utils.CHANGES_PATH;
    const params = {
      ...filters,
      _expected: expectedTimestamp ?? 0,
    };
    if (lastEtag != "") {
      params._since = lastEtag;
    }
    if (params) {
      url +=
        "?" +
        Object.entries(params)
          .map(([k, v]) => `${k}=${encodeURIComponent(v)}`)
          .join("&");
    }
    const response = await Utils.fetch(url);

    if (response.status >= 500) {
      throw new Error(`Server error ${response.status} ${response.statusText}`);
    }

    const is404FromCustomServer =
      response.status == 404 &&
      Services.prefs.prefHasUserValue("services.settings.server");

    const ct = response.headers.get("Content-Type");
    if (!is404FromCustomServer && (!ct || !ct.includes("application/json"))) {
      throw new Error(`Unexpected content-type "${ct}"`);
    }

    let payload;
    try {
      payload = await response.json();
    } catch (e) {
      payload = e.message;
    }

    if (!payload.hasOwnProperty("changes")) {
      if (!is404FromCustomServer) {
        throw new Error(
          `Server error ${url} ${response.status} ${
            response.statusText
          }: ${JSON.stringify(payload)}`
        );
      }
    }

    const { changes = [], timestamp } = payload;

    let serverTimeMillis = Date.parse(response.headers.get("Date"));
    const cacheAgeSeconds = response.headers.has("Age")
      ? parseInt(response.headers.get("Age"), 10)
      : 0;
    serverTimeMillis += cacheAgeSeconds * 1000;

    const ageSeconds = (serverTimeMillis - timestamp) / 1000;

    let backoffSeconds;
    if (response.headers.has("Backoff")) {
      const value = parseInt(response.headers.get("Backoff"), 10);
      if (!isNaN(value)) {
        backoffSeconds = value;
      }
    }

    return {
      changes,
      timestamp,
      serverTimeMillis,
      backoffSeconds,
      ageSeconds,
    };
  },

  filterObject(filters, entry) {
    return Object.entries(filters).every(([filter, value]) => {
      if (Array.isArray(value)) {
        return value.some(candidate => candidate === entry[filter]);
      } else if (typeof value === "object") {
        return Utils.filterObject(value, entry[filter]);
      } else if (!Object.prototype.hasOwnProperty.call(entry, filter)) {
        log.debug(`The property ${filter} does not exist`);
        return false;
      }
      return entry[filter] === value;
    });
  },

  sortObjects(order, list) {
    const hasDash = order[0] === "-";
    const field = hasDash ? order.slice(1) : order;
    const direction = hasDash ? -1 : 1;
    return list.slice().sort((a, b) => {
      if (a[field] && _isUndefined(b[field])) {
        return direction;
      }
      if (b[field] && _isUndefined(a[field])) {
        return -direction;
      }
      if (_isUndefined(a[field]) && _isUndefined(b[field])) {
        return 0;
      }
      return a[field] > b[field] ? direction : -direction;
    });
  },

  async fetchChangesetsBundle() {
    const tmpLz4File = await IOUtils.createUniqueFile(
      PathUtils.tempDir,
      "remote-settings-startup-bundle-"
    );
    try {
      const baseUrl = await Utils.baseAttachmentsURL();
      const bundleUrl = `${baseUrl}bundles/startup.json.mozlz4`;
      const bundleResp = await Utils.fetch(bundleUrl);
      if (!bundleResp.ok) {
        throw new Error(`Cannot fetch changeset bundle from ${bundleUrl}`);
      }
      const downloaded = await bundleResp.arrayBuffer();
      await IOUtils.write(tmpLz4File, new Uint8Array(downloaded), {
        tmpPath: `${tmpLz4File}.tmp`,
      });
      const changesetsJson = await IOUtils.readUTF8(tmpLz4File, {
        decompress: true,
      });
      return JSON.parse(changesetsJson);
    } finally {
      await IOUtils.remove(tmpLz4File, { ignoreAbsent: true });
    }
  },
};
