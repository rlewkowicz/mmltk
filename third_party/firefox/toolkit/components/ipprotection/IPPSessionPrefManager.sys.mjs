/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  IPPProxyStates:
    "moz-src:///toolkit/components/ipprotection/IPPProxyManager.sys.mjs",
  IPPProxyManager:
    "moz-src:///toolkit/components/ipprotection/IPPProxyManager.sys.mjs",
  Preferences: "resource://gre/modules/Preferences.sys.mjs",
});

export class IPPSessionPrefManagerClass {
  #active = false;
  #changedPrefs = new Map();
  #observedPrefs;

  static getPrefs() {
    return [["media.peerconnection.ice.proxy_only_if_behind_proxy", true]];
  }
  init() {}

  initOnStartupCompleted() {
    lazy.IPPProxyManager.addEventListener(
      "IPPProxyManager:StateChanged",
      this.#handleStateChange
    );
  }

  uninit() {
    lazy.IPPProxyManager.removeEventListener(
      "IPPProxyManager:StateChanged",
      this.#handleStateChange
    );
    this.stop();
  }

  #handleStateChange = () => {
    if (lazy.IPPProxyManager.state === lazy.IPPProxyStates.ACTIVE) {
      this.start();
      return;
    }
    this.stop();
  };

  start() {
    if (this.#active) {
      return;
    }
    this.#active = true;
    for (let [prefName, prefValue] of this.#observedPrefs) {
      if (lazy.Preferences.isSet(prefName)) {
        continue;
      }
      lazy.Preferences.set(prefName, prefValue);
      const callback = () => {
        this.#changedPrefs.delete(prefName);
        lazy.Preferences.ignore(prefName, callback);
      };
      this.#changedPrefs.set(prefName, callback);
      lazy.Preferences.observe(prefName, callback);
    }
  }
  stop() {
    if (!this.#active) {
      return;
    }
    this.#active = false;

    for (const [pref, callback] of this.#changedPrefs) {
      lazy.Preferences.reset(pref);
      lazy.Preferences.ignore(pref, callback);
    }
    this.#changedPrefs = new Map();
  }

  constructor(observedPrefs = IPPSessionPrefManagerClass.getPrefs()) {
    this.#observedPrefs = observedPrefs;
  }
}

export const IPPSessionPrefManager = new IPPSessionPrefManagerClass();
