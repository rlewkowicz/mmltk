/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

const LOG_PREF = "browser.ipProtection.log";

ChromeUtils.defineLazyGetter(lazy, "logConsole", function () {
  return console.createInstance({
    prefix: "IPPNetworkUtils",
    maxLogLevel: Services.prefs.getBoolPref(LOG_PREF, false) ? "Debug" : "Warn",
  });
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "CaptivePortalService",
  "@mozilla.org/network/captive-portal-service;1",
  Ci.nsICaptivePortalService
);

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "gNetworkLinkService",
  "@mozilla.org/network/network-link-service;1",
  Ci.nsINetworkLinkService
);

export const IPPNetworkUtils = {
  get isOffline() {
    try {
      return (
        Services.io.offline ||
        lazy.CaptivePortalService.state ==
          lazy.CaptivePortalService.LOCKED_PORTAL ||
        !lazy.gNetworkLinkService.isLinkUp
      );
    } catch (e) {
      lazy.logConsole.warn("Could not determine network status.", e);
    }
    return false;
  },
};
