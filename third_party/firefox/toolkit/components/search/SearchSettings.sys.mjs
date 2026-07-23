/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  AppProvidedConfigEngine:
    "moz-src:///toolkit/components/search/ConfigSearchEngine.sys.mjs",
  ConfigSearchEngine:
    "moz-src:///toolkit/components/search/ConfigSearchEngine.sys.mjs",
  DeferredTask: "resource://gre/modules/DeferredTask.sys.mjs",
  ObjectUtils: "resource://gre/modules/ObjectUtils.sys.mjs",
  SearchUtils: "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  logConsole: () =>
    console.createInstance({
      prefix: "SearchSettings",
      maxLogLevel: lazy.SearchUtils.loggingEnabled ? "Debug" : "Warn",
    }),
});


const SETTINGS_FILENAME = "search.json.mozlz4";

const ENGINE_ID_TO_OLD_NAME_MAP = new Map([
  ["wikipedia-hy", "Wikipedia (hy)"],
  ["wikipedia-kn", "Wikipedia (kn)"],
  ["wikipedia-lv", "Vikipēdija"],
  ["wikipedia-NO", "Wikipedia (no)"],
  ["wikipedia-el", "Wikipedia (el)"],
  ["wikipedia-lt", "Wikipedia (lt)"],
  ["wikipedia-my", "Wikipedia (my)"],
  ["wikipedia-pa", "Wikipedia (pa)"],
  ["wikipedia-pt", "Wikipedia (pt)"],
  ["wikipedia-si", "Wikipedia (si)"],
  ["wikipedia-tr", "Wikipedia (tr)"],
]);

export class SearchSettings {
  constructor(searchService) {
    this.#searchService = searchService;

    searchService.promiseInitialized.then(() => {
      this._delayedWrite();
    });
  }

  QueryInterface = ChromeUtils.generateQI([Ci.nsIObserver]);

  static SETTINGS_INVALIDATION_DELAY = 1000;

