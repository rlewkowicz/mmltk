/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PageDataSchema:
    "moz-src:///browser/components/pagedata/PageDataSchema.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "READY_DELAY",
  "browser.pagedata.readyDelay",
  500
);

export class PageDataChild extends JSWindowActorChild {
  #isContentWindowPrivate = true;
  #deferTimer = null;

  actorCreated() {
    this.#isContentWindowPrivate =
      lazy.PrivateBrowsingUtils.isContentWindowPrivate(this.contentWindow);
  }

  didDestroy() {
    if (this.#deferTimer) {
      this.#deferTimer.cancel();
    }
  }

  #deferReady() {
    if (!this.#deferTimer) {
      this.#deferTimer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
    }

    this.#deferTimer.initWithCallback(
      () => {
        this.#deferTimer = null;
        this.sendAsyncMessage("PageData:DocumentReady", {
          url: this.document.documentURI,
        });
      },
      lazy.READY_DELAY,
      Ci.nsITimer.TYPE_ONE_SHOT_LOW_PRIORITY
    );
  }

  receiveMessage(msg) {
    if (this.#isContentWindowPrivate) {
      return undefined;
    }

    switch (msg.name) {
      case "PageData:CheckLoaded":
        if (this.document.readystate == "complete") {
          this.#deferReady();
        }
        break;
      case "PageData:Collect":
        return lazy.PageDataSchema.collectPageData(this.document);
    }

    return undefined;
  }

  handleEvent(event) {
    if (this.#isContentWindowPrivate) {
      return;
    }

    switch (event.type) {
      case "DOMContentLoaded":
      case "pageshow":
        this.#deferReady();
        break;
    }
  }
}
