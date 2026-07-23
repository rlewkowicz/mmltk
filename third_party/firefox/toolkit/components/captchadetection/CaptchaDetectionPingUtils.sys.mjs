/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};
ChromeUtils.defineLazyGetter(lazy, "console", () => {
  return console.createInstance({
    prefix: "CaptchaDetectionPingUtils",
    maxLogLevelPref: "captchadetection.loglevel",
  });
});

ChromeUtils.defineESModuleGetters(lazy, {
  AsyncShutdown: "resource://gre/modules/AsyncShutdown.sys.mjs",
});

const HAS_UNSUBMITTED_DATA_PREF = "captchadetection.hasUnsubmittedData";
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "hasUnsubmittedData",
  HAS_UNSUBMITTED_DATA_PREF,
  false
);

const SUBMISSION_INTERVAL_PREF = "captchadetection.submissionInterval";
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "submissionInterval",
  SUBMISSION_INTERVAL_PREF,
  Math.floor((24 * 60 * 60) / 1000)
);

const LAST_SUBMISSION_PREF = "captchadetection.lastSubmission";
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "lastSubmission",
  LAST_SUBMISSION_PREF,
  0
);

export class CaptchaDetectionPingUtils {
  static #setHasUnsubmittedDataFlag() {
    if (lazy.hasUnsubmittedData) {
      return;
    }

    Services.prefs.setBoolPref(HAS_UNSUBMITTED_DATA_PREF, true);
    CaptchaDetectionPingUtils.#setPrivacyMetrics();
  }

  static #setPrivacyMetrics() {
    lazy.console.debug("Setting privacy metrics.");
    for (const [metricName, { type, name }] of Object.entries(
      CaptchaDetectionPingUtils.prefsOfInterest
    )) {
    }
  }

  static i32SafeDate() {
    return Math.floor(Date.now() / 1000 / 1000);
  }

  static flushPing(_subject, topic, prefName) {
    if (
      topic === "nsPref:changed" &&
      !Object.values(CaptchaDetectionPingUtils.prefsOfInterest).some(
        pref => pref.name === prefName
      )
    ) {
      lazy.console.debug("Pref that changed is not of interest for the ping.");
      return;
    }

    if (!lazy.hasUnsubmittedData) {
      lazy.console.debug("No unsubmitted data to submit.");
      return;
    }

    if (!CaptchaDetectionPingUtils.profileIsOpen) {
      lazy.console.debug(
        "Not submitting ping because profile is closing or already closed."
      );
      return;
    }

    lazy.console.debug("Flushing ping.");

    lazy.console.debug("Setting unsubmitted data flag to false.");
    Services.prefs.setBoolPref(HAS_UNSUBMITTED_DATA_PREF, false);

    lazy.console.debug("Setting lastSubmission to now.");
    Services.prefs.setIntPref(
      LAST_SUBMISSION_PREF,
      CaptchaDetectionPingUtils.i32SafeDate()
    );
  }

  static maybeSubmitPing(setHasUnsubmittedDataFlag = true) {
    if (!CaptchaDetectionPingUtils.profileIsOpen) {
      lazy.console.debug(
        "Not submitting ping because profile is closing or already closed."
      );
      return;
    }

    if (setHasUnsubmittedDataFlag) {
      CaptchaDetectionPingUtils.#setHasUnsubmittedDataFlag();
    }

    const lastSubmission = lazy.lastSubmission;
    if (lastSubmission === 0) {
      lazy.console.debug("Setting lastSubmission to now.");
      Services.prefs.setIntPref(
        LAST_SUBMISSION_PREF,
        CaptchaDetectionPingUtils.i32SafeDate()
      );
      return;
    }

    if (
      lastSubmission >
      CaptchaDetectionPingUtils.i32SafeDate() - lazy.submissionInterval
    ) {
      lazy.console.debug("Not enough time has passed since last submission.");
      return;
    }

    CaptchaDetectionPingUtils.flushPing();
  }

  static prefsOfInterest = {
    networkCookieCookiebehavior: {
      type: "Int",
      name: "network.cookie.cookieBehavior",
    },
    privacyTrackingprotectionEnabled: {
      type: "Bool",
      name: "privacy.trackingprotection.enabled",
    },
    privacyTrackingprotectionCryptominingEnabled: {
      type: "Bool",
      name: "privacy.trackingprotection.cryptomining.enabled",
    },
    privacyTrackingprotectionFingerprintingEnabled: {
      type: "Bool",
      name: "privacy.trackingprotection.fingerprinting.enabled",
    },
    privacyFingerprintingprotection: {
      type: "Bool",
      name: "privacy.fingerprintingProtection",
    },
    networkCookieCookiebehaviorOptinpartitioning: {
      type: "Bool",
      name: "network.cookie.cookieBehavior.optInPartitioning",
    },
    privacyResistfingerprinting: {
      type: "Bool",
      name: "privacy.resistFingerprinting",
    },
    privacyTrackingprotectionPbmEnabled: {
      type: "Bool",
      name: "privacy.trackingprotection.pbmode.enabled",
    },
    privacyFingerprintingprotectionPbm: {
      type: "Bool",
      name: "privacy.fingerprintingProtection.pbmode",
    },
    networkCookieCookiebehaviorOptinpartitioningPbm: {
      type: "Bool",
      name: "network.cookie.cookieBehavior.optInPartitioning.pbmode",
    },
    privacyResistfingerprintingPbmode: {
      type: "Bool",
      name: "privacy.resistFingerprinting.pbmode",
    },
  };

  static initialized = false;
  static profileIsOpen = true;

  static init() {
    if (CaptchaDetectionPingUtils.initialized) {
      return;
    }

    Object.values(CaptchaDetectionPingUtils.prefsOfInterest).forEach(pref => {
      Services.prefs.addObserver(
        pref.name,
        CaptchaDetectionPingUtils.flushPing
      );
    });

    if (!false) {
      ChromeUtils.idleDispatch(() =>
        CaptchaDetectionPingUtils.maybeSubmitPing(false)
      );
    }

    this.initialized = true;
    try {
      lazy.AsyncShutdown.profileBeforeChange.addBlocker(
        "CaptchaDetectionPingUtils: Don't submit pings after shutdown",
        async () => {
          this.profileIsOpen = false;
        }
      );
    } catch (e) {
      lazy.console.error(
        "Failed to add blocker for profileBeforeChange: " + e.message
      );
      this.profileIsOpen = false;
    }
  }
}

