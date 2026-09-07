/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { SuggestBackend } from "moz-src:///browser/components/urlbar/private/SuggestFeature.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  MerinoClient: "moz-src:///browser/components/urlbar/MerinoClient.sys.mjs",
});


export class SuggestBackendMerino extends SuggestBackend {
  get enablingPreferences() {
    return ["quickSuggestOnlineAvailable", "quicksuggest.online.enabled"];
  }

  get client() {
    return this.#client;
  }

  async enable(enabled) {
    if (!enabled) {
      this.#client = null;
    }
  }

  async query(searchString, { queryContext }) {
    if (!queryContext.allowRemoteResults()) {
      return [];
    }

    this.logger.debug("Handling query", { searchString });

    if (!this.#client) {
      this.#client = new lazy.MerinoClient(this.name, { allowOhttp: true });
    }

    let suggestions = await this.#client.fetch({
      query: searchString,
    });

    this.logger.debug("Got suggestions", suggestions);

    return suggestions;
  }

  cancelQuery() {
    this.#client?.cancelTimeoutTimer();

  }

  onSearchSessionEnd(_queryContext, _controller, _details) {
    this.#client?.resetSession();
  }

  #client = null;
}
