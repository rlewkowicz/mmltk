/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  cleanNoncanonicalUrl:
    "moz-src:///browser/components/tabnotes/CanonicalURL.sys.mjs",
  findCandidates: "moz-src:///browser/components/tabnotes/CanonicalURL.sys.mjs",
  pickCanonicalUrl:
    "moz-src:///browser/components/tabnotes/CanonicalURL.sys.mjs",
});

export class CanonicalURLChild extends JSWindowActorChild {
  handleEvent(event) {
    switch (event.type) {
      case "DOMContentLoaded":
      case "pageshow":
        this.#discoverCanonicalUrl();
        break;
      case "popstate":
        this.contentWindow.setTimeout(() => this.#discoverCanonicalUrl(), 0);
        break;
    }
  }

  receiveMessage(msg) {
    switch (msg.name) {
      case "CanonicalURL:Detect":
        this.#discoverCanonicalUrl();
        break;
      case "CanonicalURL:DetectFromPushState":
        this.sendAsyncMessage("CanonicalURL:Identified", {
          canonicalUrl: lazy.cleanNoncanonicalUrl(msg.data.pushStateUrl),
          canonicalUrlSources: ["pushState"],
        });
        break;
    }
  }

  #discoverCanonicalUrl() {
    const candidates = lazy.findCandidates(this.document);
    const canonicalUrl = lazy.pickCanonicalUrl(candidates);
    const canonicalUrlSources = Object.keys(candidates).filter(
      candidate => candidates[candidate]
    );
    this.sendAsyncMessage("CanonicalURL:Identified", {
      canonicalUrl,
      canonicalUrlSources,
    });
  }
}
