/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

export class ChromePushSubscription {
  #props;

  constructor(props) {
    this.#props = props;
  }

  QueryInterface = ChromeUtils.generateQI(["nsIPushSubscription"]);

  get endpoint() {
    return this.#props.endpoint;
  }

  get lastPush() {
    return this.#props.lastPush;
  }

  get pushCount() {
    return this.#props.pushCount;
  }

  get quota() {
    return this.#props.quota;
  }

  get isSystemSubscription() {
    return !!this.#props.systemRecord;
  }

  get p256dhPrivateKey() {
    return this.#props.p256dhPrivateKey;
  }

  quotaApplies() {
    return this.quota >= 0;
  }

  isExpired() {
    return this.quota === 0;
  }

  getKey(name) {
    switch (name) {
      case "p256dh":
        return this.#getRawKey(this.#props.p256dhKey);

      case "auth":
        return this.#getRawKey(this.#props.authenticationSecret);

      case "appServer":
        return this.#getRawKey(this.#props.appServerKey);
    }
    return [];
  }

  #getRawKey(key) {
    if (!key) {
      return [];
    }
    return new Uint8Array(key);
  }
}
