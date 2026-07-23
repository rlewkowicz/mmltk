/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  E10SUtils: "resource://gre/modules/E10SUtils.sys.mjs",
  NetUtil: "resource://gre/modules/NetUtil.sys.mjs",
});

const RESPONSE_HEADER_KEY_VALUE_DELIMETER = ": ";
const RESPONSE_HEADER_DELIMETER = "\r\n";
const RESPONSE_HEADER_METADATA_ELEMENT = "original-response-headers";

export class MozCachedOHTTPParent extends JSProcessActorParent {
  receiveMessage(message) {
    if (
      this.manager.remoteType !== null &&
      this.manager.remoteType !== lazy.E10SUtils.PRIVILEGEDABOUT_REMOTE_TYPE
    ) {
      return Promise.reject(new Error("Process type mismatch"));
    }

    switch (message.name) {
      case "tryCache": {
        let uri = Services.io.newURI(message.data.uriString);
        return this.tryCache(uri);
      }
      case "writeCache": {
        let uri = Services.io.newURI(message.data.uriString);
        return this.writeCache(
          uri,
          message.data.cacheInputStream,
          message.data.cacheStreamUpdatePort
        );
      }
    }

    return Promise.reject(new Error(`Unknown message: ${message.name}`));
  }

  async tryCache(resourceURI) {
    try {
      const cacheEntry = await this.#openCacheEntry(
        resourceURI,
        Ci.nsICacheStorage.OPEN_READONLY
      );

      if (!cacheEntry.dataSize) {
        throw new Error("Cache entry is empty.");
      }

      let headersStrings = cacheEntry
        .getMetaDataElement(RESPONSE_HEADER_METADATA_ELEMENT)
        .split(RESPONSE_HEADER_DELIMETER);
      let headersObj = {};
      for (let headersString of headersStrings) {
        let delimeterIndex = headersString.indexOf(
          RESPONSE_HEADER_KEY_VALUE_DELIMETER
        );
        let key = headersString.substring(0, delimeterIndex);
        let value = headersString.substring(
          delimeterIndex + RESPONSE_HEADER_KEY_VALUE_DELIMETER.length
        );
        headersObj[key] = value;
      }

      const inputStream = cacheEntry.openInputStream(0);

      return {
        success: true,
        inputStream,
        headersObj,
      };
    } catch (e) {
      return { success: false };
    }
  }

  async writeCache(resourceURI, cacheInputStream, cacheStreamUpdatePort) {
    let cacheEntry;
    let outputStream;

    try {
      cacheEntry = await this.#openCacheEntry(
        resourceURI,
        Ci.nsICacheStorage.OPEN_NORMALLY
      );
      outputStream = cacheEntry.openOutputStream(0, -1);
    } catch (e) {
      return;
    }

    cacheStreamUpdatePort.onmessage = msg => {
      switch (msg.data.name) {
        case "DoomCacheEntry": {
          cacheEntry.asyncDoom(null);
          break;
        }
        case "WriteOriginalResponseHeaders": {
          let headers = new Headers(msg.data.headersObj);
          let headersStrings = [];
          for (let [key, value] of headers.entries()) {
            headersStrings.push(
              `${key}${RESPONSE_HEADER_KEY_VALUE_DELIMETER}${value}`
            );
          }
          cacheEntry.setMetaDataElement(
            RESPONSE_HEADER_METADATA_ELEMENT,
            headersStrings.join(RESPONSE_HEADER_DELIMETER)
          );
          break;
        }
        case "WriteCacheExpiry": {
          cacheEntry.setExpirationTime(msg.data.expiry);
          break;
        }
      }
    };
    try {
      await new Promise(resolve => {
        lazy.NetUtil.asyncCopy(cacheInputStream, outputStream, writeResult => {
          if (!Components.isSuccessCode(writeResult)) {
            console.error(
              "Failed to cache moz-cached-ohttp resource with result: ",
              writeResult
            );
          }

          resolve();
        });
      });
    } finally {
      cacheStreamUpdatePort.onmessage = null;
    }
  }

  async #openCacheEntry(uri, openFlags) {
    const lci = Services.loadContextInfo.anonymous;
    const storage = Services.cache2.diskCacheStorage(lci);

    if (
      openFlags === Ci.nsICacheStorage.OPEN_READONLY &&
      !storage.exists(uri, "")
    ) {
      throw new Error("Cache entry does not exist.");
    }

    return new Promise((resolve, reject) => {
      storage.asyncOpenURI(uri, "", openFlags, {
        onCacheEntryCheck: () => Ci.nsICacheEntryOpenCallback.ENTRY_WANTED,
        onCacheEntryAvailable: (entry, _isNew, status) => {
          if (Components.isSuccessCode(status)) {
            resolve(entry);
          } else {
            reject(new Error(`Cache entry operation failed: ${status}`));
          }
        },
      });
    });
  }
}
