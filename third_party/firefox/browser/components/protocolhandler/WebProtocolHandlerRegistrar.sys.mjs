/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const STRING_BUNDLE_URI = "chrome://browser/locale/feeds/subscribe.properties";

export function WebProtocolHandlerRegistrar() {}

const lazy = {};

XPCOMUtils.defineLazyServiceGetters(lazy, {
  ExternalProtocolService: [
    "@mozilla.org/uriloader/external-protocol-service;1",
    Ci.nsIExternalProtocolService,
  ],
});

WebProtocolHandlerRegistrar.prototype = {
  get stringBundle() {
    let sb = Services.strings.createBundle(STRING_BUNDLE_URI);
    delete WebProtocolHandlerRegistrar.prototype.stringBundle;
    return (WebProtocolHandlerRegistrar.prototype.stringBundle = sb);
  },

  _getFormattedString(key, params) {
    return this.stringBundle.formatStringFromName(key, params);
  },

  _getString(key) {
    return this.stringBundle.GetStringFromName(key);
  },

  removeProtocolHandler(aProtocol, aURITemplate) {
    let handlerInfo =
      lazy.ExternalProtocolService.getProtocolHandlerInfo(aProtocol);
    let handlers = handlerInfo.possibleApplicationHandlers;
    for (let i = 0; i < handlers.length; i++) {
      try {
        let handler = handlers.queryElementAt(i, Ci.nsIWebHandlerApp);
        if (handler.uriTemplate == aURITemplate) {
          handlers.removeElementAt(i);
          let hs = Cc["@mozilla.org/uriloader/handler-service;1"].getService(
            Ci.nsIHandlerService
          );
          hs.store(handlerInfo);
          return;
        }
      } catch (e) {
      }
    }
  },

  _protocolHandlerRegistered(aProtocol, aURITemplate) {
    let handlerInfo =
      lazy.ExternalProtocolService.getProtocolHandlerInfo(aProtocol);
    let handlers = handlerInfo.possibleApplicationHandlers;
    for (let handler of handlers.enumerate()) {
      if (
        handler instanceof Ci.nsIWebHandlerApp &&
        handler.uriTemplate == aURITemplate
      ) {
        return true;
      }
    }
    return false;
  },

  registerProtocolHandler(
    aProtocol,
    aURI,
    aTitle,
    aDocumentURI,
    aBrowserOrWindow
  ) {
    aProtocol = (aProtocol || "").toLowerCase();
    if (!aURI || !aDocumentURI) {
      return;
    }

    let browser = aBrowserOrWindow; 
    if (aBrowserOrWindow instanceof Ci.nsIDOMWindow) {
      let rootDocShell = aBrowserOrWindow.docShell.sameTypeRootTreeItem;
      browser = rootDocShell.QueryInterface(Ci.nsIDocShell).chromeEventHandler;
    }

    let browserWindow = browser.documentGlobal;
    try {
      browserWindow.navigator.checkProtocolHandlerAllowed(
        aProtocol,
        aURI,
        aDocumentURI
      );
    } catch (ex) {
      return;
    }

    if (this._protocolHandlerRegistered(aProtocol, aURI.spec)) {
      return;
    }

    let message = this._getFormattedString("addProtocolHandlerMessage", [
      aURI.host,
      aProtocol,
    ]);

    let notificationIcon = aURI.prePath + "/favicon.ico";
    let notificationValue = "Protocol Registration: " + aProtocol;
    let addButton = {
      label: this._getString("addProtocolHandlerAddButton"),
      accessKey: this._getString("addProtocolHandlerAddButtonAccesskey"),
      protocolInfo: { protocol: aProtocol, uri: aURI.spec, name: aTitle },
      primary: true,

      callback(aNotification, aButtonInfo) {
        let protocol = aButtonInfo.protocolInfo.protocol;
        let name = aButtonInfo.protocolInfo.name;

        let handler = Cc[
          "@mozilla.org/uriloader/web-handler-app;1"
        ].createInstance(Ci.nsIWebHandlerApp);
        handler.name = name;
        handler.uriTemplate = aButtonInfo.protocolInfo.uri;

        let handlerInfo =
          lazy.ExternalProtocolService.getProtocolHandlerInfo(protocol);
        handlerInfo.possibleApplicationHandlers.appendElement(handler);

        handlerInfo.alwaysAskBeforeHandling = true;

        let hs = Cc["@mozilla.org/uriloader/handler-service;1"].getService(
          Ci.nsIHandlerService
        );
        hs.store(handlerInfo);
      },
    };

    let notificationBox = browser.getTabBrowser().getNotificationBox(browser);

    if (notificationBox.getNotificationWithValue(notificationValue)) {
      return;
    }

    notificationBox.appendNotification(
      notificationValue,
      {
        label: message,
        image: notificationIcon,
        priority: notificationBox.PRIORITY_INFO_LOW,
      },
      [addButton]
    );
  },

  QueryInterface: ChromeUtils.generateQI(["nsIWebProtocolHandlerRegistrar"]),
};
