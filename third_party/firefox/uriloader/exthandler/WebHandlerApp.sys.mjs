/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

export function nsWebHandlerApp() {}

nsWebHandlerApp.prototype = {
  classDescription: "A web handler for protocols and content",
  classID: Components.ID("8b1ae382-51a9-4972-b930-56977a57919d"),
  contractID: "@mozilla.org/uriloader/web-handler-app;1",
  QueryInterface: ChromeUtils.generateQI(["nsIWebHandlerApp", "nsIHandlerApp"]),

  _name: null,
  _detailedDescription: null,
  _uriTemplate: null,


  get name() {
    return this._name;
  },

  set name(aName) {
    this._name = aName;
  },

  get detailedDescription() {
    return this._detailedDescription;
  },

  set detailedDescription(aDesc) {
    this._detailedDescription = aDesc;
  },

  equals(aHandlerApp) {
    if (!aHandlerApp) {
      throw Components.Exception("", Cr.NS_ERROR_NULL_POINTER);
    }

    if (
      aHandlerApp instanceof Ci.nsIWebHandlerApp &&
      aHandlerApp.uriTemplate &&
      this.uriTemplate &&
      aHandlerApp.uriTemplate == this.uriTemplate
    ) {
      return true;
    }
    return false;
  },

  launchWithURI(aURI, aBrowsingContext) {

    let { scheme } = aURI;
    if (scheme == "ftp" || scheme == "ftps" || scheme == "sftp") {
      aURI = aURI.mutate().setUserPass("").finalize();
    }

    var escapedUriSpecToHandle = encodeURIComponent(aURI.spec);

    var uriToSend = Services.io.newURI(
      this.uriTemplate.replace("%s", escapedUriSpecToHandle)
    );

    if (!scheme.startsWith("web+")) {
      if (aBrowsingContext && aBrowsingContext != aBrowsingContext.top) {
        aBrowsingContext = null;
      }

      if (aBrowsingContext?.top.isAppTab) {
        let docURI = aBrowsingContext.currentWindowGlobal.documentURI;
        let docHost, sendHost;

        try {
          docHost = docURI?.host;
          sendHost = uriToSend?.host;
        } catch (e) {}

        if (docHost?.startsWith("www.")) {
          docHost = docHost.replace(/^www\./, "");
        }
        if (sendHost?.startsWith("www.")) {
          sendHost = sendHost.replace(/^www\./, "");
        }

        if (docHost != sendHost) {
          aBrowsingContext = null;
        }
      }
    }

    if (aBrowsingContext) {
      let triggeringPrincipal =
        Services.scriptSecurityManager.getSystemPrincipal();
      Services.tm.dispatchToMainThread(() =>
        aBrowsingContext.loadURI(uriToSend, { triggeringPrincipal })
      );
      return;
    }

    const windowType =
      AppConstants.MOZ_APP_NAME == "thunderbird"
        ? "mail:3pane"
        : "navigator:browser";
    let win = Services.wm.getMostRecentWindow(windowType);


    win.browserDOMWindow.openURI(
      uriToSend,
      null, 
      Ci.nsIBrowserDOMWindow.OPEN_DEFAULTWINDOW,
      Ci.nsIBrowserDOMWindow.OPEN_NEW,
      Services.scriptSecurityManager.getSystemPrincipal()
    );
  },


  get uriTemplate() {
    return this._uriTemplate;
  },

  set uriTemplate(aURITemplate) {
    this._uriTemplate = aURITemplate;
  },
};
