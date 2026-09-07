/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { DownloadList } from "resource://gre/modules/DownloadList.sys.mjs";
const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  Downloads: "resource://gre/modules/Downloads.sys.mjs",
  FileUtils: "resource://gre/modules/FileUtils.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
});

const HISTORY_PLACES_QUERY = `place:transition=${Ci.nsINavHistoryService.TRANSITION_DOWNLOAD}&sort=${Ci.nsINavHistoryQueryOptions.SORT_BY_DATE_DESCENDING}`;
const DESTINATIONFILEURI_ANNO = "downloads/destinationFileURI";
const METADATA_ANNO = "downloads/metaData";

const METADATA_STATE_FINISHED = 1;
const METADATA_STATE_FAILED = 2;
const METADATA_STATE_CANCELED = 3;
const METADATA_STATE_PAUSED = 4;

export let DownloadHistory = {
  async getList({ type = lazy.Downloads.PUBLIC, maxHistoryResults } = {}) {
    await DownloadCache.ensureInitialized();

    let key = `${type}|${maxHistoryResults ? maxHistoryResults : -1}`;
    if (!this._listPromises[key]) {
      this._listPromises[key] = lazy.Downloads.getList(type).then(list => {
        let query =
          HISTORY_PLACES_QUERY +
          (maxHistoryResults ? `&maxResults=${maxHistoryResults}` : "");

        return new DownloadHistoryList(list, query);
      });
    }

    return this._listPromises[key];
  },

  _listPromises: {},

  async addDownloadToHistory(download) {
    if (download.source.isPrivate) {
      return;
    }
    let sourceURI = URL.parse(download.source.url)?.URI;
    if (!sourceURI || !lazy.PlacesUtils.history.canAddURI(sourceURI)) {
      return;
    }

    await DownloadCache.addDownload(download);

    await this._updateHistoryListData(download.source.url);
  },

  async updateMetaData(download) {
    if (download.source.isPrivate || !download.stopped) {
      return;
    }
    let sourceURI = URL.parse(download.source.url)?.URI;
    if (!sourceURI || !lazy.PlacesUtils.history.canAddURI(sourceURI)) {
      return;
    }

    let state = METADATA_STATE_CANCELED;
    if (download.succeeded) {
      state = METADATA_STATE_FINISHED;
    } else if (download.error) {
      state = METADATA_STATE_FAILED;
    }

    let metaData = {
      state,
      deleted: download.deleted,
      endTime: download.endTime,
    };
    if (download.succeeded) {
      metaData.fileSize = download.target.size;
    }

    await DownloadCache.setMetadata(download.source.url, metaData);

    await this._updateHistoryListData(download.source.url);
  },

  async _updateHistoryListData(sourceUrl) {
    for (let key of Object.getOwnPropertyNames(this._listPromises)) {
      let downloadHistoryList = await this._listPromises[key];
      downloadHistoryList.updateForMetaDataChange(
        sourceUrl,
        DownloadCache.get(sourceUrl)
      );
    }
  },
};

