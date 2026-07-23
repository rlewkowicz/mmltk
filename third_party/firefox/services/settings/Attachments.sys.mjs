/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  RemoteSettingsWorker:
    "resource://services-settings/RemoteSettingsWorker.sys.mjs",
  Utils: "resource://services-settings/Utils.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "console", () => lazy.Utils.log);

class DownloadError extends Error {
  constructor(url, resp) {
    super(`Could not download ${url}`);
    this.name = "DownloadError";
    this.resp = resp;
  }
}

class DownloadBundleError extends Error {
  constructor(url, resp) {
    super(`Could not download bundle ${url}`);
    this.name = "DownloadBundleError";
    this.resp = resp;
  }
}

class BadContentError extends Error {
  constructor(path) {
    super(`${path} content does not match server hash`);
    this.name = "BadContentError";
  }
}

class ServerInfoError extends Error {
  constructor(error) {
    super(`Server response is invalid ${error}`);
    this.name = "ServerInfoError";
    this.original = error;
  }
}

class NotFoundError extends Error {
  constructor(url, resp) {
    super(`Could not find ${url} in cache or dump`);
    this.name = "NotFoundError";
    this.resp = resp;
  }
}

class LazyRecordAndBuffer {
  constructor(getRecordAndLazyBuffer) {
    this.getRecordAndLazyBuffer = getRecordAndLazyBuffer;
  }

  async _ensureRecordAndLazyBuffer() {
    if (!this.recordAndLazyBufferPromise) {
      this.recordAndLazyBufferPromise = this.getRecordAndLazyBuffer();
    }
    return this.recordAndLazyBufferPromise;
  }

  async getRecord() {
    try {
      return (await this._ensureRecordAndLazyBuffer()).record;
    } catch (e) {
      return null;
    }
  }

  async isMatchingRequestedRecord(requestedRecord) {
    const record = await this.getRecord();
    return (
      record &&
      record.last_modified === requestedRecord.last_modified &&
      record.attachment.size === requestedRecord.attachment.size &&
      record.attachment.hash === requestedRecord.attachment.hash
    );
  }

  async getResult() {
    const { record, readBuffer } = await this._ensureRecordAndLazyBuffer();
    if (!this.bufferPromise) {
      this.bufferPromise = readBuffer();
    }
    return { record, buffer: await this.bufferPromise };
  }
}

export class Downloader {
  static get DownloadError() {
    return DownloadError;
  }
  static get DownloadBundleError() {
    return DownloadBundleError;
  }
  static get BadContentError() {
    return BadContentError;
  }
  static get ServerInfoError() {
    return ServerInfoError;
  }
  static get NotFoundError() {
    return NotFoundError;
  }

  constructor(bucketName, collectionName, ...subFolders) {
    this.folders = ["settings", bucketName, collectionName, ...subFolders];
    this.bucketName = bucketName;
    this.collectionName = collectionName;
  }

  get cacheImpl() {
    throw new Error("This Downloader does not support caching");
  }

  async download(record, options) {
    return this.#fetchAttachment(record, options);
  }

