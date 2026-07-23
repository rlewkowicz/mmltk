/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import {
  SearchEngine,
  EngineURL,
} from "moz-src:///toolkit/components/search/SearchEngine.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  SearchUtils: "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
});


export class UserSearchEngine extends SearchEngine {
  constructor(options = {}) {
    super({
      loadPath: "[user]",
    });

    if (options.formInfo) {
      this.#initWithFormInfo(options.formInfo);
    } else {
      this._initWithJSON(options.json);
    }
  }

  #initWithFormInfo(formInfo) {
    this._name = formInfo.name.trim();
    let charset = formInfo.charset ?? lazy.SearchUtils.DEFAULT_QUERY_CHARSET;

    let url = new EngineURL({
      type: lazy.SearchUtils.URL_TYPE.SEARCH,
      method: formInfo.method ?? "GET",
      template: formInfo.url,
    });
    for (let [key, value] of formInfo.params ?? []) {
      url.addParam(
        Services.textToSubURI.ConvertAndEscape(charset, key),
        Services.textToSubURI
          .ConvertAndEscape(charset, value)
          .replaceAll("%7BsearchTerms%7D", "{searchTerms}")
      );
    }
    this._urls.push(url);

    if (formInfo.suggestUrl) {
      let suggestUrl = new EngineURL({
        type: lazy.SearchUtils.URL_TYPE.SUGGEST_JSON,
        template: formInfo.suggestUrl,
      });
      this._urls.push(suggestUrl);
    }

    if (formInfo.charset) {
      this._queryCharset = formInfo.charset;
    }

    this.alias = formInfo.alias;
    this.updateFavicon();
  }

  get telemetryId() {
    return `other-${this.name}`;
  }

  rename(newName) {
    if (newName == this.name) {
      return true;
    } else if (lazy.SearchService.getEngineByName(newName)) {
      return false;
    }
    this._name = newName;
    lazy.SearchUtils.notifyAction(this, lazy.SearchUtils.MODIFIED_TYPE.CHANGED);
    return true;
  }

  changeUrl(type, template, postData) {
    if (type == lazy.SearchUtils.URL_TYPE.SEARCH && !template) {
      throw new Error("Cannot remove search URL.");
    }

    this._urls = this._urls.filter(url => url.type != type);

    if (template) {
      let method = postData ? "POST" : "GET";
      let url = new EngineURL({ type, method, template });
      for (let [key, value] of new URLSearchParams(postData ?? "").entries()) {
        url.addParam(key, value);
      }
      this._urls.push(url);
    }

    lazy.SearchUtils.notifyAction(this, lazy.SearchUtils.MODIFIED_TYPE.CHANGED);
  }

  async changeIcon(newIconURL) {
    let [iconURL, size] = await this._downloadAndRescaleIcon(newIconURL);

    this._iconMapObj = {};
    this._addIconToMap(iconURL, size);
    lazy.SearchUtils.notifyAction(
      this,
      lazy.SearchUtils.MODIFIED_TYPE.ICON_CHANGED
    );
  }

  updateFavicon() {
    let searchUrl = this.getURLOfType(lazy.SearchUtils.URL_TYPE.SEARCH);
    let searchUrlOrigin = new URL(searchUrl.template).origin;

    lazy.PlacesUtils.favicons
      .getFaviconForPage(Services.io.newURI(searchUrlOrigin))
      .then(iconURL => {
        if (iconURL) {
          this.changeIcon(iconURL.dataURI.spec);
        } else if (Object.keys(this._iconMapObj).length) {
          this._iconMapObj = {};
          lazy.SearchUtils.notifyAction(
            this,
            lazy.SearchUtils.MODIFIED_TYPE.ICON_CHANGED
          );
        }
      })
      .catch(e => {
        console.warn(
          `Unable to change icon of engine ${this.name}:`,
          e.message
        );
      });
  }
}
