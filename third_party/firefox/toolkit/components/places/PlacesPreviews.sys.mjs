/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { EventEmitter } from "resource://gre/modules/EventEmitter.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BackgroundPageThumbs: "resource://gre/modules/BackgroundPageThumbs.sys.mjs",
  PageThumbsStorage: "resource://gre/modules/PageThumbs.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  clearTimeout: "resource://gre/modules/Timer.sys.mjs",
  setTimeout: "resource://gre/modules/Timer.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "logger", function () {
  return lazy.PlacesUtils.getLogger({ prefix: "Previews" });
});

ChromeUtils.defineLazyGetter(lazy, "previewsEnabled", function () {
  return Services.prefs.getBoolPref("places.previews.enabled", false);
});

const DELETE_CHUNK_SIZE = 50;
const DELETE_TIMEOUT_MS = 60000;

const PREVIEWS_DIRECTORY = "places-previews";

const PREVIEW_FILENAME_RE = /^[a-f0-9]{64}\.webp$/;

const DAYS_BEFORE_REPLACEMENT = 30;

class LimitedSet extends Set {
  #limit = 100;
  add(key) {
    super.add(key);
    let oversize = this.size - this.#limit;
    if (oversize > 0) {
      for (let entry of this) {
        if (oversize-- <= 0) {
          break;
        }
        this.delete(entry);
      }
    }
    return this;
  }
}

class DeletionHandler {
  #timeoutId = null;
  #shutdownProgress = {};

  #timeout = DELETE_TIMEOUT_MS;
  get timeout() {
    return this.#timeout;
  }
  set timeout(val) {
    if (this.#timeoutId) {
      lazy.clearTimeout(this.#timeoutId);
      this.#timeoutId = null;
    }
    this.#timeout = val;
    this.ensureRunning();
  }

  constructor() {
    lazy.PlacesUtils.history.shutdownClient.jsclient.addBlocker(
      "PlacesPreviews.sys.mjs::DeletionHandler",
      async () => {
        this.#shutdownProgress.shuttingDown = true;
        lazy.clearTimeout(this.#timeoutId);
        this.#timeoutId = null;
      },
      { fetchState: () => this.#shutdownProgress }
    );
  }

  ensureRunning() {
    if (this.#timeoutId || this.#shutdownProgress.shuttingDown) {
      return;
    }
    this.#timeoutId = lazy.setTimeout(() => {
      this.#timeoutId = null;
      ChromeUtils.idleDispatch(() => {
        this.#deleteChunk().catch(ex =>
          lazy.logger.error("Error during previews deletion:" + ex)
        );
      });
    }, this.timeout);
  }

  async #deleteChunk() {
    if (this.#shutdownProgress.shuttingDown) {
      return;
    }
    let db = await lazy.PlacesUtils.promiseDBConnection();
    let count;
    let hashes = (
      await db.executeCached(
        `SELECT hash, (SELECT count(*) FROM moz_previews_tombstones) AS count
         FROM moz_previews_tombstones LIMIT ${DELETE_CHUNK_SIZE}`
      )
    ).map(r => {
      if (count === undefined) {
        count = r.getResultByName("count");
      }
      return r.getResultByName("hash");
    });
    if (!count || this.#shutdownProgress.shuttingDown) {
      return;
    }

    let deleted = [];
    for (let hash of hashes) {
      let filePath = PlacesPreviews.getPathForHash(hash);
      try {
        await IOUtils.remove(filePath);
        PlacesPreviews.onDelete(filePath);
        deleted.push(hash);
      } catch (ex) {
        if (DOMException.isInstance(ex) && ex.name == "NotFoundError") {
          deleted.push(hash);
        } else {
          lazy.logger.error("Unable to delete file: " + filePath);
        }
      }
      if (this.#shutdownProgress.shuttingDown) {
        return;
      }
    }
    let params = deleted.reduce((p, c, i) => {
      p["hash" + i] = c;
      return p;
    }, {});
    await lazy.PlacesUtils.withConnectionWrapper(
      "PlacesPreviews.sys.mjs::ExpirePreviews",
      async db => {
        await db.execute(
          `DELETE FROM moz_previews_tombstones WHERE hash in
            (${Object.keys(params)
              .map(p => `:${p}`)
              .join(",")})`,
          params
        );
      }
    );

    if (count > DELETE_CHUNK_SIZE) {
      this.ensureRunning();
    }
  }
}

