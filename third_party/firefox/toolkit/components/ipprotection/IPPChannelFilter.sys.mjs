/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  IPPExceptionsManager:
    "moz-src:///toolkit/components/ipprotection/IPPExceptionsManager.sys.mjs",
  IPPPrincipalRules:
    "moz-src:///toolkit/components/ipprotection/IPPExceptionsManager.sys.mjs",
  ProxyService: {
    service: "@mozilla.org/network/protocol-proxy-service;1",
    iid: Ci.nsIProtocolProxyService,
  },
});
const { TRANSPARENT_PROXY_RESOLVES_HOST, ALWAYS_TUNNEL_VIA_PROXY } =
  Ci.nsIProxyInfo;
const failOverTimeout = 10; 

const MODE_PREF = "browser.ipProtection.mode";

export const IPPMode = Object.freeze({
  MODE_FULL: 0,
  MODE_PB: 1,
  MODE_TRACKER: 2,
  MODE_INCLUSION: 3,
});

const TRACKING_FLAGS =
  Ci.nsIClassifiedChannel.CLASSIFIED_TRACKING |
  Ci.nsIClassifiedChannel.CLASSIFIED_TRACKING_AD |
  Ci.nsIClassifiedChannel.CLASSIFIED_TRACKING_ANALYTICS |
  Ci.nsIClassifiedChannel.CLASSIFIED_TRACKING_SOCIAL |
  Ci.nsIClassifiedChannel.CLASSIFIED_TRACKING_CONTENT;

export class IPPChannelFilter {
  static create() {
    return new IPPChannelFilter();
  }

  static setMode(mode) {
    Services.prefs.setIntPref(MODE_PREF, mode);
  }

  static constructProxyInfo(
    authToken,
    isolationKey,
    protocol,
    fallBackInfo = null,
    alwaysTunnel = false
  ) {
    switch (protocol.name) {
      case "masque":
        return lazy.ProxyService.newMASQUEProxyInfo(
          protocol.host,
          protocol.port,
          protocol.templateString,
          authToken,
          isolationKey,
          TRANSPARENT_PROXY_RESOLVES_HOST,
          failOverTimeout,
          fallBackInfo
        );
      case "connect": {
        const flags =
          TRANSPARENT_PROXY_RESOLVES_HOST |
          (alwaysTunnel ? ALWAYS_TUNNEL_VIA_PROXY : 0);
        return lazy.ProxyService.newProxyInfo(
          protocol.scheme,
          protocol.host,
          protocol.port,
          authToken,
          isolationKey,
          flags,
          failOverTimeout,
          fallBackInfo
        );
      }
      default:
        throw new Error(
          "Cannot construct ProxyInfo for Unknown server-protocol: " +
            protocol.name
        );
    }
  }
  static serverToProxyInfo(
    authToken,
    server,
    isolationKey = IPPChannelFilter.makeIsolationKey()
  ) {
    const alwaysTunnel = !false;
    return server.protocols.reduceRight((fallBackInfo, protocol) => {
      return IPPChannelFilter.constructProxyInfo(
        authToken,
        isolationKey,
        protocol,
        fallBackInfo,
        alwaysTunnel
      );
    }, null);
  }

  initialize(pass, server) {
    if (this.proxyInfo) {
      throw new Error("Double initialization?!?");
    }
    this.#pass = pass;
    this.#server = server;
    this.#setProxyInfo(IPPChannelFilter.makeIsolationKey());
  }