let DownloadCache = {
  _data: new Map(),
  _initializePromise: null,

  ensureInitialized() {
    if (this._initializePromise) {
      return this._initializePromise;
    }
    this._initializePromise = (async () => {
      this._placesObserver = new PlacesWeakCallbackWrapper(
        this.handlePlacesEvents.bind(this)
      );
      PlacesObservers.addListener(
        ["history-cleared", "page-removed"],
        this._placesObserver
      );

      let pageAnnos = await lazy.PlacesUtils.history.fetchAnnotatedPages([
        METADATA_ANNO,
        DESTINATIONFILEURI_ANNO,
      ]);

      let metaDataPages = pageAnnos.get(METADATA_ANNO);
      if (metaDataPages) {
        for (let { uri, content } of metaDataPages) {
          try {
            this._data.set(uri.href, JSON.parse(content));
          } catch (ex) {
          }
        }
      }

      let destinationFilePages = pageAnnos.get(DESTINATIONFILEURI_ANNO);
      if (destinationFilePages) {
        for (let { uri, content } of destinationFilePages) {
          let newData = this.get(uri.href);
          newData.targetFileSpec = content;
          this._data.set(uri.href, newData);
        }
      }
    })();

    return this._initializePromise;
  },

  get(url) {
    return this._data.get(url) || {};
  },

  async addDownload(download) {
    let sourceURI = URL.parse(download.source.url)?.URI;
    if (!sourceURI || !lazy.PlacesUtils.history.canAddURI(sourceURI)) {
      return;
    }

    await this.ensureInitialized();

    let targetFile = new lazy.FileUtils.File(download.target.path);
    let targetUri = Services.io.newFileURI(targetFile);

    this._data.set(download.source.url, { targetFileSpec: targetUri.spec });

    let originalPageInfo = await lazy.PlacesUtils.history.fetch(
      download.source.url
    );

    let pageInfo = await lazy.PlacesUtils.history.insert({
      url: download.source.url,
      title:
        (originalPageInfo && originalPageInfo.title) || targetFile.leafName,
      visits: [
        {
          date: download.startTime,
          transition: lazy.PlacesUtils.history.TRANSITIONS.DOWNLOAD,
          referrer: download.source.referrerInfo
            ? download.source.referrerInfo.originalReferrer
            : null,
        },
      ],
    });

    await lazy.PlacesUtils.history.update({
      annotations: new Map([["downloads/destinationFileURI", targetUri.spec]]),
      guid: pageInfo.guid,
      url: pageInfo.url,
    });
  },

  async setMetadata(url, metadata) {
    await this.ensureInitialized();

    let existingData = this.get(url);
    let newData = { ...metadata };
    if ("targetFileSpec" in existingData) {
      newData.targetFileSpec = existingData.targetFileSpec;
    }
    this._data.set(url, newData);

    try {
      await lazy.PlacesUtils.history.update({
        annotations: new Map([[METADATA_ANNO, JSON.stringify(metadata)]]),
        url,
      });
    } catch (ex) {
      console.error(ex);
    }
  },

  QueryInterface: ChromeUtils.generateQI(["nsISupportsWeakReference"]),

  handlePlacesEvents(events) {
    for (const event of events) {
      switch (event.type) {
        case "history-cleared": {
          this._data.clear();
          break;
        }
        case "page-removed": {
          if (event.isRemovedFromStore) {
            this._data.delete(event.url);
          }
          break;
        }
      }
    }
  },
};

class HistoryDownload {
  constructor(placesNode) {
    this.placesNode = placesNode;

    this.source = {
      url: placesNode.uri,
      isPrivate: false,
    };
    this.target = {
      path: undefined,
      exists: false,
      size: undefined,
    };

    this.endTime = placesNode.time / 1000;
  }

  slot = null;

  stopped = true;

  hasProgress = false;

  hasPartialData = false;

  updateFromMetaData(metaData) {
    try {
      this.target.path = Cc["@mozilla.org/network/protocol;1?name=file"]
        .getService(Ci.nsIFileProtocolHandler)
        .getFileFromURLSpec(metaData.targetFileSpec).path;
    } catch (ex) {
      this.target.path = undefined;
    }

    if ("state" in metaData) {
      this.succeeded = metaData.state == METADATA_STATE_FINISHED;
      this.canceled =
        metaData.state == METADATA_STATE_CANCELED ||
        metaData.state == METADATA_STATE_PAUSED;
      this.endTime = metaData.endTime;
      this.deleted = metaData.deleted;

      if (metaData.state == METADATA_STATE_FAILED) {
        this.error = { message: "History download failed." };
      } else {
        this.error = null;
      }

      this.target.exists = true;
      this.target.size = metaData.fileSize;
    } else {
      this.succeeded = !this.target.path;
      this.error = this.target.path ? { message: "Unstarted download." } : null;
      this.canceled = false;
      this.deleted = false;

      this.target.exists = false;
      this.target.size = undefined;
    }
  }

