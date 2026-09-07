/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  IPPProxyManager:
    "moz-src:///toolkit/components/ipprotection/IPPProxyManager.sys.mjs",
  IPPProxyStates:
    "moz-src:///toolkit/components/ipprotection/IPPProxyManager.sys.mjs",
  IPProtectionService:
    "moz-src:///toolkit/components/ipprotection/IPProtectionService.sys.mjs",
  IPProtectionStates:
    "moz-src:///toolkit/components/ipprotection/IPProtectionService.sys.mjs",
});

export class IPPEarlyStartupFilter {
  #autoStartAndAtStartup = false;

  constructor(shouldActivate) {
    this.handleEvent = this.#handleEvent.bind(this);
    this.#autoStartAndAtStartup = shouldActivate();
  }

  init() {
    if (this.#autoStartAndAtStartup) {
      lazy.IPPProxyManager.createChannelFilter();

      lazy.IPProtectionService.addEventListener(
        "IPProtectionService:StateChanged",
        this.handleEvent
      );
      lazy.IPPProxyManager.addEventListener(
        "IPPProxyManager:StateChanged",
        this.handleEvent
      );
    }
  }

  initOnStartupCompleted() {}

  uninit() {
    if (this.#autoStartAndAtStartup) {
      this.#autoStartAndAtStartup = false;

      lazy.IPPProxyManager.removeEventListener(
        "IPPProxyManager:StateChanged",
        this.handleEvent
      );
      lazy.IPProtectionService.removeEventListener(
        "IPProtectionService:StateChanged",
        this.handleEvent
      );
    }
  }

  #cancelChannelFilter() {
    lazy.IPPProxyManager.cancelChannelFilter();
  }

  #handleEvent(_event) {
    switch (lazy.IPProtectionService.state) {
      case lazy.IPProtectionStates.UNAVAILABLE:
      case lazy.IPProtectionStates.UNAUTHENTICATED:
        this.#cancelChannelFilter();
        this.uninit();
        break;

      default:
        break;
    }

    if (lazy.IPPProxyManager.state === lazy.IPPProxyStates.ACTIVE) {
      this.uninit();
    }
  }
}