  get #settingsFilePath() {
    return PathUtils.join(PathUtils.profileDir, SETTINGS_FILENAME);
  }

  _batchTask = null;

  #searchService = null;

  #settings = null;

  #cachedSettings = {};

  addObservers() {
    Services.obs.addObserver(this, lazy.SearchUtils.TOPIC_ENGINE_MODIFIED);
    Services.obs.addObserver(this, lazy.SearchUtils.TOPIC_SEARCH_SERVICE);
  }

  removeObservers() {
    Services.obs.removeObserver(this, lazy.SearchUtils.TOPIC_ENGINE_MODIFIED);
    Services.obs.removeObserver(this, lazy.SearchUtils.TOPIC_SEARCH_SERVICE);
  }

  lastGetCorrupt = false;

  async get(origin = "") {
    this.lastGetCorrupt = false;

    let json;
    await this._ensurePendingWritesCompleted(origin);
    try {
      json = await IOUtils.readJSON(this.#settingsFilePath, {
        decompress: true,
      });
      if (!json.engines || !json.engines.length) {
        throw new Error("no engine in the file");
      }
    } catch (ex) {
      if (DOMException.isInstance(ex) && ex.name === "NotFoundError") {
        lazy.logConsole.debug("get: No settings file exists, new profile?", ex);
        return this.#resetSettings(false);
      }
      lazy.logConsole.error("get: Settings file empty or corrupt.", ex);
      return this.#resetSettings(true);
    }

    this.#settings = json;
    this.#cachedSettings = structuredClone(json);

    if (!this.#settings.metaData) {
      this.#settings.metaData = {};
    }

    try {
      await this.#migrateSettings();
    } catch (ex) {
      lazy.logConsole.error("get: Migration failed.", ex);
      return this.#resetSettings(true);
    }

    return structuredClone(json);
  }

  async #resetSettings(corrupt) {
    this.#settings = { metaData: {} };
    this.#cachedSettings = {};

    if (corrupt) {
      this.lastGetCorrupt = true;
      Services.prefs.setIntPref(
        lazy.SearchUtils.BROWSER_SEARCH_PREF + "lastSettingsCorruptTime",
        Date.now() / 1000
      );
      try {
        await IOUtils.move(
          this.#settingsFilePath,
          this.#settingsFilePath + ".bak"
        );
      } catch (ex) {
        lazy.logConsole.warn(
          "#resetSettings: Unable to create backup of corrupt settings file.",
          ex
        );
      }
    }

    return structuredClone(this.#settings);
  }

  _testResetSettings() {
    this.#resetSettings(false);
  }

  _delayedWrite() {
    if (this._batchTask) {
      this._batchTask.disarm();
    } else {
      let task = async () => {
        if (
          !this.#searchService.isInitialized ||
          this.#searchService._reloadingEngines
        ) {
          this._batchTask.arm();
          return;
        }
        lazy.logConsole.debug("batchTask: Invalidating engine settings");
        await this._write();
      };
      this._batchTask = new lazy.DeferredTask(
        task,
        SearchSettings.SETTINGS_INVALIDATION_DELAY
      );
    }
    this._batchTask.arm();
  }

  async _ensurePendingWritesCompleted(origin = "") {
    if (!this._batchTask) {
      return;
    }
    lazy.logConsole.debug("finalizing batch task");
    let task = this._batchTask;
    this._batchTask = null;
    if (origin == "test") {
      task.disarm();
    } else {
      await task.finalize();
    }
  }

  async _write() {
    if (this._batchTask) {
      this._batchTask.disarm();
    }

    let settings = {};

    settings.version = lazy.SearchUtils.SETTINGS_VERSION;
    settings.engines = [...this.#searchService._engines.values()].map(engine =>
      JSON.parse(JSON.stringify(engine))
    );
    settings.metaData = this.#settings.metaData;

    if (this.#settings?.engines) {
      for (let engine of this.#settings.engines) {
        if (
          engine._isConfigEngine &&
          !engine._metaData["user-installed"]
        ) {
          let included = settings.engines.some(
            e => e.id == engine.id || e._name == engine._name
          );
          if (!included) {
            settings.engines.push(engine);
          }
        }
      }
    }

    this.#settings = settings;

    try {
      if (!settings.engines.length) {
        throw new Error("cannot write without any engine.");
      }

      if (this.isCurrentAndCachedSettingsEqual()) {
        lazy.logConsole.debug(
          "_write: Settings unchanged. Did not write to disk."
        );
        Services.obs.notifyObservers(
          null,
          lazy.SearchUtils.TOPIC_SEARCH_SERVICE,
          "write-prevented-when-settings-unchanged"
        );
        Services.obs.notifyObservers(
          null,
          lazy.SearchUtils.TOPIC_SEARCH_SERVICE,
          "write-settings-to-disk-complete"
        );

        return;
      }

      this.#cachedSettings = structuredClone(this.#settings);

      lazy.logConsole.debug("_write: Writing to settings file.");
      await IOUtils.writeJSON(this.#settingsFilePath, settings, {
        compress: true,
        tmpPath: this.#settingsFilePath + ".tmp",
      });
      lazy.logConsole.debug("_write: settings file written to disk.");
      Services.obs.notifyObservers(
        null,
        lazy.SearchUtils.TOPIC_SEARCH_SERVICE,
        "write-settings-to-disk-complete"
      );
    } catch (ex) {
      lazy.logConsole.error("_write: Could not write to settings file:", ex);
    }
  }

  setMetaDataAttribute(name, val) {
    this.#settings.metaData[name] = val;
    this._delayedWrite();
  }

  setVerifiedMetaDataAttribute(name, val) {
    this.#settings.metaData[name] = val;
    this.#settings.metaData[this.getHashName(name)] =
      lazy.SearchUtils.getVerificationHash(val);
    this._delayedWrite();
  }

  getMetaDataAttribute(name) {
    return this.#settings.metaData[name] ?? undefined;
  }

  getSettingsMetaData() {
    return { ...this.#settings.metaData };
  }

  getVerifiedMetaDataAttribute(name, isConfigEngine) {
    let attribute = this.getMetaDataAttribute(name);

    if (isConfigEngine) {
      return attribute;
    }

    if (
      attribute &&
      this.getMetaDataAttribute(this.getHashName(name)) !=
        lazy.SearchUtils.getVerificationHash(attribute)
    ) {
      lazy.logConsole.warn(
        "getVerifiedMetaDataAttribute, invalid hash for",
        name
      );
      return undefined;
    }
    return attribute;
  }

  setEngineMetaDataAttribute(engineName, property, value) {
    let engines = [...this.#searchService._engines.values()];
    let engine = engines.find(e => e._name == engineName);
    if (engine) {
      engine._metaData[property] = value;
      this._delayedWrite();
    }
  }

  getEngineMetaDataAttribute(engineName, property) {
    let engine = this.#settings.engines.find(e => e._name == engineName);
    return engine._metaData[property] ?? undefined;
  }

  getHashName(name) {
    if (name == "current") {
      return "hash";
    }
    return name + "Hash";
  }

  async shutdown(state) {
    if (!this._batchTask) {
      return;
    }
    state.step = "Finalizing batched task";
    try {
      await this._batchTask.finalize();
      state.step = "Batched task finalized";
    } catch (ex) {
      state.step = "Batched task failed to finalize";

      state.latestError.message = "" + ex;
      if (ex && typeof ex == "object") {
        state.latestError.stack = ex.stack || undefined;
      }
    }
  }

  observe(subject, topic, verb) {
    switch (topic) {
      case lazy.SearchUtils.TOPIC_ENGINE_MODIFIED:
        switch (verb) {
          case lazy.SearchUtils.MODIFIED_TYPE.ADDED:
          case lazy.SearchUtils.MODIFIED_TYPE.CHANGED:
          case lazy.SearchUtils.MODIFIED_TYPE.REMOVED:
            this._delayedWrite();
            break;
          case lazy.SearchUtils.MODIFIED_TYPE.ICON_CHANGED:
            if (!(subject.wrappedJSObject instanceof lazy.ConfigSearchEngine)) {
              this._delayedWrite();
            }
            break;
        }
        break;
      case lazy.SearchUtils.TOPIC_SEARCH_SERVICE:
        switch (verb) {
          case "engines-reloaded":
            this._delayedWrite();
            break;
        }
        break;
    }
  }

  isCurrentAndCachedSettingsEqual() {
    return lazy.ObjectUtils.deepEqual(this.#settings, this.#cachedSettings);
  }

  migrateEngineIds(clonedSettings) {
    if (clonedSettings.version <= 6) {
      lazy.logConsole.debug("migrateEngineIds: start");

      for (let engineSettings of clonedSettings.engines) {
        let engine = this.#getEngineByName(engineSettings._name);

        if (engine) {
          engineSettings.id = engine.id;
        }
      }

      let currentDefaultEngine = this.#getEngineByName(
        clonedSettings.metaData.current
      );
      let privateDefaultEngine = this.#getEngineByName(
        clonedSettings.metaData.private
      );

      if (
        currentDefaultEngine &&
        (currentDefaultEngine instanceof lazy.AppProvidedConfigEngine ||
          lazy.SearchUtils.getVerificationHash(
            clonedSettings.metaData.current
          ) == clonedSettings.metaData[this.getHashName("current")])
      ) {
        this.setVerifiedMetaDataAttribute(
          "defaultEngineId",
          currentDefaultEngine.id
        );
      } else {
        this.setVerifiedMetaDataAttribute("defaultEngineId", "");
      }

      if (
        privateDefaultEngine &&
        (privateDefaultEngine instanceof lazy.AppProvidedConfigEngine ||
          lazy.SearchUtils.getVerificationHash(
            clonedSettings.metaData.private
          ) == clonedSettings.metaData[this.getHashName("private")])
      ) {
        this.setVerifiedMetaDataAttribute(
          "privateDefaultEngineId",
          privateDefaultEngine.id
        );
      } else {
        this.setVerifiedMetaDataAttribute("privateDefaultEngineId", "");
      }

      lazy.logConsole.debug("migrateEngineIds: done");
    }
  }

  static findSettingsForEngine(settings, engineId, engineName) {
    if (settings.version <= 6) {
      let engineSettings = settings.engines?.find(e => e._name == engineName);
      if (!engineSettings) {
        let oldEngineName = ENGINE_ID_TO_OLD_NAME_MAP.get(engineId);
        if (oldEngineName) {
          engineSettings = settings.engines?.find(
            e => e._name == oldEngineName
          );
        }
      }
      return engineSettings;
    }
    return settings.engines?.find(e => e.id == engineId);
  }

  #getEngineByName(engineName) {
    for (let engine of this.#searchService._engines.values()) {
      if (engine.name == engineName) {
        return engine;
      }
    }

    return null;
  }

  async #migrateSettings() {
    this.#migrateTo6();
    this.#migrateTo9();
    this.#migrateTo10();
    this.#migrateTo11();
    await this.#migrateTo12();
    this.#migrateTo13();
  }

  #migrateTo6() {
    if (
      this.#settings.version < 6 ||
      !("useSavedOrder" in this.#settings.metaData)
    ) {
      const prefName = lazy.SearchUtils.BROWSER_SEARCH_PREF + "useDBForOrder";
      let useSavedOrder = Services.prefs.getBoolPref(prefName, false);

      this.setMetaDataAttribute("useSavedOrder", useSavedOrder);

      Services.prefs.clearUserPref(prefName);
    }
  }

  #migrateTo9() {
    if (this.#settings.version < 9 && this.#settings.engines) {
      const hiddenOneOffsPrefs = Services.prefs.getStringPref(
        "browser.search.hiddenOneOffs",
        ""
      );
      for (const engine of this.#settings.engines) {
        engine._metaData.hideOneOffButton = hiddenOneOffsPrefs.includes(
          engine._name
        );
      }
      Services.prefs.clearUserPref("browser.search.hiddenOneOffs");
    }
  }

  #migrateTo10() {
    if (
      this.#settings.version > 6 &&
      this.#settings.version < 10 &&
      this.#settings.engines
    ) {
      let changedEngines = new Map();
      for (let engine of this.#settings.engines) {
        if (engine._isAppProvided && engine.id) {
          let oldId = engine.id;
          engine.id = engine.id
            .replace("@search.mozilla.orgdefault", "")
            .replace("@search.mozilla.org", "-");
          changedEngines.set(oldId, engine.id);
        }
      }

      const PROPERTIES_CONTAINING_IDS = [
        "privateDefaultEngineId",
        "appDefaultEngineId",
        "defaultEngineId",
      ];

      for (let prop of PROPERTIES_CONTAINING_IDS) {
        if (changedEngines.has(this.#settings.metaData[prop])) {
          this.#settings.metaData[prop] = changedEngines.get(
            this.#settings.metaData[prop]
          );
        }
      }
    }
  }

  #migrateTo11() {
    if (this.#settings.version < 11 && this.#settings.engines) {
      for (let engine of this.#settings.engines) {
        if (!engine._iconMapObj) {
          continue;
        }
        let oldIconMap = engine._iconMapObj;
        engine._iconMapObj = {};

        for (let [sizeStr, icon] of Object.entries(oldIconMap)) {
          let sizeObj;
          try {
            sizeObj = JSON.parse(sizeStr);
          } catch {}
          if (
            typeof sizeObj === "object" &&
            "width" in sizeObj &&
            parseInt(sizeObj.width) > 0 &&
            sizeObj.width == sizeObj.height
          ) {
            engine._iconMapObj[sizeObj.width] = icon;
          } else if (typeof sizeObj === "number") {
            engine._iconMapObj[sizeObj] = icon;
          }
        }
      }
    }
  }

  async #migrateTo12() {
    if (this.#settings.version < 12 && this.#settings.engines) {
      for (let engine of this.#settings.engines) {
        if (engine._iconURL) {
          let iconURL = engine._iconURL;
          delete engine._iconURL;

          let uri = lazy.SearchUtils.makeURI(iconURL);
          if (!uri) {
            continue;
          }

          switch (uri.scheme) {
            case "data":
              break;
            default:
              continue;
          }

          let byteArray, contentType;
          try {
            [byteArray, contentType] = await lazy.SearchUtils.fetchIcon(uri);
          } catch {
            lazy.logConsole.warn(
              `_iconURL migration: failed to load icon of search engine ${engine._name}.`
            );
            engine._iconMapObj ||= {};
            engine._iconMapObj[16] = iconURL;
            continue;
          }

          if (byteArray.length > lazy.SearchUtils.MAX_ICON_SIZE) {
            try {
              [byteArray, contentType] = lazy.SearchUtils.rescaleIcon(
                byteArray,
                contentType
              );
              let url =
                "data:" + contentType + ";base64," + byteArray.toBase64();

              engine._iconMapObj ||= {};
              engine._iconMapObj[32] = url;
            } catch {
              lazy.logConsole.warn(
                `_iconURL migration: failed to resize icon of search engine ${engine._name}.`
              );
            }
            continue;
          }

          let size = lazy.SearchUtils.decodeSize(byteArray, contentType, 16);
          engine._iconMapObj ||= {};
          engine._iconMapObj[size] = iconURL;
        }
      }
    }
  }

  #migrateTo13() {
    if (this.#settings.version < 13 && this.#settings.engines) {
      for (let engine of this.#settings.engines) {
        if (engine._isAppProvided) {
          delete engine._isAppProvided;
          engine._isConfigEngine = true;
        } else if (engine._isBuiltin) {
          delete engine._isBuiltin;
          engine._isConfigEngine = true;
        }
      }
    }
  }

}
