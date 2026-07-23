/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

function checkCert(aChannel, aAllowNonBuiltInCerts) {
  if (!aChannel.originalURI.schemeIs("https")) {
    return;
  }

  let secInfo = aChannel.securityInfo;
  if (!secInfo.serverCert) {
    const noCertErr = "No server certificate.";
    throw new Components.Exception(noCertErr, Cr.NS_ERROR_ABORT);
  }

  if (aAllowNonBuiltInCerts === true) {
    return;
  }

  if (!secInfo.isBuiltCertChainRootBuiltInRoot) {
    const certNotBuiltInErr = "Certificate issuer is not built-in.";
    throw new Components.Exception(certNotBuiltInErr, Cr.NS_ERROR_ABORT);
  }
}

function BadCertHandler(aAllowNonBuiltInCerts) {
  this.allowNonBuiltInCerts = aAllowNonBuiltInCerts;
}
BadCertHandler.prototype = {
  asyncOnChannelRedirect(oldChannel, newChannel, flags, callback) {
    if (this.allowNonBuiltInCerts) {
      callback.onRedirectVerifyCallback(Cr.NS_OK);
      return;
    }

    if (!(flags & Ci.nsIChannelEventSink.REDIRECT_INTERNAL)) {
      checkCert(oldChannel);
    }

    callback.onRedirectVerifyCallback(Cr.NS_OK);
  },

  getInterface(iid) {
    return this.QueryInterface(iid);
  },

  QueryInterface: ChromeUtils.generateQI([
    "nsIChannelEventSink",
    "nsIInterfaceRequestor",
  ]),
};

export var CertUtils = {
  BadCertHandler,
  checkCert,
};
