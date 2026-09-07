/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import {
  ContextIdCallback,
  ContextIdComponent,
} from "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustContextId.sys.mjs";

const CONTEXT_ID_PREF = "browser.contextual-services.contextId";
const CONTEXT_ID_TIMESTAMP_PREF =
  "browser.contextual-services.contextId.timestamp-in-seconds";
const CONTEXT_ID_ROTATION_DAYS_PREF =
  "browser.contextual-services.contextId.rotation-in-days";
const CONTEXT_ID_RUST_COMPONENT_ENABLED_PREF =
  "browser.contextual-services.contextId.rust-component.enabled";
const SHUTDOWN_TOPIC = "profile-before-change";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ObliviousHTTP: "resource://gre/modules/ObliviousHTTP.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "CURRENT_CONTEXT_ID",
  CONTEXT_ID_PREF,
  ""
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "UNIFIED_ADS_ENDPOINT",
  "browser.newtabpage.activity-stream.unifiedAds.endpoint",
  ""
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "OHTTP_RELAY_URL",
  "browser.newtabpage.activity-stream.discoverystream.ohttp.relayURL",
  ""
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "OHTTP_CONFIG_URL",
  "browser.newtabpage.activity-stream.discoverystream.ohttp.configURL",
  ""
);

class JsContextIdCallback extends ContextIdCallback {
  constructor(dispatchEvent) {
    super();
    this.dispatchEvent = dispatchEvent;
  }

  persist(newContextId, creationTimestamp) {
    Services.prefs.setCharPref(CONTEXT_ID_PREF, newContextId);
    Services.prefs.setIntPref(CONTEXT_ID_TIMESTAMP_PREF, creationTimestamp);
    this.dispatchEvent(new CustomEvent("ContextId:Persisted"));
  }

  rotated(oldContextId) {

    ContextId.sendMARSDeletionRequest(oldContextId);
  }
}

export class _ContextId extends EventTarget {
  #comp = null;
  #rotationDays = 0;
  #rustComponentEnabled = false;
  #observer = null;

  constructor() {
    super();

    this.#rustComponentEnabled = Services.prefs.getBoolPref(
      CONTEXT_ID_RUST_COMPONENT_ENABLED_PREF,
      false
    );

    if (this.#rustComponentEnabled) {
      this.#rotationDays = Services.prefs.getIntPref(
        CONTEXT_ID_ROTATION_DAYS_PREF,
        0
      );
      this.#comp = ContextIdComponent.init(
        lazy.CURRENT_CONTEXT_ID,
        Services.prefs.getIntPref(CONTEXT_ID_TIMESTAMP_PREF, 0),
        true ,
        new JsContextIdCallback(this.dispatchEvent.bind(this))
      );
      this.#observer = (subject, topic, data) => {
        this.observe(subject, topic, data);
      };

      Services.obs.addObserver(this.#observer, SHUTDOWN_TOPIC);
    }
  }

  observe(_subject, topic, _data) {
    if (topic == SHUTDOWN_TOPIC) {
      this.#comp.unsetCallback();
      Services.obs.removeObserver(this.#observer, SHUTDOWN_TOPIC);
    }
  }

  async request() {
    if (this.#rustComponentEnabled) {
      return this.#comp.request(this.#rotationDays);
    }

    if (!lazy.CURRENT_CONTEXT_ID) {
      let _contextId = Services.uuid.generateUUID().toString();
      Services.prefs.setStringPref(CONTEXT_ID_PREF, _contextId);
    }

    return Promise.resolve(lazy.CURRENT_CONTEXT_ID);
  }

  async forceRotation() {
    if (this.#rustComponentEnabled) {
      return this.#comp.forceRotation();
    }
    return Promise.resolve();
  }

  get rotationEnabled() {
    return this.#rustComponentEnabled && this.#rotationDays > 0;
  }

  requestSynchronously() {
    if (this.rotationEnabled) {
      throw new Error(
        "Cannot request context ID synchronously when rotation is enabled."
      );
    }

    return lazy.CURRENT_CONTEXT_ID;
  }

  async sendMARSDeletionRequest(oldContextId) {
    if (
      !lazy.UNIFIED_ADS_ENDPOINT ||
      !lazy.OHTTP_RELAY_URL ||
      !lazy.OHTTP_CONFIG_URL
    ) {
      return;
    }

    const endpoint = `${lazy.UNIFIED_ADS_ENDPOINT}v1/delete_user`;
    const body = {
      context_id: oldContextId,
    };
    const headers = new Headers();
    headers.append("content-type", "application/json");

    const config = await lazy.ObliviousHTTP.getOHTTPConfig(
      lazy.OHTTP_CONFIG_URL
    );

    if (!config) {
      console.error(
        new Error(
          `OHTTP was configured for ${endpoint} but we couldn't fetch a valid config`
        )
      );
    }

    const controller = new AbortController();
    const { signal } = controller;

    const response = await lazy.ObliviousHTTP.ohttpRequest(
      lazy.OHTTP_RELAY_URL,
      config,
      endpoint,
      {
        method: "DELETE",
        headers,
        body: JSON.stringify(body),
        credentials: "omit",
        signal,
      }
    );

    if (!response.ok) {
      console.error(new Error(`Unexpected status (${response.status})`));
    }
  }
}

export const ContextId = new _ContextId();
