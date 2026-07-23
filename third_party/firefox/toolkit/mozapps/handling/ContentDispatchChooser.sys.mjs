/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

import { E10SUtils } from "resource://gre/modules/E10SUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  NimbusFeatures: "resource://nimbus/ExperimentAPI.sys.mjs",
});

const DIALOG_URL_APP_CHOOSER =
  "chrome://mozapps/content/handling/appChooser.xhtml";
const DIALOG_URL_PERMISSION =
  "chrome://mozapps/content/handling/permissionDialog.xhtml";

const MAILTO_SYSTEM_DEFAULT_ID = "system-default";

const gPrefs = {};
XPCOMUtils.defineLazyPreferenceGetter(
  gPrefs,
  "promptForExternal",
  "network.protocol-handler.prompt-from-external",
  true
);

const PROTOCOL_HANDLER_OPEN_PERM_KEY = "open-protocol-handler";
const PERMISSION_KEY_DELIMITER = "^";

export class nsContentDispatchChooser {
  async handleURI(
    aHandler,
    aURI,
    aPrincipal,
    aBrowsingContext,
    aTriggeredExternally = false
  ) {
    const isStandardProtocol = E10SUtils.STANDARD_SAFE_PROTOCOLS.includes(
      aURI.scheme
    );
    let callerHasPermission = this._hasProtocolHandlerPermission(
      aHandler,
      aPrincipal,
      aTriggeredExternally,
      isStandardProtocol
    );

    if (
      aTriggeredExternally &&
      gPrefs.promptForExternal &&
      !(
        aHandler.preferredAction == Ci.nsIHandlerInfo.useHelperApp &&
        aHandler.preferredApplicationHandler instanceof Ci.nsIWebHandlerApp
      )
    ) {
      aHandler.alwaysAskBeforeHandling = true;
    }

    if ("mailto" === aURI.scheme) {

      const browser = aBrowsingContext?.topFrameElement;
      // so the picker can be turned off remotely. When skipped, fall through to
      if (
        browser &&
        aHandler.alwaysAskBeforeHandling &&
        lazy.NimbusFeatures.mailto.getVariable("dialog") &&
        this._hasMailtoHandlerOptions(aHandler)
      ) {
        lazy.NimbusFeatures.mailto.recordExposureEvent();
        let choice = null;
        let dialogFailed = false;
        try {
          choice = await this._promiseMailtoChoice(aBrowsingContext, aHandler);
        } catch {
          // The picker failed to open; fall through to the default protocol
          dialogFailed = true;
        }
        if (choice) {
          const { handler, alwaysAsk } = choice;
          if (handler === MAILTO_SYSTEM_DEFAULT_ID) {
            aHandler.preferredAction = Ci.nsIHandlerInfo.useSystemDefault;
          } else {
            aHandler.preferredApplicationHandler = handler;
            aHandler.preferredAction = Ci.nsIHandlerInfo.useHelperApp;
          }
          aHandler.alwaysAskBeforeHandling = alwaysAsk;
          try {
            aHandler.launchWithURI(aURI, null);
            Cc["@mozilla.org/uriloader/handler-service;1"]
              .getService(Ci.nsIHandlerService)
              .store(aHandler);
          } catch (error) {
            console.error(error);
          }
        }
        if (!dialogFailed) {
          return;
        }
      }
    }

    if (
      callerHasPermission &&
      !aHandler.alwaysAskBeforeHandling &&
      (aHandler.preferredAction == Ci.nsIHandlerInfo.useHelperApp ||
        aHandler.preferredAction == Ci.nsIHandlerInfo.useSystemDefault)
    ) {
      try {
        aHandler.launchWithURI(aURI, aBrowsingContext);
        return;
      } catch (error) {
        if (error.result == Cr.NS_ERROR_FILE_NOT_FOUND) {
          aHandler.alwaysAskBeforeHandling = true;
        } else {
          throw error;
        }
      }
    }

    let shouldOpenHandler = false;

    try {
      shouldOpenHandler = await this._prompt(
        aHandler,
        aPrincipal,
        callerHasPermission,
        aBrowsingContext,
        aURI
      );
    } catch (error) {
      console.error(error.message);
    }

    if (!shouldOpenHandler) {
      return;
    }

    aHandler.launchWithURI(aURI, aBrowsingContext);
  }

  _hasMailtoHandlerOptions(aHandler) {
    if (aHandler.hasDefaultHandler) {
      return true;
    }
    for (const app of aHandler.possibleApplicationHandlers.enumerate()) {
      if (app instanceof Ci.nsIWebHandlerApp) {
        return true;
      }
    }
    return false;
  }

