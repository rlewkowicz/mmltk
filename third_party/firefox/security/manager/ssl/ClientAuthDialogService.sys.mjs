/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export function ClientAuthDialogService() {}

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PromptUtils: "resource://gre/modules/PromptUtils.sys.mjs",
});

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

if (AppConstants.platform == "android") {
  ChromeUtils.defineESModuleGetters(lazy, {
    GeckoViewPrompter: "resource://gre/modules/GeckoViewPrompter.sys.mjs",
  });
}

function getTabDialogBoxForLoadContext(loadContext) {
  let tabBrowser = loadContext?.topFrameElement?.getTabBrowser();
  if (!tabBrowser) {
    return null;
  }
  for (let browser of tabBrowser.browsers) {
    if (browser.browserId == loadContext.top?.browserId) {
      return tabBrowser.getTabDialogBox(browser);
    }
  }
  return null;
}

ClientAuthDialogService.prototype = {
  classID: Components.ID("{d7d2490d-2640-411b-9f09-a538803c11ee}"),
  QueryInterface: ChromeUtils.generateQI(["nsIClientAuthDialogService"]),

  chooseCertificate: function ClientAuthDialogService_chooseCertificate(
    hostname,
    certArray,
    loadContext,
    caNames,
    callback
  ) {
    if (AppConstants.platform == "android") {
      const prompt = new lazy.GeckoViewPrompter(
        loadContext.topFrameElement.documentGlobal
      );
      let issuers = null;
      if (caNames.length) {
        issuers = [];
        for (let caName of caNames) {
          issuers.push(btoa(caName.map(b => String.fromCharCode(b)).join("")));
        }
      }
      prompt.asyncShowPrompt(
        { type: "certificate", host: hostname, issuers },
        result => {
          let certDB = Cc["@mozilla.org/security/x509certdb;1"].getService(
            Ci.nsIX509CertDB
          );
          let certificate = null;
          if (result.alias) {
            try {
              certificate = certDB.getAndroidCertificateFromAlias(result.alias);
            } catch (e) {
              console.error("couldn't get certificate from alias", e);
            }
          }
          callback.certificateChosen(
            certificate,
            Ci.nsIClientAuthRememberService.Session
          );
        }
      );

      return;
    }

    const clientAuthAskURI = "chrome://pippki/content/clientauthask.xhtml";
    let retVals = {
      cert: null,
      rememberDuration: Ci.nsIClientAuthRememberService.Session,
    };
    let args = lazy.PromptUtils.objectToPropBag({
      hostname,
      certArray,
      retVals,
    });

    let tabDialogBox = getTabDialogBoxForLoadContext(loadContext);
    if (tabDialogBox) {
      tabDialogBox.open(clientAuthAskURI, {}, args).closedPromise.then(() => {
        callback.certificateChosen(retVals.cert, retVals.rememberDuration);
      });
      return;
    }
    let browserWindow = loadContext?.topFrameElement?.documentGlobal;
    if (!browserWindow?.gDialogBox) {
      browserWindow = Services.wm.getMostRecentBrowserWindow();
    }

    if (browserWindow?.gDialogBox) {
      browserWindow.gDialogBox.open(clientAuthAskURI, args).then(() => {
        callback.certificateChosen(retVals.cert, retVals.rememberDuration);
      });
      return;
    }

    let mostRecentWindow = Services.wm.getMostRecentWindow("");
    Services.ww.openWindow(
      mostRecentWindow,
      clientAuthAskURI,
      "_blank",
      "centerscreen,chrome,modal,titlebar",
      args
    );
    callback.certificateChosen(retVals.cert, retVals.rememberDuration);
  },
};
