/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

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

export class ServiceRequest extends XMLHttpRequest {
  constructor(options) {
    super(options);
  }
  open(method, url, options) {
    super.open(method, url, true);

    if (super.channel instanceof Ci.nsIHttpChannelInternal) {
      let internal = super.channel.QueryInterface(Ci.nsIHttpChannelInternal);
      internal.beConservative = true;
      if (options?.bypassProxy && this.bypassProxyEnabled) {
        internal.bypassProxy = true;
      }
    }
  }

  get bypassProxy() {
    let { channel } = this;
    return channel.QueryInterface(Ci.nsIHttpChannelInternal).bypassProxy;
  }

  get isProxied() {
    let { channel } = this;
    return !!(channel instanceof Ci.nsIProxiedChannel && channel.proxyInfo);
  }

  get bypassProxyEnabled() {
    return Services.prefs.getBoolPref("network.proxy.allow_bypass", true);
  }

  static get isOffline() {
    try {
      return (
        Services.io.offline ||
        lazy.CaptivePortalService.state ==
          lazy.CaptivePortalService.LOCKED_PORTAL ||
        !lazy.gNetworkLinkService.isLinkUp
      );
    } catch (ex) {
    }
    return false;
  }
}
