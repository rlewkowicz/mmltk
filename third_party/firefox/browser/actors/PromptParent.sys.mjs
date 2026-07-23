/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PromptUtils: "resource://gre/modules/PromptUtils.sys.mjs",
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "gTabBrowserLocalization", function () {
  return new Localization(["browser/tabbrowser.ftl"], true);
});


let gBrowserDialogs = new WeakMap();

export class PromptParent extends JSWindowActorParent {
  didDestroy() {
    this.forceClosePrompts();
  }

  registerDialog(dialog, id) {
    let dialogs = gBrowserDialogs.get(this.browsingContext);
    if (!dialogs) {
      dialogs = new Map();
      gBrowserDialogs.set(this.browsingContext, dialogs);
    }

    dialogs.set(id, dialog);
  }

  unregisterPrompt(id) {
    let dialogs = gBrowserDialogs.get(this.browsingContext);
    dialogs?.delete(id);
  }

  forceClosePrompts() {
    let dialogs = gBrowserDialogs.get(this.browsingContext) || [];

    for (let [, dialog] of dialogs) {
      dialog?.abort();
    }
  }

  isAboutAddonsOptionsPage(browsingContext) {
    const { embedderWindowGlobal, name } = browsingContext;
    if (!embedderWindowGlobal) {
      return false;
    }

    return (
      embedderWindowGlobal.documentPrincipal.isSystemPrincipal &&
      embedderWindowGlobal.documentURI.spec === "about:addons" &&
      name === "addon-inline-options"
    );
  }

  isEmbeddedInSidebar(browser) {
    if (
      browser?.documentGlobal?.browsingContext.embedderElement?.id != "sidebar"
    ) {
      return false;
    }
    if (
      browser.getAttribute("messagemanagergroup") == "webext-browsers" ||
      browser.getAttribute("messagemanagergroup") == "chatbot-browser"
    ) {
      return false;
    }
    return true;
  }

  receiveMessage(message) {
    switch (message.name) {
      case "Prompt:Open":
        if (!this.windowContext.isActiveInTab) {
          return undefined;
        }

        return this.openPromptWithTabDialogBox(message.data);
    }

    return undefined;
  }

