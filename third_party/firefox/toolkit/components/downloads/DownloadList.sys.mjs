/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const FILE_EXTENSIONS = [
  "aac",
  "adt",
  "adts",
  "accdb",
  "accde",
  "accdr",
  "accdt",
  "aif",
  "aifc",
  "aiff",
  "apng",
  "aspx",
  "avi",
  "avif",
  "bat",
  "bin",
  "bmp",
  "cab",
  "cda",
  "csv",
  "dif",
  "dll",
  "doc",
  "docm",
  "docx",
  "dot",
  "dotx",
  "eml",
  "eps",
  "exe",
  "flac",
  "flv",
  "gif",
  "htm",
  "html",
  "ico",
  "ini",
  "iso",
  "jar",
  "jfif",
  "jpg",
  "jpeg",
  "json",
  "m4a",
  "mdb",
  "mid",
  "midi",
  "mov",
  "mp3",
  "mp4",
  "mpeg",
  "mpg",
  "msi",
  "mui",
  "oga",
  "ogg",
  "ogv",
  "opus",
  "pdf",
  "pjpeg",
  "pjp",
  "png",
  "pot",
  "potm",
  "potx",
  "ppam",
  "pps",
  "ppsm",
  "ppsx",
  "ppt",
  "pptm",
  "pptx",
  "psd",
  "pst",
  "pub",
  "rar",
  "rdf",
  "rtf",
  "shtml",
  "sldm",
  "sldx",
  "svg",
  "swf",
  "sys",
  "tif",
  "tiff",
  "tmp",
  "txt",
  "vob",
  "vsd",
  "vsdm",
  "vsdx",
  "vss",
  "vssm",
  "vst",
  "vstm",
  "vstx",
  "wav",
  "wbk",
  "webm",
  "webp",
  "wks",
  "wma",
  "wmd",
  "wmv",
  "wmz",
  "wms",
  "wpd",
  "wp5",
  "xht",
  "xhtml",
  "xla",
  "xlam",
  "xll",
  "xlm",
  "xls",
  "xlsm",
  "xlsx",
  "xlt",
  "xltm",
  "xltx",
  "xml",
  "zip",
];

export class DownloadList {
  constructor() {
    this._downloads = [];

    this._views = new Set();
  }

  async getAll() {
    return Array.from(this._downloads);
  }

  async add(download) {
    this._downloads.push(download);
    download.onchange = this._change.bind(this, download);
    this._notifyAllViews("onDownloadAdded", download);
  }

  async remove(download) {
    let index = this._downloads.indexOf(download);
    if (index != -1) {
      this._downloads.splice(index, 1);
      download.onchange = null;
      this._notifyAllViews("onDownloadRemoved", download);
    }
  }

  _change(download) {
    this._notifyAllViews("onDownloadChanged", download);
  }

  addView(view) {
    this._views.add(view);

    if ("onDownloadAdded" in view) {
      this._notifyAllViews("onDownloadBatchStarting");
      for (let download of this._downloads) {
        try {
          view.onDownloadAdded(download);
        } catch (ex) {
          console.error(ex);
        }
      }
      this._notifyAllViews("onDownloadBatchEnded");
    }
  }

  removeView(view) {
    this._views.delete(view);
  }

  _notifyAllViews(methodName, ...args) {
    for (let view of this._views) {
      try {
        if (methodName in view) {
          view[methodName](...args);
        }
      } catch (ex) {
        console.error(ex);
      }
    }
  }

  removeFinished(filterFn) {
    (async () => {
      let list = await this.getAll();
      for (let download of list) {
        if (
          download.stopped &&
          (!download.hasPartialData || download.error) &&
          (!filterFn || filterFn(download))
        ) {
          await this.remove(download);
          let sameFileIsDownloading = false;
          for (let otherDownload of await this.getAll()) {
            if (
              download !== otherDownload &&
              download.target.path == otherDownload.target.path &&
              !otherDownload.error
            ) {
              sameFileIsDownloading = true;
            }
          }
          let removePartialData = !sameFileIsDownloading;
          download.finalize(removePartialData).catch(console.error);
        }
      }
    })().catch(console.error);
  }
}

export class DownloadCombinedList extends DownloadList {
  constructor(publicList, privateList) {
    super();

    this._publicList = publicList;

    this._privateList = privateList;

    publicList.addView(this);
    privateList.addView(this);
  }

  add(download) {
    let extension = download.target.path.split(".").pop();

    if (!FILE_EXTENSIONS.includes(extension)) {
      extension = "other";
    }


    if (download.source.isPrivate) {
      return this._privateList.add(download);
    }

    return this._publicList.add(download);
  }

  remove(download) {
    if (download.source.isPrivate) {
      return this._privateList.remove(download);
    }
    return this._publicList.remove(download);
  }

  onDownloadAdded(download) {
    this._downloads.push(download);
    this._notifyAllViews("onDownloadAdded", download);
  }

  onDownloadChanged(download) {
    this._notifyAllViews("onDownloadChanged", download);
  }

  onDownloadRemoved(download) {
    let index = this._downloads.indexOf(download);
    if (index != -1) {
      this._downloads.splice(index, 1);
    }
    this._notifyAllViews("onDownloadRemoved", download);
  }
}
export class DownloadSummary {
  constructor() {
    this._downloads = [];

    this._views = new Set();
  }

  _list = null;

  allHaveStopped = true;

  allUnknownSize = true;

  progressTotalBytes = 0;

  progressCurrentBytes = 0;

  async bindToList(list) {
    if (this._list) {
      throw new Error("bindToList may be called only once.");
    }

    await list.addView(this);
    this._list = list;
    this._onListChanged();
  }

  addView(view) {
    this._views.add(view);

    if ("onSummaryChanged" in view) {
      try {
        view.onSummaryChanged();
      } catch (ex) {
        console.error(ex);
      }
    }
  }

  removeView(view) {
    this._views.delete(view);
  }

  _onListChanged() {
    let allHaveStopped = true;
    let allUnknownSize = true;
    let progressTotalBytes = 0;
    let progressCurrentBytes = 0;

    for (let download of this._downloads) {
      if (download.isInCurrentBatch) {
        allHaveStopped = false;
        if (download.hasProgress) {
          allUnknownSize = false;
          progressTotalBytes += download.totalBytes;
        } else {
          progressTotalBytes += download.currentBytes;
        }
        progressCurrentBytes += download.currentBytes;
      }
    }

    if (
      this.allHaveStopped == allHaveStopped &&
      this.allUnknownSize == allUnknownSize &&
      this.progressTotalBytes == progressTotalBytes &&
      this.progressCurrentBytes == progressCurrentBytes
    ) {
      return;
    }

    this.allHaveStopped = allHaveStopped;
    this.allUnknownSize = allUnknownSize;
    this.progressTotalBytes = progressTotalBytes;
    this.progressCurrentBytes = progressCurrentBytes;

    for (let view of this._views) {
      try {
        if ("onSummaryChanged" in view) {
          view.onSummaryChanged();
        }
      } catch (ex) {
        console.error(ex);
      }
    }
  }

  onDownloadAdded(download) {
    this._downloads.push(download);
    if (this._list) {
      this._onListChanged();
    }
  }

  onDownloadChanged() {
    this._onListChanged();
  }

  onDownloadRemoved(download) {
    let index = this._downloads.indexOf(download);
    if (index != -1) {
      this._downloads.splice(index, 1);
    }
    this._onListChanged();
  }
}
