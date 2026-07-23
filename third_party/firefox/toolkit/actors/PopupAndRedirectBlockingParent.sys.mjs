/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export class PopupAndRedirectBlocker {
  #mBrowser;
  #mBlockedPopupCounts;
  #mBlockedRedirects;
  #mHasBeenDismissed;

  constructor(aBrowser) {
    this.#mBrowser = aBrowser;
    this.#mBlockedPopupCounts = new WeakMap();
    this.#mBlockedRedirects = new WeakMap();
    this.#mHasBeenDismissed = new WeakSet();
  }

  getBlockedPopupCount() {
    let blockedPopupCount = 0;

    const contextsToVisit = [this.#mBrowser.browsingContext];
    while (contextsToVisit.length) {
      const currentBC = contextsToVisit.pop();
      const currentWG = currentBC.currentWindowGlobal;
      if (!currentWG) {
        continue;
      }

      const currentCount = this.#mBlockedPopupCounts.get(currentWG) || 0;
      blockedPopupCount += currentCount;
      contextsToVisit.push(...currentBC.children);
    }

    return blockedPopupCount;
  }

  isRedirectBlocked() {
    const browserBC = this.#mBrowser.browsingContext;
    const browserWG = browserBC.currentWindowGlobal;
    if (!browserWG) {
      return false;
    }

    return this.#mBlockedRedirects.has(browserWG);
  }

  hasBeenDismissed() {
    const browserBC = this.#mBrowser.browsingContext;
    const browserWG = browserBC.currentWindowGlobal;
    if (!browserWG) {
      return false;
    }

    return this.#mHasBeenDismissed.has(browserWG);
  }

  eventCallback(reason) {
    if (reason == "dismissed") {
      const browserBC = this.#mBrowser.browsingContext;
      const browserWG = browserBC.currentWindowGlobal;
      if (!browserWG) {
        return;
      }

      this.#mHasBeenDismissed.add(browserWG);
    }
  }

  async getBlockedPopups() {
    const contextsToVisit = [this.#mBrowser.browsingContext];
    const result = [];

    while (contextsToVisit.length) {
      const currentBC = contextsToVisit.pop();
      const currentWG = currentBC.currentWindowGlobal;
      if (!currentWG) {
        continue;
      }

      const currentCount = this.#mBlockedPopupCounts.get(currentWG) || 0;
      if (currentCount) {
        const actor = currentWG.getActor("PopupAndRedirectBlocking");
        const popups = await actor.sendQuery("GetBlockedPopups");
        for (const popup of popups) {
          result.push({
            browsingContext: currentBC,
            innerWindowId: currentWG.innerWindowId,
            popupWindowURISpec: popup.popupWindowURISpec,
            reportIndex: popup.reportIndex,
          });
        }
      }

      contextsToVisit.push(...currentBC.children);
    }

    return result;
  }

  async getBlockedRedirect() {
    const browserBC = this.#mBrowser.browsingContext;
    const browserWG = browserBC.currentWindowGlobal;
    if (!browserWG) {
      return null;
    }

    const sourceBC = this.#mBlockedRedirects.get(browserWG);
    if (!sourceBC) {
      return null;
    }

    const sourceWG = sourceBC.currentWindowGlobal;
    if (!sourceWG) {
      return null;
    }

    const actor = sourceWG.getActor("PopupAndRedirectBlocking");
    const redirect = await actor.sendQuery("GetBlockedRedirect");

    return {
      browsingContext: sourceBC,
      innerWindowId: sourceWG.innerWindowId,
      redirectURISpec: redirect.redirectURISpec,
    };
  }

  unblockPopup(aBrowsingContext, aInnerWindowId, aReportIndex) {
    const sourceWG = aBrowsingContext.currentWindowGlobal;
    if (sourceWG?.innerWindowId != aInnerWindowId) {
      return;
    }

    const actor = sourceWG.getActor("PopupAndRedirectBlocking");
    actor.sendAsyncMessage("UnblockPopup", { reportIndex: aReportIndex });
  }

  unblockRedirect(aBrowsingContext, aInnerWindowId, aRedirectURISpec) {
    const sourceWG = aBrowsingContext.currentWindowGlobal;
    if (!sourceWG || sourceWG.innerWindowId != aInnerWindowId) {
      return;
    }

    const uri = Services.io.newURI(aRedirectURISpec);
    aBrowsingContext.top.loadURI(uri, {
      triggeringPrincipal: sourceWG.documentPrincipal,
    });
  }

  async unblockAllPopups() {
    const popups = await this.getBlockedPopups();
    for (let i = 0; i < popups.length; ++i) {
      const popup = popups[i];
      this.unblockPopup(
        popup.browsingContext,
        popup.innerWindowId,
        popup.reportIndex
      );
    }
  }

  async unblockFirstRedirect() {
    const redirect = await this.getBlockedRedirect();
    if (!redirect) {
      return;
    }

    this.unblockRedirect(
      redirect.browsingContext,
      redirect.innerWindowId,
      redirect.redirectURISpec
    );
  }

  sendObserverUpdateBlockedPopupsEvent() {
    const event = new Event("DOMUpdateBlockedPopups", {
      bubbles: true,
      cancelable: true,
    });
    this.#mBrowser.dispatchEvent(event);
  }

  sendObserverUpdateBlockedRedirectEvent() {
    const event = new Event("DOMUpdateBlockedRedirect", {
      bubbles: true,
      cancelable: true,
    });
    this.#mBrowser.dispatchEvent(event);
  }

  updateObserverAboutBlockedPopups(aBrowsingContext, aBlockedPopupCount) {
    const sourceWG = aBrowsingContext.currentWindowGlobal;
    if (!sourceWG) {
      return;
    }

    this.#mBlockedPopupCounts.set(sourceWG, aBlockedPopupCount);
    this.sendObserverUpdateBlockedPopupsEvent();
  }

  updateObserverAboutBlockedRedirect(aBrowsingContext) {
    const sourceWG = aBrowsingContext.currentWindowGlobal;
    if (!sourceWG) {
      return;
    }

    const browserBC = this.#mBrowser.browsingContext;
    const browserWG = browserBC.currentWindowGlobal;
    if (!browserWG) {
      return;
    }

    if (this.#mBlockedRedirects.has(browserWG)) {
      return;
    }

    this.#mBlockedRedirects.set(browserWG, aBrowsingContext);
    this.sendObserverUpdateBlockedRedirectEvent();
  }

  destroy(aBrowsingContext) {
    const sourceWG = aBrowsingContext.currentWindowGlobal;
    this.#mBlockedPopupCounts.delete(sourceWG);
    this.sendObserverUpdateBlockedPopupsEvent();

    if (aBrowsingContext == aBrowsingContext.top) {
      const browserBC = this.#mBrowser.browsingContext;
      const browserWG = browserBC.currentWindowGlobal;

      this.#mBlockedRedirects.delete(browserWG);
      this.sendObserverUpdateBlockedRedirectEvent();

      this.#mHasBeenDismissed.delete(browserWG);
    }
  }
}

export class PopupAndRedirectBlockingParent extends JSWindowActorParent {
  didDestroy() {
    const browser = this.browsingContext.top.embedderElement;
    if (!browser) {
      return;
    }

    browser.popupAndRedirectBlocker.destroy(this.browsingContext);
  }

  receiveMessage(aMessage) {
    switch (aMessage.name) {
      case "UpdateBlockedPopups":
        this.updateBrowserAboutPopupCount(aMessage);
        break;

      case "UpdateBlockedRedirect":
        this.updateBrowserAboutBlockedRedirect();
        break;
    }
  }

  updateBrowserAboutPopupCount(aMessage) {
    const browser = this.browsingContext.top.embedderElement;
    if (!browser) {
      return;
    }

    browser.popupAndRedirectBlocker.updateObserverAboutBlockedPopups(
      this.browsingContext,
      aMessage.data.count
    );
  }

  updateBrowserAboutBlockedRedirect() {
    const browser = this.browsingContext.top.embedderElement;
    if (!browser) {
      return;
    }

    browser.popupAndRedirectBlocker.updateObserverAboutBlockedRedirect(
      this.browsingContext
    );
  }
}