  async openPromptWithTabDialogBox(args) {
    const COMMON_DIALOG = "chrome://global/content/commonDialog.xhtml";
    const SELECT_DIALOG = "chrome://global/content/selectDialog.xhtml";
    let uri = args.promptType == "select" ? SELECT_DIALOG : COMMON_DIALOG;

    let browsingContext = this.browsingContext.top;

    let browser = browsingContext.embedderElement;

    let isEmbeddedInSidebar = this.isEmbeddedInSidebar(browser);
    if (isEmbeddedInSidebar || this.isAboutAddonsOptionsPage(browsingContext)) {
      browser = browser.documentGlobal.browsingContext.embedderElement;
    }

    let promptRequiresBrowser =
      args.modalType === Services.prompt.MODAL_TYPE_TAB ||
      args.modalType === Services.prompt.MODAL_TYPE_CONTENT;
    if (promptRequiresBrowser && !browser) {
      let modal_type =
        args.modalType === Services.prompt.MODAL_TYPE_TAB ? "tab" : "content";
      throw new Error(`Cannot ${modal_type}-prompt without a browser!`);
    }

    const closingEventDetails =
      args.modalType === Services.prompt.MODAL_TYPE_CONTENT
        ? {
            owningBrowsingContext: this.browsingContext,
            promptType: args.inPermitUnload ? "beforeunload" : args.promptType,
          }
        : null;

    let win;

    if (!browsingContext.isContent && browsingContext.window) {
      win = browsingContext.window;
    } else {
      win = browser?.documentGlobal;
    }

    if (win?.winUtils && !win.winUtils.isParentWindowMainWidgetVisible) {
      throw new Error("Cannot open a prompt in a hidden window");
    }

    try {
      if (browsingContext.embedderElement) {
        browsingContext.embedderElement.enterModalState();
        lazy.PromptUtils.fireDialogEvent(
          win,
          "DOMWillOpenModalDialog",
          browsingContext.embedderElement,
          this.getOpenEventDetail(args)
        );
      }

      args.promptAborted = false;
      args.openedWithTabDialog = true;
      args.owningBrowsingContext = this.browsingContext;

      let bag;

      if (promptRequiresBrowser && win?.gBrowser?.getTabDialogBox) {
        let dialogBox = win.gBrowser.getTabDialogBox(browser);

        if (dialogBox._allowTabFocusByPromptPrincipal) {
          this.addTabSwitchCheckboxToArgs(dialogBox, args);
        }

        let currentLocationsTabLabel;

        let targetTab = win.gBrowser.getTabForBrowser(browser);
        if (
          !Services.prefs.getBoolPref(
            "privacy.authPromptSpoofingProtection",
            false
          )
        ) {
          args.isTopLevelCrossDomainAuth = false;
        }
        if (args.isTopLevelCrossDomainAuth && targetTab) {
          browser.currentAuthPromptURI = args.channel.URI;
          if (browser == win.gBrowser.selectedBrowser) {
            win.gURLBar.setURI();
          }
          currentLocationsTabLabel = targetTab.label;
          win.gBrowser.setTabLabelForAuthPrompts(
            targetTab,
            lazy.BrowserUtils.formatURIForDisplay(args.channel.URI)
          );
        }
        bag = lazy.PromptUtils.objectToPropBag(args);
        let promptID = args._remoteId;
        try {
          let { dialog, closedPromise } = dialogBox.open(
            uri,
            {
              features: "resizable=no",
              modalType: args.modalType,
              allowFocusCheckbox: args.allowFocusCheckbox,
              hideContent: args.isTopLevelCrossDomainAuth,
              webProgress: isEmbeddedInSidebar
                ? browsingContext?.webProgress
                : undefined,
            },
            bag
          );
          dialog.promptID = promptID;
          this.registerDialog(dialog, promptID);
          await closedPromise;
        } finally {
          if (args.isTopLevelCrossDomainAuth) {
            browser.currentAuthPromptURI = null;
            if (browser == win.gBrowser.selectedBrowser) {
              win.gURLBar.setURI();
            }
            win.gBrowser.setTabLabelForAuthPrompts(
              targetTab,
              currentLocationsTabLabel
            );
          }
          this.unregisterPrompt(promptID);
        }
      } else {
        args.modalType = Services.prompt.MODAL_TYPE_WINDOW;
        bag = lazy.PromptUtils.objectToPropBag(args);
        Services.ww.openWindow(
          win,
          uri,
          "_blank",
          "centerscreen,chrome,modal,titlebar",
          bag
        );
      }

      lazy.PromptUtils.propBagToObject(bag, args);
    } finally {
      if (browsingContext.embedderElement) {
        browsingContext.embedderElement.maybeLeaveModalState();
        lazy.PromptUtils.fireDialogEvent(
          win,
          "DOMModalDialogClosed",
          browsingContext.embedderElement,
          closingEventDetails
            ? {
                ...closingEventDetails,
                areLeaving: args.ok,
                value: args.ok ? args.value : null,
              }
            : null
        );
      }
    }
    return args;
  }

  getOpenEventDetail(args) {
    let details =
      args.modalType === Services.prompt.MODAL_TYPE_CONTENT
        ? {
            inPermitUnload: args.inPermitUnload,
            promptPrincipal: args.promptPrincipal,
            tabPrompt: true,
          }
        : null;

    return details;
  }

  addTabSwitchCheckboxToArgs(dialogBox, args) {
    let allowTabFocusByPromptPrincipal =
      dialogBox._allowTabFocusByPromptPrincipal;

    if (
      allowTabFocusByPromptPrincipal &&
      args.modalType === Services.prompt.MODAL_TYPE_CONTENT
    ) {
      let domain = allowTabFocusByPromptPrincipal.addonPolicy?.name;
      try {
        domain ||= allowTabFocusByPromptPrincipal.URI.displayHostPort;
      } catch (ex) {
      }
      domain ||= allowTabFocusByPromptPrincipal.URI.prePath;
      let [allowFocusMsg] = lazy.gTabBrowserLocalization.formatMessagesSync([
        {
          id: "tabbrowser-allow-dialogs-to-get-focus",
          args: { domain },
        },
      ]);
      let labelAttr = allowFocusMsg.attributes.find(a => a.name == "label");
      if (labelAttr) {
        args.allowFocusCheckbox = true;
        args.checkLabel = labelAttr.value;
      }
    }
  }
}
