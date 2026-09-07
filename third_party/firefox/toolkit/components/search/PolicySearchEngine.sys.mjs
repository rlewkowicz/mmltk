/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { SearchEngine } from "moz-src:///toolkit/components/search/SearchEngine.sys.mjs";

export class PolicySearchEngine extends SearchEngine {
  constructor(options) {
    let id = "policy-" + options.details.Name;

    super({
      loadPath: "[policy]",
      id,
    });

    let details = {
      iconURL: options.details.IconURL ? options.details.IconURL.href : null,
      name: options.details.Name,
      encoding: options.details.Encoding,
      search_url: encodeURI(options.details.URLTemplate),
      keyword: options.details.Alias,
      search_url_post_params:
        options.details.Method == "POST" ? options.details.PostData : undefined,
      suggest_url: options.details.SuggestURLTemplate,
    };
    this._initWithDetails(details);

    this._loadSettings(options.settings);
  }

  get inMemory() {
    return true;
  }

  get telemetryId() {
    return `other-${this.name}`;
  }

  toJSON() {
    let json = super.toJSON();

    return {
      id: json.id,
      _name: json._name,
      _loadPath: json._loadPath,
      _metaData: json._metaData,
    };
  }
}
