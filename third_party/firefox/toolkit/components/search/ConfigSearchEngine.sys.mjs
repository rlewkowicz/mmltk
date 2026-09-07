/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */



import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

import {
  SearchEngine,
  EngineURL,
  QueryParameter,
} from "moz-src:///toolkit/components/search/SearchEngine.sys.mjs";

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  NimbusFeatures: "resource://nimbus/ExperimentAPI.sys.mjs",
  ObjectUtils: "resource://gre/modules/ObjectUtils.sys.mjs",
  RemoteSettings: "resource://services-settings/remote-settings.sys.mjs",
  SearchEngineClassification:
    "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustSearch.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  SearchUtils: "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  idleService: {
    service: "@mozilla.org/widget/useridleservice;1",
    iid: Ci.nsIUserIdleService,
  },
  logConsole: () =>
    console.createInstance({
      prefix: "SearchEngine",
      maxLogLevel: lazy.SearchUtils.loggingEnabled ? "Debug" : "Warn",
    }),
});

const HAS_BEEN_USED = "hasBeenUsed";

const ICON_UPDATE_ON_IDLE_DELAY = 30;

class IconHandler {
  #iconCollection = null;

  #iconMap = null;

  #queuedIdle = false;

  #pendingUpdatesMap = new Map();

  constructor() {
    this.#iconCollection = lazy.RemoteSettings("search-config-icons");
    this.#iconCollection.on("sync", this._onIconListUpdated.bind(this));
  }

  getKey(engineID) {
    return engineID.substring(0, 2);
  }

  async getAvailableRecords(engineIdentifier) {
    if (!this.#iconMap) {
      await this.#buildIconMap();
    }

    let iconList = this.#iconMap.get(this.getKey(engineIdentifier)) || [];
    return iconList.filter(r =>
      this.#identifierMatches(engineIdentifier, r.engineIdentifiers)
    );
  }

  async createIconURL(iconRecord) {
    let iconData;
    try {
      iconData = await this.#iconCollection.attachments.get(iconRecord);
    } catch (ex) {
      console.error(ex);
    }
    if (!iconData) {
      console.warn("Unable to find the attachment for", iconRecord.id);
      this.#pendingUpdatesMap.set(iconRecord.id, iconRecord);
      this.#maybeQueueIdle();
      return null;
    }

    if (iconData.record.last_modified != iconRecord.last_modified) {
      this.#pendingUpdatesMap.set(iconRecord.id, iconRecord);
      this.#maybeQueueIdle();
    }
    return URL.createObjectURL(
      new Blob([iconData.buffer], { type: iconRecord.attachment.mimetype })
    );
  }

  QueryInterface = ChromeUtils.generateQI(["nsIObserver"]);

  async observe(subject, topic) {
    if (topic != "idle") {
      return;
    }

    this.#queuedIdle = false;
    lazy.idleService.removeIdleObserver(this, ICON_UPDATE_ON_IDLE_DELAY);

    await this.#buildIconMap();

    let appProvidedEngines = await lazy.SearchService.getAppProvidedEngines();
    for (let record of this.#pendingUpdatesMap.values()) {
      let iconData;
      try {
        iconData = await this.#iconCollection.attachments.download(record);
      } catch (ex) {
        console.error("Could not download new icon", ex);
        continue;
      }

      for (let engine of appProvidedEngines) {
        if (this.#identifierMatches(engine.id, record.engineIdentifiers)) {
          await engine.maybeUpdateIconURL(
            URL.createObjectURL(
              new Blob([iconData.buffer], {
                type: record.attachment.mimetype,
              })
            ),
            record.imageSize
          );
        }
      }
    }

    this.#pendingUpdatesMap.clear();
  }

  #identifierMatches(identifier, engineIdentifiers) {
    return engineIdentifiers.some(i => {
      if (i.endsWith("*")) {
        return identifier.startsWith(i.slice(0, -1));
      }
      return identifier == i;
    });
  }

  async #buildIconMap() {
    let iconList = [];
    try {
      iconList = await this.#iconCollection.get();
    } catch (ex) {
      console.error(ex);
    }
    if (!iconList.length) {
      console.error("Failed to obtain search engine icon list records");
    }

    this.#iconMap = new Map();
    for (let record of iconList) {
      let keys = new Set(record.engineIdentifiers.map(this.getKey));
      for (let key of keys) {
        if (this.#iconMap.has(key)) {
          this.#iconMap.get(key).push(record);
        } else {
          this.#iconMap.set(key, [record]);
        }
      }
    }
  }

  async _onIconListUpdated({ data: { created, updated } }) {
    created.forEach(record => {
      this.#pendingUpdatesMap.set(record.id, record);
    });
    for (let record of updated) {
      if (record.new) {
        this.#pendingUpdatesMap.set(record.new.id, record.new);
      }
    }
    this.#maybeQueueIdle();
  }

  #maybeQueueIdle() {
    if (this.#pendingUpdatesMap && !this.#queuedIdle) {
      this.#queuedIdle = true;
      lazy.idleService.addIdleObserver(this, ICON_UPDATE_ON_IDLE_DELAY);
    }
  }
}

