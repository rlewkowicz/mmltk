/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */



import {
  EngineURL,
  SearchEngine,
} from "moz-src:///toolkit/components/search/SearchEngine.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  loadAndParseOpenSearchEngine:
    "moz-src:///toolkit/components/search/OpenSearchLoader.sys.mjs",
  SearchUtils: "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  logConsole: () =>
    console.createInstance({
      prefix: "OpenSearchEngine",
      maxLogLevel: lazy.SearchUtils.loggingEnabled ? "Debug" : "Warn",
    }),
});

const OPENSEARCH_DEFAULT_UPDATE_INTERVAL = 7;

export class OpenSearchEngine extends SearchEngine {
  _data = null;
  _updateInterval = null;
  _updateURL = null;

  constructor(options = {}) {
    super({
      loadPath:
        options.json?._loadPath ??
        OpenSearchEngine.getAnonymizedLoadPath(
          lazy.SearchUtils.sanitizeName(options.engineData.name),
          options.engineData.installURL
        ),
    });

    if (options.faviconURL) {
      this._setIcon(options.faviconURL, {
        override: false,
        originAttributes: options.originAttributes,
      }).catch(e =>
        lazy.logConsole.error(
          `Error while setting icon for search engine ${options.engineData.name}:`,
          e.message
        )
      );
    }

    if (options.engineData) {
      this.#setEngineData(options.engineData, options.originAttributes);

      this.setAttr(
        "loadPathHash",
        lazy.SearchUtils.getVerificationHash(this._loadPath)
      );

      if (this.hasUpdates) {
        this.#setNextUpdateTime();
      }
    } else {
      this._initWithJSON(options.json);
      this._updateInterval = options.json._updateInterval ?? null;
      this._updateURL = options.json._updateURL ?? null;
    }
  }

  toJSON() {
    let json = super.toJSON();
    json._updateInterval = this._updateInterval;
    json._updateURL = this._updateURL;
    return json;
  }

  get hasUpdates() {
    let selfURL = this.getURLOfType(
      lazy.SearchUtils.URL_TYPE.OPENSEARCH,
      "self"
    );
    return !!(this._updateURL || selfURL);
  }

  get updateURI() {
    let updateURL = this.getURLOfType(lazy.SearchUtils.URL_TYPE.OPENSEARCH);
    let updateURI =
      updateURL && updateURL._hasRelation("self")
        ? updateURL.getSubmission("", this.queryCharset).uri
        : lazy.SearchUtils.makeURI(this._updateURL);
    return updateURI;
  }

  async maybeUpdate() {
    if (!this.hasUpdates) {
      return;
    }

    let currentTime = Date.now();

    let expireTime = this.getAttr("updateexpir");

    if (!expireTime || !(expireTime <= currentTime)) {
      lazy.logConsole.debug(this.name, "Skipping update, not expired yet.");
      return;
    }

    await this.#update();

    this.#setNextUpdateTime();
  }

  async #update() {
    let updateURI = this.updateURI;
    if (updateURI) {
      let data = await lazy.loadAndParseOpenSearchEngine(
        updateURI,
        this.getAttr("updatelastmodified")
      );

      this.#setEngineData(data);

      lazy.SearchUtils.notifyAction(
        this,
        lazy.SearchUtils.MODIFIED_TYPE.CHANGED
      );

      this.setAttr("updatelastmodified", new Date().toUTCString());
    }
  }

  #setEngineData(data, originAttributes) {
    let name = data.name.trim();

    this._name = name;
    this._queryCharset = data.queryCharset ?? "UTF-8";
    if (data.searchForm) {
      try {
        let searchFormUrl = new EngineURL({
          type: lazy.SearchUtils.URL_TYPE.SEARCH_FORM,
          template: data.searchForm,
        });
        this._urls.push(searchFormUrl);
      } catch (ex) {
        throw new Error(
          `Failed to add ${data.searchForm} as a searchForm URL`,
          { cause: ex }
        );
      }
    }

    for (let url of data.urls) {
      if (url.rels.includes("searchform")) {
        let searchFormURL;
        try {
          searchFormURL = new EngineURL({
            type: lazy.SearchUtils.URL_TYPE.SEARCH_FORM,
            template: url.template,
          });
        } catch (ex) {
          throw new Error(`Failed to add ${url.template} as an Engine URL`, {
            cause: ex,
          });
        }
        this.#addParamsToUrl(searchFormURL, url.params);
        this._urls.push(searchFormURL);
      }

      let engineURL;
      try {
        engineURL = new EngineURL(url);
      } catch (ex) {
        throw new Error(`Failed to add ${url.template} as an Engine URL`, {
          cause: ex,
        });
      }

      let nonSearchformRels = url.rels.filter(rel => rel != "searchform");
      if (nonSearchformRels.length) {
        engineURL.rels = nonSearchformRels;
      }

      this.#addParamsToUrl(engineURL, url.params);
      this._urls.push(engineURL);
    }

    for (let image of data.images) {
      this._setIcon(image.url, { size: image.size, originAttributes }).catch(
        e =>
          lazy.logConsole.error(
            `Error while setting icon for search engine ${data.name}:`,
            e.message
          )
      );
    }
  }

  #addParamsToUrl(engineURL, params) {
    for (let param of params) {
      try {
        engineURL.addParam(param.name, param.value);
      } catch (ex) {
        lazy.logConsole.error("OpenSearch url has an invalid param", param);
      }
    }
  }

  #setNextUpdateTime() {
    var interval = this._updateInterval || OPENSEARCH_DEFAULT_UPDATE_INTERVAL;
    var milliseconds = interval * 86400000; 
    this.setAttr("updateexpir", Date.now() + milliseconds);
  }

  static getAnonymizedLoadPath(sanitizedName, uri) {
    return `[${uri.scheme}]${uri.host}/${sanitizedName}.xml`;
  }
}
