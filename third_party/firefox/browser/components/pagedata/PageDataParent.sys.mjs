/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PageDataService:
    "moz-src:///browser/components/pagedata/PageDataService.sys.mjs",
});

export class PageDataParent extends JSWindowActorParent {
  #deferredCollection = null;

  collectPageData() {
    if (!this.#deferredCollection) {
      this.#deferredCollection = Promise.withResolvers();
      this.sendQuery("PageData:Collect").then(
        this.#deferredCollection.resolve,
        this.#deferredCollection.reject
      );
    }

    return this.#deferredCollection.promise;
  }

  didDestroy() {
    this.#deferredCollection?.resolve(null);
  }

  receiveMessage(msg) {
    switch (msg.name) {
      case "PageData:DocumentReady":
        lazy.PageDataService.pageLoaded(this, msg.data.url);
        break;
    }
  }
}