const ParamPreferenceCache = {
  QueryInterface: ChromeUtils.generateQI([
    "nsIObserver",
    "nsISupportsWeakReference",
  ]),

  initCache() {
    let branchFetcher = AppConstants.NIGHTLY_BUILD
      ? "getBranch"
      : "getDefaultBranch";
    this.branch = Services.prefs[branchFetcher](
      lazy.SearchUtils.BROWSER_SEARCH_PREF + "param."
    );
    this.cache = new Map();
    this.nimbusCache = new Map();
    for (let prefName of this.branch.getChildList("")) {
      this.cache.set(prefName, this.branch.getCharPref(prefName, null));
    }
    this.branch.addObserver("", this, true);

    this.onNimbusUpdate = this.onNimbusUpdate.bind(this);
    this.onNimbusUpdate();
    lazy.NimbusFeatures.searchConfiguration.onUpdate(this.onNimbusUpdate);
    lazy.NimbusFeatures.searchConfiguration.ready().then(this.onNimbusUpdate);
  },

  observe(subject, topic, data) {
    this.cache.set(data, this.branch.getCharPref(data, null));
  },

  onNimbusUpdate() {
    let extraParams =
      lazy.NimbusFeatures.searchConfiguration.getVariable("extraParams") || [];
    this.nimbusCache.clear();
    try {
      for (const { key, value } of extraParams) {
        this.nimbusCache.set(key, value);
      }
    } catch (ex) {
      console.error("Failed to load nimbus variables for extraParams:", ex);
    }
  },

  getPref(prefName) {
    if (!this.cache) {
      this.initCache();
    }
    return this.nimbusCache.has(prefName)
      ? this.nimbusCache.get(prefName)
      : this.cache.get(prefName);
  },
};

class QueryPreferenceParameter extends QueryParameter {
  constructor(name, prefName) {
    super(name, prefName);
  }

  get value() {
    const prefValue = ParamPreferenceCache.getPref(this._value);
    return prefValue ? encodeURIComponent(prefValue) : null;
  }

  toJSON() {
    lazy.logConsole.warn(
      "QueryPreferenceParameter should only exist for config engines which are never saved as JSON"
    );
    return {
      condition: "pref",
      name: this.name,
      pref: this._value,
    };
  }
}

export class ConfigSearchEngine extends SearchEngine {
  static URL_TYPE_MAP = new Map([
    ["search", lazy.SearchUtils.URL_TYPE.SEARCH],
    ["suggestions", lazy.SearchUtils.URL_TYPE.SUGGEST_JSON],
    ["trending", lazy.SearchUtils.URL_TYPE.TRENDING_JSON],
    ["searchForm", lazy.SearchUtils.URL_TYPE.SEARCH_FORM],
    ["visualSearch", lazy.SearchUtils.URL_TYPE.VISUAL_SEARCH],
  ]);
  static iconHandler = new IconHandler();

  #blobURLPromises = new Map();

  #isGeneralPurposeSearchEngine = false;

  #prevEngineInfo = null;

  #partnerCode = "";

  #telemetryId;

  #orderHint;