  #setProxyInfo(isolationKey) {
    this.#isolationKey = isolationKey;
    const proxyInfo = IPPChannelFilter.serverToProxyInfo(
      this.#pass.asBearerToken(),
      this.#server,
      isolationKey
    );
    Object.freeze(proxyInfo);
    this.proxyInfo = proxyInfo;
    this.#processPendingChannels();
  }

  constructor() {
    XPCOMUtils.defineLazyPreferenceGetter(
      this,
      "mode",
      MODE_PREF,
      IPPMode.MODE_FULL
    );
  }

  applyFilter(channel, defaultProxyInfo, proxyFilter) {
    if (!this.shouldProxy(channel)) {
      proxyFilter.onProxyFilterResult(defaultProxyInfo);
      return;
    }

    if (!this.proxyInfo) {
      this.#pendingChannels.push({ channel, proxyFilter });
      return;
    }

    proxyFilter.onProxyFilterResult(this.proxyInfo);

    this.#observers.forEach(observer => {
      observer(channel);
    });
  }

  shouldProxy(channel) {
    if (
      !this.proxyInfo &&
      !channel.isDocument &&
      channel.loadInfo?.triggeringPrincipal?.isSystemPrincipal
    ) {
      return false;
    }

    if (
      channel instanceof Ci.nsIHttpChannelInternal &&
      channel.isTRRServiceChannel
    ) {
      return false;
    }

    const principal = this.#principalForChannel(channel);
    const rule = lazy.IPPExceptionsManager.getPrincipalRule(principal);
    if (rule === lazy.IPPPrincipalRules.INCLUDED) {
      return true;
    }
    if (rule === lazy.IPPPrincipalRules.EXCLUDED) {
      return false;
    }
    return this.#matchMode(channel);
  }

  #principalForChannel(channel) {
    let { loadingPrincipal, triggeringPrincipal } = channel.loadInfo ?? {};
    if (loadingPrincipal && !loadingPrincipal.isSystemPrincipal) {
      return loadingPrincipal;
    }
    if (
      channel.loadInfo?.isUserTriggeredSave &&
      triggeringPrincipal &&
      !triggeringPrincipal.isSystemPrincipal
    ) {
      return triggeringPrincipal;
    }
    return Services.scriptSecurityManager.getChannelURIPrincipal(channel);
  }

  #matchMode(channel) {
    switch (this.mode) {
      case IPPMode.MODE_PB:
        return !!channel.loadInfo.originAttributes.privateBrowsingId;

      case IPPMode.MODE_TRACKER:
        return !!(
          TRACKING_FLAGS &
          channel.loadInfo.triggeringThirdPartyClassificationFlags
        );
      case IPPMode.MODE_INCLUSION:
        return false;
      case IPPMode.MODE_FULL:
      default:
        return true;
    }
  }

  start() {
    lazy.ProxyService.registerChannelFilter(
      this ,
      0 
    );
    this.#active = true;
  }

  stop() {
    if (!this.#active) {
      return;
    }

    lazy.ProxyService.unregisterChannelFilter(this);

    this.abortPendingChannels();

    this.#active = false;
    this.#abort.abort();
  }

  get isolationKey() {
    if (!this.proxyInfo) {
      return null;
    }
    return this.proxyInfo.connectionIsolationKey;
  }

  get hasPendingChannels() {
    return !!this.#pendingChannels.length;
  }

  suspend() {
    this.proxyInfo = null;
  }

  get canResume() {
    return !!this.#pass?.isValid() && !this.#pass.shouldRotate();
  }

  resume() {
    this.#setProxyInfo(this.#isolationKey);
  }

  replaceAuthTokenAndResume(pass) {
    this.#pass = pass;
    this.#setProxyInfo(IPPChannelFilter.makeIsolationKey());
  }

  async *proxiedChannels() {
    const stop = Promise.withResolvers();
    this.#abort.signal.addEventListener(
      "abort",
      () => {
        stop.reject();
      },
      { once: true }
    );
    while (this.#active) {
      const { promise, resolve } = Promise.withResolvers();
      this.#observers.push(resolve);
      try {
        const result = await Promise.race([stop.promise, promise]);
        this.#observers = this.#observers.filter(
          observer => observer !== resolve
        );
        yield result;
      } catch (error) {
        return;
      }
    }
  }

  get active() {
    return this.#active;
  }

  #processPendingChannels() {
    if (this.#pendingChannels.length) {
      this.#pendingChannels.forEach(data =>
        this.applyFilter(data.channel, null, data.proxyFilter)
      );
      this.#pendingChannels = [];
    }
  }

  abortPendingChannels() {
    if (this.#pendingChannels.length) {
      this.#pendingChannels.forEach(data =>
        data.channel.cancel(Cr.NS_BINDING_ABORTED)
      );
      this.#pendingChannels = [];
    }
  }

  #abort = new AbortController();
  #observers = [];
  #active = false;
  #pendingChannels = [];
  #server = null;
  #pass = null;
  #isolationKey = null;

  static makeIsolationKey() {
    return Math.random().toString(36).slice(2, 18).padEnd(16, "0");
  }
}
