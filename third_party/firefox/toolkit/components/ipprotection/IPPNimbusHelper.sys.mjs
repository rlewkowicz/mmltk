/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  IPProtectionService:
    "moz-src:///toolkit/components/ipprotection/IPProtectionService.sys.mjs",
  NimbusFeatures: "resource://nimbus/ExperimentAPI.sys.mjs",
});
class IPPNimbusHelperSingleton {
  init() {}

  initOnStartupCompleted() {
    lazy.NimbusFeatures.ipProtection.onUpdate(
      lazy.IPProtectionService.updateState
    );
  }

  uninit() {
    lazy.NimbusFeatures.ipProtection.offUpdate(
      lazy.IPProtectionService.updateState
    );
  }

  get isEligible() {
    let inExperiment = lazy.NimbusFeatures.ipProtection.getEnrollmentMetadata();

    if (inExperiment) {
      lazy.NimbusFeatures.ipProtection.recordExposureEvent({
        once: true,
      });

      return inExperiment.branch !== "control";
    }

    return true;
  }
}

const IPPNimbusHelper = new IPPNimbusHelperSingleton();

export { IPPNimbusHelper };