  async finalize() {}

  async refresh() {
    try {
      this.target.size = (await IOUtils.stat(this.target.path)).size;
      this.target.exists = true;
    } catch (ex) {
      this.target.exists = false;
    }

    this.slot.list._notifyAllViews("onDownloadChanged", this);
  }

  async manuallyRemoveData() {
    let { path } = this.target;
    if (this.target.path && this.succeeded) {
      await IOUtils.setPermissions(path, 0o660);
      await IOUtils.remove(path, { ignoreAbsent: true });
    }
    this.deleted = true;
    await this.refresh();
  }

}

function makeSlotKey(url) {
  return url.startsWith("data:") ? lazy.PlacesUtils.sha256(url) : url;
}

class DownloadSlot {
  constructor(list) {
    this.list = list;
  }

  #dataUrlSlotKey = null;

  set slotKey(url) {
    if (url.startsWith("data:")) {
      this.#dataUrlSlotKey = lazy.PlacesUtils.sha256(url);
    }
  }

  get slotKey() {
    return this.#dataUrlSlotKey ?? this.download.source.url;
  }

  sessionDownload = null;
  _historyDownload = null;

  get historyDownload() {
    return this._historyDownload;
  }

  set historyDownload(historyDownload) {
    this._historyDownload = historyDownload;
    if (historyDownload) {
      historyDownload.slot = this;
    }
  }

  get download() {
    return this.sessionDownload || this.historyDownload;
  }
}

class DownloadHistoryList extends DownloadList {
  constructor(publicList, place) {
    super();

    this._slots = [];
    this._slotsForUrl = new Map();
    this._slotForDownload = new WeakMap();

    publicList.addView(this);

    let query = {},
      options = {};
    lazy.PlacesUtils.history.queryStringToQuery(place, query, options);

    let result = lazy.PlacesUtils.history.executeQuery(
      query.value,
      options.value
    );
    result.addObserver(this);

    Services.obs.addObserver(() => {
      this.result = null;
    }, "quit-application-granted");
  }

  _result = null;

  _firstSessionSlotIndex = 0;

  get result() {
    return this._result;
  }

  set result(result) {
    if (this._result == result) {
      return;
    }

    if (this._result) {
      this._result.removeObserver(this);
      this._result.root.containerOpen = false;
    }

    this._result = result;

    if (this._result) {
      this._result.root.containerOpen = true;
    }
  }

  updateForMetaDataChange(sourceUrl, metaData) {
    let slotsForUrl = this._slotsForUrl.get(makeSlotKey(sourceUrl));
    if (!slotsForUrl) {
      return;
    }

    for (let slot of slotsForUrl) {
      if (slot.sessionDownload) {
        return;
      }
      slot.historyDownload.updateFromMetaData(metaData);
      this._notifyAllViews("onDownloadChanged", slot.download);
    }
  }

  _insertSlot({ slot, index, slotsForUrl }) {
    this._slots.splice(index, 0, slot);
    this._downloads.splice(index, 0, slot.download);
    if (!slot.sessionDownload) {
      this._firstSessionSlotIndex++;
    }

    slot.slotKey = slot.download.source.url;
    slotsForUrl.add(slot);
    this._slotsForUrl.set(slot.slotKey, slotsForUrl);

    this._notifyAllViews("onDownloadAdded", slot.download, {
      insertBefore: this._downloads[index + 1],
    });
  }

  _removeSlot({ slot, slotsForUrl }) {
    let index = this._slots.indexOf(slot);
    this._slots.splice(index, 1);
    this._downloads.splice(index, 1);
    if (this._firstSessionSlotIndex > index) {
      this._firstSessionSlotIndex--;
    }

    slotsForUrl.delete(slot);
    if (slotsForUrl.size == 0) {
      this._slotsForUrl.delete(slot.slotKey);
    }

    this._notifyAllViews("onDownloadRemoved", slot.download);
  }

