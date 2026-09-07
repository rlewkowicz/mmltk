/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "streamConv",
  "@mozilla.org/streamConverters;1",
  Ci.nsIStreamConverterService
);
export function HttpIndexViewer() {}

HttpIndexViewer.prototype = {
  QueryInterface: ChromeUtils.generateQI(["nsIDocumentLoaderFactory"]),

  createInstance(
    aCommand,
    aChannel,
    aLoadGroup,
    aContentType,
    aContainer,
    aExtraInfo,
    aDocListenerResult
  ) {

    let uri = aChannel.URI;
    if (uri instanceof Ci.nsINestedURI) {
      uri = uri.QueryInterface(Ci.nsINestedURI).innermostURI;
    }

    const allowedSchemes = Services.prefs.getStringPref(
      "network.http_index_format.allowed_schemes",
      ""
    );
    let isFile =
      allowedSchemes === "*" || allowedSchemes.split(",").some(uri.schemeIs);
    let contentType = isFile ? "text/html" : "text/plain";

    aChannel.contentType = contentType;

    let factory = Cc[
      "@mozilla.org/content/document-loader-factory;1"
    ].getService(Ci.nsIDocumentLoaderFactory);

    let listener = {};
    let res = factory.createInstance(
      "view",
      aChannel,
      aLoadGroup,
      contentType,
      aContainer,
      aExtraInfo,
      listener
    );

    if (isFile) {
      aDocListenerResult.value = lazy.streamConv.asyncConvertData(
        "application/http-index-format",
        "text/html",
        listener.value,
        null
      );
    } else {
      aDocListenerResult.value = listener.value;
      aChannel.loadInfo.browsingContext.window.console.warn(
        "application/http-index-format is deprecated, content will display as plain text"
      );
    }

    return res;
  },
};
