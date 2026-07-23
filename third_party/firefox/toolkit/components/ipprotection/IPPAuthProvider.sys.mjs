/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export class IPPAuthProvider extends EventTarget {
  get isReady() {
    return false;
  }

  get hasUpgraded() {
    return false;
  }

  get isEnrolling() {
    return false;
  }

  get maxBytes() {
    return null;
  }

  async checkForUpgrade() {}

  async enroll() {
    throw new Error("enroll() must be implemented by subclasses");
  }

  async aboutToStart() {
    return { error: "no_auth_provider" };
  }

  async fetchProxyPass(_abortSignal) {
    return { error: "no_auth_provider" };
  }

  async fetchProxyUsage(_abortSignal) {
    return null;
  }

  get excludedUrlPrefs() {
    return [];
  }
}