  _insertPlacesNode(placesNode) {
    let slotsForUrl =
      this._slotsForUrl.get(makeSlotKey(placesNode.uri)) || new Set();

    if (slotsForUrl.size > 0) {
      for (let slot of slotsForUrl) {
        if (!slot.historyDownload) {
          slot.historyDownload = new HistoryDownload(placesNode);
        } else {
          slot.historyDownload.placesNode = placesNode;
        }
      }
      return;
    }

    let historyDownload = new HistoryDownload(placesNode);
    historyDownload.updateFromMetaData(DownloadCache.get(placesNode.uri));
    let slot = new DownloadSlot(this);
    slot.historyDownload = historyDownload;
    this._insertSlot({ slot, slotsForUrl, index: this._firstSessionSlotIndex });
  }

  containerStateChanged(node) {
    this.invalidateContainer(node);
  }

  invalidateContainer(container) {
    this._notifyAllViews("onDownloadBatchStarting");

    for (let index = this._slots.length - 1; index >= 0; index--) {
      let slot = this._slots[index];
      if (slot.sessionDownload) {
        slot.historyDownload = null;
      } else {
        let slotsForUrl = this._slotsForUrl.get(slot.slotKey);
        this._removeSlot({ slot, slotsForUrl });
      }
    }

    for (let index = container.childCount - 1; index >= 0; --index) {
      try {
        this._insertPlacesNode(container.getChild(index));
      } catch (ex) {
        console.error(ex);
      }
    }

    this._notifyAllViews("onDownloadBatchEnded");
  }

  nodeInserted(parent, placesNode) {
    this._insertPlacesNode(placesNode);
  }

  nodeRemoved(parent, placesNode) {
    let slotsForUrl = this._slotsForUrl.get(makeSlotKey(placesNode.uri));
    for (let slot of slotsForUrl) {
      if (slot.sessionDownload) {
        slot.historyDownload = null;
      } else {
        this._removeSlot({ slot, slotsForUrl });
      }
    }
  }

  nodeIconChanged() {}
  nodeTitleChanged() {}
  nodeKeywordChanged() {}
  nodeDateAddedChanged() {}
  nodeLastModifiedChanged() {}
  nodeHistoryDetailsChanged() {}
  nodeTagsChanged() {}
  sortingChanged() {}
  nodeMoved() {}
  nodeURIChanged() {}
  batching() {}

  onDownloadAdded(download) {
    let slotsForUrl =
      this._slotsForUrl.get(makeSlotKey(download.source.url)) || new Set();

    let slot = [...slotsForUrl][0];
    if (slot && !slot.sessionDownload) {
      this._removeSlot({ slot, slotsForUrl });
    } else {
      slot = new DownloadSlot(this);
    }
    slot.sessionDownload = download;
    this._insertSlot({ slot, slotsForUrl, index: this._slots.length });
    this._slotForDownload.set(download, slot);
  }

  onDownloadChanged(download) {
    let slot = this._slotForDownload.get(download);
    this._notifyAllViews("onDownloadChanged", slot.download);
  }

  onDownloadRemoved(download) {
    let slot = this._slotForDownload.get(download);
    let slotsForUrl = this._slotsForUrl.get(slot.slotKey);
    this._removeSlot({ slot, slotsForUrl });

    this._slotForDownload.delete(download);

    if (slotsForUrl.size == 0 && slot.historyDownload) {
      slot.historyDownload.updateFromMetaData(
        DownloadCache.get(download.source.url)
      );
      slot.sessionDownload = null;
      this._insertSlot({
        slot,
        slotsForUrl,
        index: this._firstSessionSlotIndex,
      });
    }
  }

  add() {
    throw new Error("Not implemented.");
  }

  remove() {
    throw new Error("Not implemented.");
  }

  removeFinished() {
    throw new Error("Not implemented.");
  }
}