  constructor({ config, settings }) {
    if (new.target === ConfigSearchEngine) {
      throw new Error("Cannot instanciate abstract class ConfigSearchEngine");
    }
    super({
      loadPath: "[app]" + config.identifier,
      id: config.identifier,
    });

    this.#init(config);
    this._loadSettings(settings);

    this.#prevEngineInfo = new Map(
       ([
        ["name", this.name],
        ["_loadPath", this._loadPath],
        ["submissionURL", this.getSubmission("foo").uri.spec],
        ["aliases", this._definedAliases],
      ])
    );
  }

  async cleanup() {
    for (let [size, blobURLPromise] of this.#blobURLPromises) {
      URL.revokeObjectURL(await blobURLPromise);
      this.#blobURLPromises.delete(size);
    }
  }

  update({ configuration }) {
    this._urls = [];
    this.#init(configuration);

    let needToSendUpdate = this.#hasBeenModified(this, this.#prevEngineInfo, [
      "name",
      "_loadPath",
      "aliases",
    ]);

    if (needToSendUpdate) {
      lazy.SearchUtils.notifyAction(
        this,
        lazy.SearchUtils.MODIFIED_TYPE.CHANGED
      );

      this._resetPrevEngineInfo();
    }
  }

  get inMemory() {
    return true;
  }

  get isGeneralPurposeEngine() {
    return this.#isGeneralPurposeSearchEngine;
  }

  get partnerCode() {
    return this.#partnerCode;
  }

  get telemetryId() {
    if (this.getAttr("overriddenBy")) {
      return this.#telemetryId + "-addon";
    }
    return this.#telemetryId;
  }

  get orderHint() {
    return this.#orderHint;
  }

  async getIconURL(preferredWidth) {
    preferredWidth ||= 16;

    let availableRecords =
      await ConfigSearchEngine.iconHandler.getAvailableRecords(this.id);
    if (!availableRecords.length) {
      console.warn("No icon found for", this.id);
      return null;
    }

    let availableSizes = availableRecords.map(r => r.imageSize);
    let width = lazy.SearchUtils.chooseIconSize(preferredWidth, availableSizes);

    if (this.#blobURLPromises.has(width)) {
      return this.#blobURLPromises.get(width);
    }

    let record = availableRecords.find(r => r.imageSize == width);
    let promise = ConfigSearchEngine.iconHandler.createIconURL(record);
    this.#blobURLPromises.set(width, promise);
    return promise;
  }

  async maybeUpdateIconURL(blobURL, size) {
    if (this.#blobURLPromises.has(size)) {
      URL.revokeObjectURL(await this.#blobURLPromises.get(size));
    }
    this.#blobURLPromises.set(size, Promise.resolve(blobURL));

    lazy.SearchUtils.notifyAction(
      this,
      lazy.SearchUtils.MODIFIED_TYPE.ICON_CHANGED
    );
  }

  markAsUsed() {
    this.setAttr(HAS_BEEN_USED, true, true);
  }

  get hasBeenUsed() {
    return this.getAttr(HAS_BEEN_USED) ?? false;
  }

  clearUsage() {
    this.clearAttr(HAS_BEEN_USED);
  }

  toJSON() {
    return {
      id: this.id,
      _name: this.name,
      _isConfigEngine: true,
      _metaData: this._metaData,
    };
  }

  #init(engineConfig) {
    this.#orderHint = engineConfig.orderHint;
    this.#telemetryId = engineConfig.identifier;
    this.#isGeneralPurposeSearchEngine =
      engineConfig.classification == lazy.SearchEngineClassification.GENERAL;

    if (engineConfig.charset) {
      this._queryCharset = engineConfig.charset;
    }

    if (engineConfig.telemetrySuffix) {
      this.#telemetryId += `-${engineConfig.telemetrySuffix}`;
    }

    if (engineConfig.clickUrl) {
      this.clickUrl = engineConfig.clickUrl;
    }

    this._name = engineConfig.name.trim();
    this._definedAliases =
      engineConfig.aliases?.map(alias => `@${alias}`) ?? [];
    this.#partnerCode = engineConfig.partnerCode ?? "";
    this.isNewUntil = engineConfig.isNewUntil ?? "";

    for (const [type, urlData] of Object.entries(engineConfig.urls)) {
      if (urlData) {
        this.#setUrl(type, urlData, engineConfig.partnerCode);
      }
    }
  }

  #setUrl(type, urlData, partnerCode) {
    let urlType = ConfigSearchEngine.URL_TYPE_MAP.get(type);
    if (!urlType) {
      console.warn("unexpected engine url type.", type);
      return;
    }

    let engineURL = new EngineURL({
      ...urlData,
      type: urlType,
      template: urlData.base,
    });

    if (urlData.params) {
      for (const param of urlData.params) {
        if (param.value != undefined) {
          engineURL.addParam(
            param.name,
            param.value == "{partnerCode}" ? partnerCode : param.value
          );
        } else if (param.experimentConfig != undefined) {
          engineURL.addQueryParameter(
            new QueryPreferenceParameter(param.name, param.experimentConfig)
          );
        }
      }
    }

    if (urlData.searchTermParamName) {
      engineURL.setSearchTermParamName(urlData.searchTermParamName);
    } else if (
      !urlData.base.includes("{searchTerms}") &&
      (urlType == lazy.SearchUtils.URL_TYPE.SEARCH ||
        urlType == lazy.SearchUtils.URL_TYPE.SUGGEST_JSON)
    ) {
      throw new Error("Search terms missing from engine URL.");
    }

    this._urls.push(engineURL);
  }

  #hasBeenModified(currentEngine, initialValues, targetKeys) {
    for (let i = 0; i < targetKeys.length; i++) {
      let key = targetKeys[i];

      if (
        !lazy.ObjectUtils.deepEqual(currentEngine[key], initialValues.get(key))
      ) {
        return true;
      }

      let currentEngineSubmissionURL =
        currentEngine.getSubmission("foo").uri.spec;
      if (currentEngineSubmissionURL != initialValues.get("submissionURL")) {
        return true;
      }
    }

    return false;
  }

  _resetPrevEngineInfo() {
    this.#prevEngineInfo.forEach((_value, key) => {
      let newValue;
      if (key == "submissionURL") {
        newValue = this.getSubmission("foo").uri.spec;
      } else {
        newValue = this[key];
      }

      this.#prevEngineInfo.set(key, newValue);
    });
  }
}

export class AppProvidedConfigEngine extends ConfigSearchEngine {
  downgrade() {
    if (!this.getAttr("user-installed")) {
      throw new Error("Cannot downgrade without user-installed attribute.");
    }
    Object.setPrototypeOf(this, UserInstalledConfigEngine.prototype);
  }
}

export class UserInstalledConfigEngine extends ConfigSearchEngine {
  constructor(options) {
    super(options);
    this.setAttr("user-installed", true);
  }

  upgrade() {
    Object.setPrototypeOf(this, AppProvidedConfigEngine.prototype);
  }
}