  async _promiseMailtoChoice(aBrowsingContext, aHandler) {
    const outArgs = Cc["@mozilla.org/hash-property-bag;1"].createInstance(
      Ci.nsIWritablePropertyBag
    );
    outArgs.setProperty("openHandler", false);
    outArgs.setProperty("preferredAction", aHandler.preferredAction);
    outArgs.setProperty(
      "preferredApplicationHandler",
      aHandler.preferredApplicationHandler
    );
    outArgs.setProperty(
      "alwaysAskBeforeHandling",
      aHandler.alwaysAskBeforeHandling
    );

    try {
      await this._openDialog(
        DIALOG_URL_APP_CHOOSER,
        { handler: aHandler, outArgs, kind: "mailto" },
        aBrowsingContext
      );
    } catch (error) {
      console.error(error);
      throw error;
    }

    if (!outArgs.getProperty("openHandler")) {
      return null;
    }

    const useSystemDefault =
      outArgs.getProperty("preferredAction") ==
      Ci.nsIHandlerInfo.useSystemDefault;
    const alwaysAsk = outArgs.getProperty("alwaysAskBeforeHandling");

    return {
      handler: useSystemDefault
        ? MAILTO_SYSTEM_DEFAULT_ID
        : outArgs.getProperty("preferredApplicationHandler"),
      alwaysAsk,
    };
  }

  _getHandlerName(aHandler) {
    if (aHandler.alwaysAskBeforeHandling) {
      return null;
    }
    if (
      aHandler.preferredAction == Ci.nsIHandlerInfo.useSystemDefault &&
      aHandler.hasDefaultHandler
    ) {
      return aHandler.defaultDescription;
    }
    return aHandler.preferredApplicationHandler?.name;
  }

  async _prompt(aHandler, aPrincipal, aHasPermission, aBrowsingContext, aURI) {
    let shouldOpenHandler = aHasPermission;
    let resetHandlerChoice = false;
    let updateHandlerData = false;

    const isStandardProtocol = E10SUtils.STANDARD_SAFE_PROTOCOLS.includes(
      aURI.scheme
    );
    const {
      hasDefaultHandler,
      preferredApplicationHandler,
      alwaysAskBeforeHandling,
    } = aHandler;

    if (
      !isStandardProtocol &&
      hasDefaultHandler &&
      preferredApplicationHandler == null &&
      alwaysAskBeforeHandling
    ) {
      aHandler.alwaysAskBeforeHandling = false;
      updateHandlerData = true;
    }

    if (!aHasPermission) {
      let canPersistPermission = this._isSupportedPrincipal(aPrincipal);

      let outArgs = Cc["@mozilla.org/hash-property-bag;1"].createInstance(
        Ci.nsIWritablePropertyBag
      );
      outArgs.setProperty("granted", false);
      outArgs.setProperty("resetHandlerChoice", null);
      outArgs.setProperty("remember", null);

      await this._openDialog(
        DIALOG_URL_PERMISSION,
        {
          handler: aHandler,
          principal: aPrincipal,
          browsingContext: aBrowsingContext,
          outArgs,
          canPersistPermission,
          preferredHandlerName: this._getHandlerName(aHandler),
        },
        aBrowsingContext
      );
      if (!outArgs.getProperty("granted")) {
        return false;
      }

      resetHandlerChoice = outArgs.getProperty("resetHandlerChoice");

      if (!resetHandlerChoice && aPrincipal) {
        let remember = outArgs.getProperty("remember");
        this._updatePermission(aPrincipal, aHandler.type, remember);
      }

      shouldOpenHandler = true;
    }

    if (aHandler.alwaysAskBeforeHandling || resetHandlerChoice) {
      let outArgs = Cc["@mozilla.org/hash-property-bag;1"].createInstance(
        Ci.nsIWritablePropertyBag
      );
      outArgs.setProperty("openHandler", false);
      outArgs.setProperty("preferredAction", aHandler.preferredAction);
      outArgs.setProperty(
        "preferredApplicationHandler",
        aHandler.preferredApplicationHandler
      );
      outArgs.setProperty(
        "alwaysAskBeforeHandling",
        aHandler.alwaysAskBeforeHandling
      );
      let usePrivateBrowsing = aBrowsingContext?.usePrivateBrowsing;
      await this._openDialog(
        DIALOG_URL_APP_CHOOSER,
        {
          handler: aHandler,
          outArgs,
          usePrivateBrowsing,
          enableButtonDelay: aHasPermission,
        },
        aBrowsingContext
      );

      shouldOpenHandler = outArgs.getProperty("openHandler");

      if (shouldOpenHandler) {
        for (let prop of [
          "preferredAction",
          "preferredApplicationHandler",
          "alwaysAskBeforeHandling",
        ]) {
          aHandler[prop] = outArgs.getProperty(prop);
        }
        updateHandlerData = true;
      }
    }

    if (updateHandlerData) {
      Cc["@mozilla.org/uriloader/handler-service;1"]
        .getService(Ci.nsIHandlerService)
        .store(aHandler);
    }

    return shouldOpenHandler;
  }