export const PlacesPreviews = new (class extends EventEmitter {
  #placesObserver = null;
  #deletionHandler = null;
  #recentlyUpdatedPreviews = new LimitedSet();

  fileExtension = ".webp";
  fileContentType = "image/webp";

  constructor() {
    super();
    this.#placesObserver = new PlacesWeakCallbackWrapper(
      this.handlePlacesEvents.bind(this)
    );
    PlacesObservers.addListener(
      ["history-cleared", "page-removed"],
      this.#placesObserver
    );

    this.#deletionHandler = new DeletionHandler();
    this.#deletionHandler.ensureRunning();
  }

  handlePlacesEvents(events) {
    for (const event of events) {
      if (
        event.type == "history-cleared" ||
        (event.type == "page-removed" && event.isRemovedFromStore)
      ) {
        this.#deletionHandler.ensureRunning();
        return;
      }
    }
  }

  get enabled() {
    return lazy.previewsEnabled;
  }

  getPath() {
    return PathUtils.join(
      Services.dirsvc.get("ProfD", Ci.nsIFile).path,
      PREVIEWS_DIRECTORY
    );
  }

  getPathForUrl(url) {
    return PathUtils.join(
      this.getPath(),
      lazy.PlacesUtils.sha256(url, { format: "hex" }) + this.fileExtension
    );
  }

  getPathForHash(hash) {
    return PathUtils.join(this.getPath(), hash + this.fileExtension);
  }

  getPageThumbURL(url) {
    return (
      "moz-page-thumb://" +
      "places-previews" +
      "/?url=" +
      encodeURIComponent(url) +
      "&revision=" +
      lazy.PageThumbsStorage.getRevision(url)
    );
  }

  async update(url, { forceUpdate = false } = {}) {
    if (!this.enabled) {
      return false;
    }
    let filePath = this.getPathForUrl(url);
    if (!forceUpdate) {
      if (this.#recentlyUpdatedPreviews.has(filePath)) {
        lazy.logger.debug("Skipping update because recently updated");
        return true;
      }
      try {
        let fileInfo = await IOUtils.stat(filePath);
        if (
          fileInfo.lastModified >
          Date.now() - DAYS_BEFORE_REPLACEMENT * 86400000
        ) {
          this.#recentlyUpdatedPreviews.add(filePath);
          lazy.logger.debug("Skipping update because file is recent");
          return true;
        }
      } catch (ex) {
        if (!DOMException.isInstance(ex) || ex.name != "NotFoundError") {
          lazy.logger.error("Error while trying to stat() preview" + ex);
          return false;
        }
      }
    }

    let buffer = await new Promise(resolve => {
      let observer = (subject, topic, errorUrl) => {
        if (errorUrl == url) {
          resolve(null);
        }
      };
      Services.obs.addObserver(observer, "page-thumbnail:error");
      lazy.BackgroundPageThumbs.capture(url, {
        dontStore: true,
        contentType: this.fileContentType,
        onDone: (url, reason, handle) => {
          Services.obs.removeObserver(observer, "page-thumbnail:error");
          resolve(handle?.data);
        },
      });
    });
    if (!buffer) {
      lazy.logger.error("Unable to fetch preview: " + url);
      return false;
    }
    try {
      await IOUtils.makeDirectory(this.getPath(), { ignoreExisting: true });
      await IOUtils.write(filePath, new Uint8Array(buffer), {
        tmpPath: filePath + ".tmp",
      });
    } catch (ex) {
      lazy.logger.error("Unable to create preview: " + ex);
      return false;
    }
    this.#recentlyUpdatedPreviews.add(filePath);
    return true;
  }

  async deleteOrphans() {
    if (!this.enabled) {
      return false;
    }

    let files = await IOUtils.getChildren(this.getPath());
    let hashes = files
      .map(f => PathUtils.filename(f))
      .filter(f => PREVIEW_FILENAME_RE.test(f))
      .map(n => n.substring(0, n.lastIndexOf(".")));

    if (!hashes.length) {
      return true;
    }

    await lazy.PlacesUtils.withConnectionWrapper(
      "PlacesPreviews.sys.mjs::deleteOrphans",
      async db => {
        await db.executeTransaction(async () => {
          for (let chunk of lazy.PlacesUtils.chunkArray(
            hashes,
            db.variableLimit
          )) {
            let params = chunk.reduce((p, h, i) => {
              p["hash" + i] = h;
              return p;
            }, {});
            await db.execute(
              `
              WITH files(hash) AS (
                VALUES ${Object.keys(params)
                  .map(p => `(:${p})`)
                  .join(", ")}
              )
              INSERT OR IGNORE INTO moz_previews_tombstones
                SELECT hash FROM files
                EXCEPT
                SELECT sha256hex(url) FROM moz_places
              `,
              params
            );
          }
        });
      }
    );
    this.#deletionHandler.ensureRunning();
    return true;
  }

  onDelete(filePath) {
    this.#recentlyUpdatedPreviews.delete(filePath);
    this.emit("places-preview-deleted", filePath);
  }

  testSetDeletionTimeout(timeout) {
    if (timeout === null) {
      this.#deletionHandler.timeout = DELETE_TIMEOUT_MS;
    } else {
      this.#deletionHandler.timeout = timeout;
    }
  }
})();

export function PlacesPreviewsHelperService() {}

PlacesPreviewsHelperService.prototype = {
  classID: Components.ID("{bd0a4d3b-ff26-4d4d-9a62-a513e1c1bf92}"),
  QueryInterface: ChromeUtils.generateQI(["nsIPlacesPreviewsHelperService"]),

  getFilePathForURL(url) {
    return PlacesPreviews.getPathForUrl(url);
  },
};
