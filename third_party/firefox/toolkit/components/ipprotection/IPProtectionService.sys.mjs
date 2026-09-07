/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  IPPAuthProvider:
    "moz-src:///toolkit/components/ipprotection/IPPAuthProvider.sys.mjs",
  IPPNimbusHelper:
    "moz-src:///toolkit/components/ipprotection/IPPNimbusHelper.sys.mjs",
  IPPStartupCache:
    "moz-src:///toolkit/components/ipprotection/IPPStartupCache.sys.mjs",
});

const ENABLED_PREF = "browser.ipProtection.enabled";

export const IPProtectionStates = Object.freeze({
  UNINITIALIZED: "uninitialized",
  UNAVAILABLE: "unavailable",
  UNAUTHENTICATED: "unauthenticated",
  READY: "ready",
});

class IPProtectionServiceSingleton extends EventTarget {
  #state = IPProtectionStates.UNINITIALIZED;

  #helpers = [];
  #authProvider = new lazy.IPPAuthProvider();

  get state() {
    return this.#state;
  }

  constructor() {
    super();
    this.updateState = this.#updateState.bind(this);
    this.setState = this.#setState.bind(this);
  }

  setHelpers(helpers) {
    this.#helpers = helpers;
  }

  get authProvider() {
    return this.#authProvider;
  }

  setAuthProvider(authProvider) {
    this.#authProvider = authProvider;
  }

  async init() {
    if (
      this.#state !== IPProtectionStates.UNINITIALIZED ||
      !this.featureEnabled
    ) {
      return;
    }

    this.#helpers.forEach(helper => helper.init());

    this.#updateState();

    if (lazy.IPPStartupCache.isStartupCompleted) {
      this.initOnStartupCompleted();
    }
  }

  uninit() {
    if (this.#state === IPProtectionStates.UNINITIALIZED) {
      return;
    }
    this.#helpers.forEach(helper => helper.uninit());

    this.#setState(IPProtectionStates.UNINITIALIZED);
  }

  async initOnStartupCompleted() {
    await Promise.allSettled(
      this.#helpers.map(helper => helper.initOnStartupCompleted?.())
    );
  }

  #updateState() {
    this.#setState(this.#computeState());
  }

  #computeState() {
    if (!this.featureEnabled) {
      return IPProtectionStates.UNINITIALIZED;
    }

    if (!lazy.IPPStartupCache.isStartupCompleted) {
      return lazy.IPPStartupCache.state;
    }

    if (!lazy.IPPNimbusHelper.isEligible) {
      return IPProtectionStates.UNAVAILABLE;
    }

    if (!this.#authProvider.isReady) {
      return IPProtectionStates.UNAUTHENTICATED;
    }

    return IPProtectionStates.READY;
  }

  #setState(newState) {
    if (newState === this.#state) {
      return;
    }

    let prevState = this.#state;
    this.#state = newState;

    this.#stateChanged(newState, prevState);
  }

  #stateChanged(state, prevState) {
    this.dispatchEvent(
      new CustomEvent("IPProtectionService:StateChanged", {
        bubbles: true,
        composed: true,
        detail: {
          state,
          prevState,
        },
      })
    );
  }
}

const IPProtectionService = new IPProtectionServiceSingleton();

XPCOMUtils.defineLazyPreferenceGetter(
  IPProtectionService,
  "featureEnabled",
  ENABLED_PREF,
  false,
  (_pref, _oldVal, featureEnabled) => {
    if (Services.appinfo.OS === "Android") {
      return;
    }
    if (featureEnabled) {
      IPProtectionService.init();
    } else {
      IPProtectionService.uninit();
    }
  }
);

export { IPProtectionService };