  _hasProtocolHandlerPermission(
    aHandler,
    aPrincipal,
    aTriggeredExternally,
    isStandardProtocol
  ) {
    const { type, hasDefaultHandler, preferredApplicationHandler } = aHandler;

    if (
      Services.prefs.getBoolPref(
        "network.protocol-handler.external." + type,
        false
      )
    ) {
      return true;
    }

    if (
      !aPrincipal ||
      (aPrincipal.isSystemPrincipal && !aTriggeredExternally) ||
      (!isStandardProtocol &&
        hasDefaultHandler &&
        !preferredApplicationHandler &&
        aTriggeredExternally)
    ) {
      return false;
    }

    let key = this._getSkipProtoDialogPermissionKey(type);
    return (
      Services.perms.testPermissionFromPrincipal(aPrincipal, key) ===
      Services.perms.ALLOW_ACTION
    );
  }

  _getSkipProtoDialogPermissionKey(aProtocolScheme) {
    return (
      PROTOCOL_HANDLER_OPEN_PERM_KEY +
      PERMISSION_KEY_DELIMITER +
      aProtocolScheme
    );
  }

  async _openDialog(aDialogURL, aDialogArgs, aBrowsingContext) {
    let resizable = `resizable=${
      aDialogURL == DIALOG_URL_APP_CHOOSER ? "yes" : "no"
    }`;

    if (aBrowsingContext) {
      let window = aBrowsingContext.topChromeWindow;
      if (!window) {
        throw new Error(
          "Can't show external protocol dialog. BrowsingContext has no chrome window associated."
        );
      }

      let { topFrameElement } = aBrowsingContext;
      if (topFrameElement?.tagName != "browser") {
        throw new Error(
          "Can't show external protocol dialog. BrowsingContext has no browser associated."
        );
      }

      let getTabDialogBox = window.gBrowser?.getTabDialogBox;
      if (getTabDialogBox) {
        return getTabDialogBox(topFrameElement).open(
          aDialogURL,
          {
            features: resizable,
            allowDuplicateDialogs: false,
            keepOpenSameOriginNav: true,
          },
          aDialogArgs
        ).closedPromise;
      }
    }

    let win = Services.ww.openWindow(
      null,
      aDialogURL,
      null,
      `chrome,dialog=yes,centerscreen,${resizable}`,
      aDialogArgs
    );

    return new Promise(resolve => {
      win.addEventListener("unload", function onUnload(event) {
        if (event.target.location != aDialogURL) {
          return;
        }
        win.removeEventListener("unload", onUnload);
        resolve();
      });
    });
  }

  _updatePermission(aPrincipal, aScheme, aAllow) {
    if (
      aPrincipal.isSystemPrincipal ||
      !this._isSupportedPrincipal(aPrincipal)
    ) {
      return;
    }

    let principal = aPrincipal;

    let permKey = this._getSkipProtoDialogPermissionKey(aScheme);
    if (aAllow) {
      Services.perms.addFromPrincipal(
        principal,
        permKey,
        Services.perms.ALLOW_ACTION,
        Services.perms.EXPIRE_NEVER
      );
    } else {
      Services.perms.removeFromPrincipal(principal, permKey);
    }
  }

  _isSupportedPrincipal(aPrincipal) {
    if (!aPrincipal) {
      return false;
    }

    return ["http", "https", "file"].some(scheme =>
      aPrincipal.schemeIs(scheme)
    );
  }
}

nsContentDispatchChooser.prototype.classID = Components.ID(
  "e35d5067-95bc-4029-8432-e8f1e431148d"
);
nsContentDispatchChooser.prototype.QueryInterface = ChromeUtils.generateQI([
  "nsIContentDispatchChooser",
]);
