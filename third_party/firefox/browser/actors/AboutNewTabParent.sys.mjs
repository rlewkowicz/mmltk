/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AboutNewTab: "resource:///modules/AboutNewTab.sys.mjs",
  ASRouter: "resource:///modules/asrouter/ASRouter.sys.mjs",
});

let gLoadedTabs = new Map();

export class AboutNewTabParent extends JSWindowActorParent {
  static get loadedTabs() {
    return gLoadedTabs;
  }

  getTabDetails() {
    let browser = this.browsingContext.top.embedderElement;
    return browser ? gLoadedTabs.get(browser) : null;
  }

  handleEvent(event) {
    if (event.type == "SwapDocShells") {
      let oldBrowser = this.browsingContext.top.embedderElement;
      let newBrowser = event.detail;

      let tabDetails = gLoadedTabs.get(oldBrowser);
      if (tabDetails) {
        tabDetails.browser = newBrowser;
        gLoadedTabs.delete(oldBrowser);
        gLoadedTabs.set(newBrowser, tabDetails);

        oldBrowser.removeEventListener("SwapDocShells", this);
        newBrowser.addEventListener("SwapDocShells", this);
      }
    }
  }

  makeTransientIfDisabledAndInitial() {
    if (Services.prefs.getBoolPref("browser.newtabpage.enabled", true)) {
      return;
    }

    const sh = this.browsingContext.sessionHistory;
    if (!sh || sh.count > 1) {
      return;
    }

    const entry = sh.getEntryAtIndex(0);
    if (entry.URI.spec === "about:newtab") {
      entry.setTransient();
    }
  }

  async receiveMessage(message) {
    switch (message.name) {
      case "AboutNewTabVisible":
        {
          const browsingContext = this.browsingContext;
          await lazy.ASRouter.waitForInitialized;
          if (!browsingContext.isDiscarded) {
            await lazy.ASRouter.sendTriggerMessage({
              browser: browsingContext.top.embedderElement,
              id: "defaultBrowserCheck",
              context: { source: "newtab" },
            });
          }
          if (!browsingContext.isDiscarded) {
            await lazy.ASRouter.sendTriggerMessage({
              browser: browsingContext.top.embedderElement,
              id: "newtabMessageCheck",
            });
          }
        }
        break;
      case "Init": {
        let browsingContext = this.browsingContext;
        let browser = browsingContext.top.embedderElement;
        if (!browser) {
          return null;
        }

        let tabDetails = {
          actor: this,
          browser,
          browsingContext,
          portID: message.data.portID,
          url: message.data.url,
        };
        gLoadedTabs.set(browser, tabDetails);

        browser.addEventListener("SwapDocShells", this);
        browser.addEventListener("EndSwapDocShells", this);

        this.notifyActivityStreamChannel("onNewTabInit", message, tabDetails);
        break;
      }

      case "Load": {
        this.notifyActivityStreamChannel("onNewTabLoad", message);
        this.makeTransientIfDisabledAndInitial();
        break;
      }

      case "Unload": {
        let tabDetails = this.getTabDetails();
        if (!tabDetails) {
          tabDetails = this.getByBrowsingContext(this.browsingContext);
        }

        if (!tabDetails) {
          return null;
        }

        tabDetails.browser.removeEventListener("EndSwapDocShells", this);

        gLoadedTabs.delete(tabDetails.browser);

        this.notifyActivityStreamChannel("onNewTabUnload", message, tabDetails);
        break;
      }

      case "ActivityStream:ContentToMain": {
        this.notifyActivityStreamChannel("onMessage", message);
        break;
      }

      case "AssignRenderer": {
        if (!lazy.AboutNewTab.activityStream) {
          await lazy.AboutNewTab.activityStreamPromise;
        }
        const rendererActor = this.browsingContext.currentWindowGlobal.getActor(
          "MozNewTabRemoteRendererProtocol"
        );
        return rendererActor.assignRenderer();
      }
    }
    return null;
  }

  notifyActivityStreamChannel(name, message, tabDetails) {
    if (!tabDetails) {
      tabDetails = this.getTabDetails();
      if (!tabDetails) {
        return;
      }
    }

    let channel = this.getChannel();
    if (!channel) {
      AboutNewTabParent.#queuedMessages.push({
        actor: this,
        name,
        message,
        tabDetails,
      });
      return;
    }

    let messageToSend = {
      target: this,
      data: message.data || {},
    };

    channel[name](messageToSend, tabDetails);
  }

  getByBrowsingContext(expectedBrowsingContext) {
    for (let tabDetails of AboutNewTabParent.loadedTabs.values()) {
      if (tabDetails.browsingContext === expectedBrowsingContext) {
        return tabDetails;
      }
    }

    return null;
  }

  getChannel() {
    return lazy.AboutNewTab.activityStream?.store?.getMessageChannel();
  }

  static #queuedMessages = [];

  static flushQueuedMessagesFromContent() {
    for (let messageData of AboutNewTabParent.#queuedMessages) {
      let { actor, name, message, tabDetails } = messageData;
      actor.notifyActivityStreamChannel(name, message, tabDetails);
    }
    AboutNewTabParent.#queuedMessages = [];
  }
}