  async cacheAll(force = false) {
    if (lazy.Utils.isOffline) {
      return null;
    }

    if (!force && (await this.cacheImpl.hasData())) {
      return null;
    }

    const BULK_SAVE_COUNT = 50;

    const url =
      (await lazy.Utils.baseAttachmentsURL()) +
      `bundles/${this.bucketName}--${this.collectionName}.zip`;
    const tmpZipFilePath = PathUtils.join(
      PathUtils.tempDir,
      `${Services.uuid.generateUUID().toString().slice(1, -1)}.zip`
    );
    let allSuccess = true;

    try {
      lazy.console.debug(
        `${this.bucketName}/${this.collectionName}: Fetch attachments bundle from ${url}`
      );
      const resp = await lazy.Utils.fetch(url);
      if (!resp.ok) {
        throw new Downloader.DownloadBundleError(url, resp);
      }

      const downloaded = await resp.arrayBuffer();
      await IOUtils.write(tmpZipFilePath, new Uint8Array(downloaded), {
        tmpPath: `${tmpZipFilePath}.tmp`,
      });

      const zipReader = Cc["@mozilla.org/libjar/zip-reader;1"].createInstance(
        Ci.nsIZipReader
      );

      const tmpZipFile = await IOUtils.getFile(tmpZipFilePath);
      zipReader.open(tmpZipFile);

      lazy.console.debug(
        `${this.bucketName}/${this.collectionName}: Read zip bundle content`
      );
      const cacheEntries = [];
      const zipFiles = Array.from(zipReader.findEntries("*.meta.json"));
      allSuccess = !!zipFiles.length;

      const logStep = Math.max(1, Math.floor(zipFiles.length / 10));
      for (let i = 0; i < zipFiles.length; i++) {
        const lastLoop = i == zipFiles.length - 1;
        if (i == 0 || i % logStep == 0 || lastLoop) {
          lazy.console.debug(
            `${this.bucketName}/${this.collectionName}: Extract attachment ${i + 1}/${zipFiles.length} from bundle`
          );
        }
        const entryName = zipFiles[i];
        try {
          const recordZStream = zipReader.getInputStream(entryName);
          const recordDataLength = recordZStream.available();
          const recordStream = Cc[
            "@mozilla.org/scriptableinputstream;1"
          ].createInstance(Ci.nsIScriptableInputStream);
          recordStream.init(recordZStream);
          const recordBytes = recordStream.readBytes(recordDataLength);
          const recordBlob = new Blob([recordBytes], {
            type: "application/json",
          });
          const record = JSON.parse(await recordBlob.text());
          recordZStream.close();
          recordStream.close();

          const zStream = zipReader.getInputStream(record.id);
          const dataLength = zStream.available();
          const stream = Cc[
            "@mozilla.org/scriptableinputstream;1"
          ].createInstance(Ci.nsIScriptableInputStream);
          stream.init(zStream);
          const fileBytes = stream.readBytes(dataLength);
          const blob = new Blob([fileBytes]);

          cacheEntries.push([record.id, { record, blob }]);

          stream.close();
          zStream.close();
        } catch (ex) {
          lazy.console.warn(
            `${this.bucketName}/${this.collectionName}: Unable to extract attachment of ${entryName}.`,
            ex
          );
          allSuccess = false;
        }

        if (lastLoop || cacheEntries.length == BULK_SAVE_COUNT) {
          try {
            await this.cacheImpl.setMultiple(cacheEntries);
          } catch (ex) {
            lazy.console.warn(
              `${this.bucketName}/${this.collectionName}: Unable to save attachments in cache`,
              ex
            );
            allSuccess = false;
          }
          cacheEntries.splice(0); 
        }
      }
    } catch (ex) {
      lazy.console.warn(
        `${this.bucketName}/${this.collectionName}: Unable to retrieve remote-settings attachment bundle.`,
        ex
      );
      return false;
    }

    return allSuccess;
  }

