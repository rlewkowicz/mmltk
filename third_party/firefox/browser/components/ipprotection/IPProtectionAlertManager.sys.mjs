/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { BANDWIDTH } from "chrome://browser/content/ipprotection/ipprotection-constants.mjs";

const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "ipProtectionLocalization", () => {
  return new Localization(["browser/ipProtection.ftl"], true);
});

ChromeUtils.defineESModuleGetters(lazy, {
  EveryWindow: "resource:///modules/EveryWindow.sys.mjs",
  IPPProxyManager:
    "moz-src:///toolkit/components/ipprotection/IPPProxyManager.sys.mjs",
  IPPProxyStates:
    "moz-src:///toolkit/components/ipprotection/IPPProxyManager.sys.mjs",
  IPProtectionService:
    "moz-src:///toolkit/components/ipprotection/IPProtectionService.sys.mjs",
});

class IPProtectionAlertManagerClass {
  #localizationMessages = null;
  #promptsOpen = false;
  #initialized = false;

  get initialized() {
    return this.#initialized;
  }

  init() {
    if (this.#initialized) {
      return;
    }

    lazy.IPPProxyManager.addEventListener("IPPProxyManager:StateChanged", this);

    this.#initialized = true;
  }

  uninit() {
    if (!this.#initialized) {
      return;
    }

    lazy.IPPProxyManager.removeEventListener(
      "IPPProxyManager:StateChanged",
      this
    );

    this.#closeAllPrompts();

    this.#initialized = false;
  }

  get localizationMessages() {
    if (!this.#localizationMessages) {
      const [
        pausedTitle,
        pausedBody,
        closeTabsButton,
        continueButton,
        errorTitle,
        errorBody,
      ] = lazy.ipProtectionLocalization.formatMessagesSync([
        { id: "vpn-paused-alert-title" },
        {
          id: "vpn-paused-alert-body",
          args: { maxUsage: this.#getMaxBandwidthUsage() },
        },
        { id: "vpn-paused-alert-close-tabs-button" },
        { id: "vpn-paused-alert-continue-wo-vpn-button" },
        { id: "vpn-error-alert-title" },
        { id: "vpn-error-alert-body" },
      ]);

      this.#localizationMessages = {
        pausedTitle: pausedTitle.value,
        pausedBody: pausedBody.value,
        closeTabsButton: closeTabsButton.value,
        continueButton: continueButton.value,
        errorTitle: errorTitle.value,
        errorBody: errorBody.value,
      };
    }

    return this.#localizationMessages;
  }

  #getMaxBandwidthUsage() {
    if (lazy.IPPProxyManager.usageInfo?.max != null) {
      return Number(lazy.IPPProxyManager.usageInfo.max) / BANDWIDTH.BYTES_IN_GB;
    } else if (lazy.IPProtectionService.authProvider.maxBytes != null) {
      return (
        Number(lazy.IPProtectionService.authProvider.maxBytes) /
        BANDWIDTH.BYTES_IN_GB
      );
    }

    return BANDWIDTH.MAX_IN_GB;
  }

  handleEvent(event) {
    if (event.type !== "IPPProxyManager:StateChanged") {
      return;
    }

    switch (event.detail.state) {
      case lazy.IPPProxyStates.ACTIVE:
      case lazy.IPPProxyStates.NOT_READY:
      case lazy.IPPProxyStates.READY: {
        this.#closeAllPrompts();
        break;
      }
      case lazy.IPPProxyStates.PAUSED: {
        this.showPausedPrompts();
        break;
      }
      case lazy.IPPProxyStates.ERROR: {
        this.showErrorPrompts();
        break;
      }
    }
  }

  #createAllPrompts(title, body, button0, button1) {
    if (this.#promptsOpen) {
      return [];
    }

    const promises = [];
    for (let window of lazy.EveryWindow.readyWindows) {
      promises.push(
        Services.prompt.asyncConfirmEx(
          window.browsingContext,
          Services.prompt.MODAL_TYPE_INTERNAL_WINDOW,
          title,
          body,
          Ci.nsIPromptService.BUTTON_POS_0_DEFAULT |
            (Ci.nsIPromptService.BUTTON_TITLE_IS_STRING *
              Ci.nsIPromptService.BUTTON_POS_0) |
            (Ci.nsIPromptService.BUTTON_TITLE_IS_STRING *
              Ci.nsIPromptService.BUTTON_POS_1),
          button0,
          button1,
          null,
          null,
          false,
          { useTitle: true }
        )
      );
    }
    this.#promptsOpen = true;
    return promises;
  }

  #closeAllPrompts() {
    if (!this.#promptsOpen) {
      return;
    }

    for (let window of lazy.EveryWindow.readyWindows) {
      window.gDialogBox.dialog?.close();
    }

    this.#promptsOpen = false;
  }

  async showPausedPrompts() {
    if (!lazy.IPPProxyManager.active) {
      return;
    }
    let { pausedTitle, pausedBody, continueButton, closeTabsButton } =
      this.localizationMessages;

    const promises = this.#createAllPrompts(
      pausedTitle,
      pausedBody,
      continueButton,
      closeTabsButton
    );

    if (promises.length === 0) {
      return;
    }

    let result = await Promise.any(promises);
    let buttonClicked = result.getProperty("buttonNumClicked");

    this.#handlePromptAction(buttonClicked, "paused");
  }

  async showErrorPrompts() {
    if (!lazy.IPPProxyManager.active) {
      return;
    }

    let { errorTitle, errorBody, continueButton, closeTabsButton } =
      this.localizationMessages;

    const promises = this.#createAllPrompts(
      errorTitle,
      errorBody,
      continueButton,
      closeTabsButton
    );

    if (promises.length === 0) {
      return;
    }

    let result = await Promise.any(promises);
    let buttonClicked = result.getProperty("buttonNumClicked");

    this.#handlePromptAction(buttonClicked, "error");
  }

  #handlePromptAction(buttonClicked, reason) {
    this.#closeAllPrompts();

    if (buttonClicked === 0) {
      lazy.IPPProxyManager.stop(true);
    } else if (buttonClicked === 1) {
      this.#closeAllTabs();
    }

  }

  async #closeAllTabs() {
    const mostRecentWindow = Services.wm.getMostRecentBrowserWindow();
    const tabs = mostRecentWindow.gBrowser.tabs;
    mostRecentWindow.openTrustedLinkIn("about:home", "tab");
    mostRecentWindow.gBrowser.removeTabs(tabs);

    for (let window of lazy.EveryWindow.readyWindows) {
      if (window === mostRecentWindow) {
        continue;
      }

      window.close();
    }

    lazy.IPPProxyManager.stop(true);
  }
}

const IPProtectionAlertManager = new IPProtectionAlertManagerClass();
export { IPProtectionAlertManager };
