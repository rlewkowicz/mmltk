/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */



const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  DeferredTask: "resource://gre/modules/DeferredTask.sys.mjs",
  FileUtils: "resource://gre/modules/FileUtils.sys.mjs",
  NetUtil: "resource://gre/modules/NetUtil.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "gTextDecoder", function () {
  return new TextDecoder();
});

const FileInputStream = Components.Constructor(
  "@mozilla.org/network/file-input-stream;1",
  "nsIFileInputStream",
  "init"
);

const kSaveDelayMs = 1500;


export function JSONFile(config) {
  this.path = config.path;
  this.sanitizedBasename =
    config.sanitizedBasename ??
    PathUtils.filename(this.path)
      .replace(/\.json(.lz4)?$/, "")
      .replaceAll(/[^a-zA-Z0-9_.]/g, "");

  if (typeof config.dataPostProcessor === "function") {
    this._dataPostProcessor = config.dataPostProcessor;
  }
  if (typeof config.beforeSave === "function") {
    this._beforeSave = config.beforeSave;
  }

  if (config.saveDelayMs === undefined) {
    config.saveDelayMs = kSaveDelayMs;
  }
  this._saver = new lazy.DeferredTask(() => this._save(), config.saveDelayMs);

  this._options = {};
  if (config.compression) {
    this._options.decompress = this._options.compress = true;
  }

  if (config.backupTo) {
    this._options.backupFile = this._options.backupTo = config.backupTo;
  }

  if (config.saveFailureHandler) {
    this._saveFailureHandler = config.saveFailureHandler;
  }

  this._finalizeAt = config.finalizeAt || IOUtils.profileBeforeChange;
  this._finalizeInternalBound = this._finalizeInternal.bind(this);
  this._finalizeAt.addBlocker(
    `JSON store: writing data for '${this.sanitizedBasename}'`,
    this._finalizeInternalBound,
    () => ({ sanitizedBasename: this.sanitizedBasename })
  );
}

JSONFile.prototype = {
  path: "",

  sanitizedBasename: "",

  dataReady: false,

  _saver: null,

  _saveFailureHandler: () => {},

  _data: null,

  _finalizeAt: null,
  _finalizePromise: null,
  _finalizeInternalBound: null,

  get data() {
    if (!this.dataReady) {
      throw new Error("Data is not ready.");
    }
    return this._data;
  },

  set data(data) {
    this._data = data;
    this.dataReady = true;
  },

  async load() {
    if (this.dataReady) {
      return;
    }

    let data = {};

    try {
      data = await IOUtils.readJSON(this.path, this._options);

      if (this.dataReady) {
        return;
      }
    } catch (ex) {


      if (!(DOMException.isInstance(ex) && ex.name == "NotFoundError")) {
        console.error(ex);

        try {
          let uniquePath = await IOUtils.createUniqueFile(
            PathUtils.parent(this.path),
            PathUtils.filename(this.path) + ".corrupt",
            0o600
          );
          await IOUtils.move(this.path, uniquePath);
        } catch (e2) {
          console.error(e2);
        }
      }

      if (this._options.backupFile) {
        try {
          await IOUtils.copy(this._options.backupFile, this.path);
        } catch (e) {
          if (!(DOMException.isInstance(e) && e.name == "NotFoundError")) {
            console.error(e);
          }
        }

        try {
          data = await IOUtils.readJSON(
            this._options.backupFile,
            this._options
          );
          if (this.dataReady) {
            return;
          }
        } catch (e3) {
          if (!(DOMException.isInstance(e3) && e3.name == "NotFoundError")) {
            console.error(e3);
          }
        }
      }

      if (this.dataReady) {
        return;
      }
    }

    this._processLoadedData(data);
  },

  ensureDataReady() {
    if (this.dataReady) {
      return;
    }

    let data = {};

    try {
      let inputStream = new FileInputStream(
        new lazy.FileUtils.File(this.path),
        lazy.FileUtils.MODE_RDONLY,
        lazy.FileUtils.PERMS_FILE,
        0
      );
      try {
        let bytes = lazy.NetUtil.readInputStream(
          inputStream,
          inputStream.available()
        );
        data = JSON.parse(lazy.gTextDecoder.decode(bytes));
      } finally {
        inputStream.close();
      }
    } catch (ex) {

      if (
        !(
          ex instanceof Components.Exception &&
          ex.result == Cr.NS_ERROR_FILE_NOT_FOUND
        )
      ) {
        console.error(ex);
        try {
          let originalFile = new lazy.FileUtils.File(this.path);
          let backupFile = originalFile.clone();
          backupFile.leafName += ".corrupt";
          backupFile.createUnique(
            Ci.nsIFile.NORMAL_FILE_TYPE,
            lazy.FileUtils.PERMS_FILE
          );
          backupFile.remove(false);
          originalFile.moveTo(backupFile.parent, backupFile.leafName);
        } catch (e2) {
          console.error(e2);
        }
      }

      if (this._options.backupFile) {
        try {
          let basename = PathUtils.filename(this.path);
          let backupFile = new lazy.FileUtils.File(this._options.backupFile);
          backupFile.copyTo(null, basename);
        } catch (e) {
          if (e.result != Cr.NS_ERROR_FILE_NOT_FOUND) {
            console.error(e);
          }
        }

        try {
          let inputStream = new FileInputStream(
            new lazy.FileUtils.File(this._options.backupFile),
            lazy.FileUtils.MODE_RDONLY,
            lazy.FileUtils.PERMS_FILE,
            0
          );
          try {
            let bytes = lazy.NetUtil.readInputStream(
              inputStream,
              inputStream.available()
            );
            data = JSON.parse(lazy.gTextDecoder.decode(bytes));
          } finally {
            inputStream.close();
          }
        } catch (e3) {
          if (e3.result != Cr.NS_ERROR_FILE_NOT_FOUND) {
            console.error(e3);
          }
        }
      }
    }

    this._processLoadedData(data);
  },

  saveSoon() {
    return this._saver.arm();
  },

  async _save() {
    if (this._beforeSave) {
      await Promise.resolve(this._beforeSave());
    }

    try {
      await IOUtils.writeJSON(
        this.path,
        this._data,
        Object.assign({ tmpPath: this.path + ".tmp" }, this._options)
      );
    } catch (ex) {
      if (typeof this._data.toJSONSafe == "function") {
        await IOUtils.writeUTF8(
          this.path,
          this._data.toJSONSafe(),
          Object.assign({ tmpPath: this.path + ".tmp" }, this._options)
        );
      } else {
        try {
          this._saveFailureHandler(ex);
        } catch (saveFailureEx) {
          console.error(
            "Failed to handle ",
            ex,
            " in save failure handler due to ",
            saveFailureEx
          );
        }
      }
    }
  },

  _processLoadedData(data) {
    if (this._finalizePromise) {
      return;
    }
    this.data = this._dataPostProcessor ? this._dataPostProcessor(data) : data;
  },

  _finalizeInternal() {
    if (this._finalizePromise) {
      return this._finalizePromise;
    }
    this._finalizePromise = (async () => {
      await this._saver.finalize();
      this._data = null;
      this.dataReady = false;
    })();
    return this._finalizePromise;
  },

  async finalize() {
    if (this._finalizePromise) {
      throw new Error(`The file ${this.path} has already been finalized`);
    }
    await this._finalizeInternal();
    this._finalizeAt.removeBlocker(this._finalizeInternalBound);
  },
};