  async get(
    record,
    options = {
      attachmentId: record?.id,
    }
  ) {
    return this.#fetchAttachment(record, {
      ...options,
      avoidDownload: true,
      fallbackToCache: true,
      fallbackToDump: true,
    });
  }

  // eslint-disable-next-line complexity
  async #fetchAttachment(record, options) {
    let {
      retries,
      checkHash,
      attachmentId = record?.id,
      fallbackToCache = false,
      fallbackToDump = false,
      avoidDownload = false,
      cacheResult = true,
    } = options || {};
    if (!attachmentId) {
      throw new Error(
        "download() was called without attachmentId or `record.id`"
      );
    }

    if (!lazy.Utils.LOAD_DUMPS) {
      if (fallbackToDump) {
        lazy.console.warn(
          "#fetchAttachment: Forcing fallbackToDump to false due to Utils.LOAD_DUMPS being false"
        );
      }
      fallbackToDump = false;
    }

    const dumpInfo = new LazyRecordAndBuffer(() =>
      this._readAttachmentDump(attachmentId)
    );
    const cacheInfo = new LazyRecordAndBuffer(() =>
      this._readAttachmentCache(attachmentId)
    );

    if (fallbackToDump && record) {
      if (await dumpInfo.isMatchingRequestedRecord(record)) {
        try {
          lazy.console.debug(
            `${this.bucketName}/${this.collectionName}: Read attachment from dump for record ${record.id}`
          );
          return { ...(await dumpInfo.getResult()), _source: "dump_match" };
        } catch (e) {
          lazy.console.error(e);
        }
      }
    }

    if (record) {
      if (await cacheInfo.isMatchingRequestedRecord(record)) {
        try {
          lazy.console.debug(
            `${this.bucketName}/${this.collectionName}: Read attachment from cache for record ${record.id}`
          );
          return { ...(await cacheInfo.getResult()), _source: "cache_match" };
        } catch (e) {
          lazy.console.error(e);
        }
      }
    }

    let errorIfAllFails;

    if (!avoidDownload && record && record.attachment) {
      try {
        const newBuffer = await this.downloadAsBytes(record, {
          retries,
          checkHash,
        });
        if (cacheResult) {
          const blob = new Blob([newBuffer]);
          this.cacheImpl
            .set(attachmentId, { record, blob })
            .catch(e => lazy.console.error(e));
        }
        return { buffer: newBuffer, record, _source: "remote_match" };
      } catch (e) {
        errorIfAllFails = e;
      }
    }


    const cacheRecord = fallbackToCache && (await cacheInfo.getRecord());
    if (cacheRecord) {
      const dumpRecord = fallbackToDump && (await dumpInfo.getRecord());
      if (dumpRecord?.last_modified >= cacheRecord.last_modified) {
        try {
          lazy.console.debug(
            `${this.bucketName}/${this.collectionName}: Attachment fallback to fresh dump for record ${dumpRecord.id}`
          );
          return { ...(await dumpInfo.getResult()), _source: "dump_fallback" };
        } catch (e) {
          lazy.console.error(e);
        }
      }

      try {
        lazy.console.debug(
          `${this.bucketName}/${this.collectionName}: Attachment fallback to cache for record ${cacheRecord.id}`
        );
        return { ...(await cacheInfo.getResult()), _source: "cache_fallback" };
      } catch (e) {
        lazy.console.error(e);
      }
    }

    const fallbackDumpRecord = fallbackToDump && (await dumpInfo.getRecord());
    if (fallbackDumpRecord) {
      try {
        lazy.console.debug(
          `${this.bucketName}/${this.collectionName}: Attachment fallback to dump for record ${fallbackDumpRecord.id}`
        );
        return { ...(await dumpInfo.getResult()), _source: "dump_fallback" };
      } catch (e) {
        errorIfAllFails = e;
      }
    }

    if (errorIfAllFails) {
      throw errorIfAllFails;
    }

    if (avoidDownload) {
      throw new Downloader.NotFoundError(attachmentId);
    }
    throw new Downloader.DownloadError(attachmentId);
  }

  isDownloaded(record) {
    const cacheInfo = new LazyRecordAndBuffer(() =>
      this._readAttachmentCache(record.id)
    );
    return cacheInfo.isMatchingRequestedRecord(record);
  }

  async deleteDownloaded(record, options) {
    let { attachmentId = record?.id } = options || {};
    if (!attachmentId) {
      throw new Error(
        "deleteDownloaded() was called without attachmentId or `record.id`"
      );
    }
    return this.cacheImpl.delete(attachmentId);
  }

  async prune(excludeIds) {
    return this.cacheImpl.prune(excludeIds);
  }
  async downloadAsBytes(record, options = {}) {
    const {
      attachment: { location, hash, size },
    } = record;

    let baseURL;
    try {
      baseURL = await lazy.Utils.baseAttachmentsURL();
    } catch (error) {
      throw new Downloader.ServerInfoError(error);
    }

    const remoteFileUrl = baseURL + location;

    const { retries = 3, checkHash = true } = options;
    let retried = 0;
    while (true) {
      try {
        lazy.console.debug(
          `${this.bucketName}/${this.collectionName}: Download from ${remoteFileUrl} (attempt ${retried + 1}/${retries})`
        );
        const buffer = await this._fetchAttachment(remoteFileUrl);
        if (!checkHash) {
          return buffer;
        }
        if (
          await lazy.RemoteSettingsWorker.checkContentHash(buffer, size, hash)
        ) {
          return buffer;
        }
        throw new Downloader.BadContentError(location);
      } catch (e) {
        if (retried >= retries) {
          throw e;
        }
      }
      retried++;
    }
  }

  async _fetchAttachment(url) {
    const headers = new Headers();
    headers.set("Accept-Encoding", "gzip");
    const resp = await lazy.Utils.fetch(url, { headers });
    if (!resp.ok) {
      throw new Downloader.DownloadError(url, resp);
    }
    return resp.arrayBuffer();
  }

  async _readAttachmentCache(attachmentId) {
    const cached = await this.cacheImpl.get(attachmentId);
    if (!cached) {
      throw new Downloader.DownloadError(attachmentId);
    }
    return {
      record: cached.record,
      async readBuffer() {
        const buffer = await cached.blob.arrayBuffer();
        const { size, hash } = cached.record.attachment;
        if (
          await lazy.RemoteSettingsWorker.checkContentHash(buffer, size, hash)
        ) {
          return buffer;
        }
        throw new Downloader.BadContentError(attachmentId);
      },
    };
  }

  async _readAttachmentDump(attachmentId) {
    async function fetchResource(resourceUrl) {
      try {
        return await fetch(resourceUrl);
      } catch (e) {
        throw new Downloader.DownloadError(resourceUrl);
      }
    }
    const resourceUrlPrefix =
      Downloader._RESOURCE_BASE_URL + "/" + this.folders.join("/") + "/";
    const recordUrl = `${resourceUrlPrefix}${attachmentId}.meta.json`;
    const attachmentUrl = `${resourceUrlPrefix}${attachmentId}`;
    const record = await (await fetchResource(recordUrl)).json();
    return {
      record,
      async readBuffer() {
        return (await fetchResource(attachmentUrl)).arrayBuffer();
      },
    };
  }

  static _RESOURCE_BASE_URL = "resource://app/defaults";
}

export class UnstoredDownloader extends Downloader {
  get cacheImpl() {
    const cacheImpl = {
      get: async () => {},
      set: async () => {},
      setMultiple: async () => {},
      delete: async () => {},
      deleteMultiple: async () => {},
      prune: async () => {},
      hasData: async () => false,
    };
    Object.defineProperty(this, "cacheImpl", { value: cacheImpl });
    return cacheImpl;
  }
}
