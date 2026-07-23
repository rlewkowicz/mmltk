/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

export class InteractionsChild extends JSWindowActorChild {
  #progressListener;
  #currentURL;

  actorCreated() {
    this.isContentWindowPrivate =
      lazy.PrivateBrowsingUtils.isContentWindowPrivate(this.contentWindow);

    if (this.isContentWindowPrivate) {
      return;
    }

    this.#progressListener = {
      onLocationChange: (webProgress, request, location, flags) => {
        this.onLocationChange(webProgress, request, location, flags);
      },

      QueryInterface: ChromeUtils.generateQI([
        "nsIWebProgressListener2",
        "nsIWebProgressListener",
        "nsISupportsWeakReference",
      ]),
    };

    let webProgress = this.docShell
      .QueryInterface(Ci.nsIInterfaceRequestor)
      .getInterface(Ci.nsIWebProgress);
    webProgress.addProgressListener(
      this.#progressListener,
      Ci.nsIWebProgress.NOTIFY_STATE_DOCUMENT |
        Ci.nsIWebProgress.NOTIFY_LOCATION
    );
  }

  didDestroy() {
    if (!this.#progressListener || !this.docShell) {
      return;
    }

    let webProgress = this.docShell
      .QueryInterface(Ci.nsIInterfaceRequestor)
      .getInterface(Ci.nsIWebProgress);
    webProgress.removeProgressListener(this.#progressListener);
  }

  onLocationChange(webProgress, request, location, flags) {
    if (!webProgress.isTopLevel) {
      return;
    }

    if (!(flags & Ci.nsIWebProgressListener.LOCATION_CHANGE_SAME_DOCUMENT)) {
      return;
    }

    this.#recordNewPage();
  }

  #recordNewPage() {
    if (!this.docShell.currentDocumentChannel) {
      this.sendAsyncMessage("Interactions:PageHide");
      return;
    }

    let docInfo = this.#getDocumentInfo();

    if (docInfo.url == this.#currentURL) {
      return;
    }

    this.#currentURL = docInfo.url;

    if (
      this.docShell.currentDocumentChannel instanceof Ci.nsIHttpChannel &&
      !this.docShell.currentDocumentChannel.requestSucceeded
    ) {
      return;
    }

    this.sendAsyncMessage("Interactions:PageLoaded", docInfo);
  }

  async handleEvent(event) {
    if (this.isContentWindowPrivate) {
      return;
    }
    switch (event.type) {
      case "DOMContentLoaded": {
        this.#recordNewPage();
        break;
      }
      case "pagehide": {
        let currentDocumentChannel =  (
          this.docShell.currentDocumentChannel
        );
        if (!currentDocumentChannel) {
          return;
        }

        if (!currentDocumentChannel.requestSucceeded) {
          return;
        }

        this.sendAsyncMessage("Interactions:PageHide");
        break;
      }
    }
  }

  #getDocumentInfo() {
    let doc = this.document;

    let referrer;
    if (doc.referrer) {
      referrer = Services.io.newURI(doc.referrer);
    }
    return {
      isActive: this.manager.browsingContext.isActive,
      url: doc.documentURIObject.specIgnoringRef,
      referrer: referrer?.specIgnoringRef,
    };
  }
}
