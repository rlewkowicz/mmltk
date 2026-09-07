/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AsyncShutdown: "resource://gre/modules/AsyncShutdown.sys.mjs",
  ClientID: "resource://gre/modules/ClientID.sys.mjs",
  DoHConfigController: "moz-src:///toolkit/components/doh/DoHConfig.sys.mjs",
  EnrollmentType: "resource://nimbus/ExperimentAPI.sys.mjs",
  Heuristics: "moz-src:///toolkit/components/doh/DoHHeuristics.sys.mjs",
  NimbusFeatures: "resource://nimbus/ExperimentAPI.sys.mjs",
  clearTimeout: "resource://gre/modules/Timer.sys.mjs",
  setTimeout: "resource://gre/modules/Timer.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "kNetworkDebounceTimeout",
  "doh-rollout.network-debounce-timeout",
  1000
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "kHeuristicsThrottleTimeout",
  "doh-rollout.heuristics-throttle-timeout",
  15000
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "kHeuristicsRateLimit",
  "doh-rollout.heuristics-throttle-rate-limit",
  2
);

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "gCaptivePortalService",
  "@mozilla.org/network/captive-portal-service;1",
  Ci.nsICaptivePortalService
);

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "gNetworkLinkService",
  "@mozilla.org/network/network-link-service;1",
  Ci.nsINetworkLinkService
);

const FIRST_RUN_PREF = "doh-rollout.doneFirstRun";

const DISABLED_PREF = "doh-rollout.disable-heuristics";

const SKIP_HEURISTICS_PREF = "doh-rollout.skipHeuristicsCheck";

const CLEAR_ON_SHUTDOWN_PREF = "doh-rollout.clearModeOnShutdown";

const BREADCRUMB_PREF = "doh-rollout.self-enabled";

const NETWORK_TRR_MODE_PREF = "network.trr.mode";
const NETWORK_TRR_URI_PREF = "network.trr.uri";

const NETWORK_TRR_START_CONFIRMATION_FAILED_PREF =
  "network.trr.start-confirmation-in-failed-state";

const ANDROID_DOH_START_CONFIRMATION_FAILED_PREF =
  "network.android_doh.start_in_confirmation_failed_state";

const ROLLOUT_MODE_PREF = "doh-rollout.mode";
const ROLLOUT_URI_PREF = "doh-rollout.uri";

const TRR_SELECT_DRY_RUN_RESULT_PREF =
  "doh-rollout.trr-selection.dry-run-result";

const kLinkStatusChangedTopic = "network:link-status-changed";
const kConnectivityTopic = "network:captive-portal-connectivity-changed";
const kPrefChangedTopic = "nsPref:changed";

function getHashedNetworkID() {
  let currentNetworkID = lazy.gNetworkLinkService.networkID;
  if (!currentNetworkID) {
    return "";
  }

  let hasher = Cc["@mozilla.org/security/hash;1"].createInstance(
    Ci.nsICryptoHash
  );

  hasher.init(Ci.nsICryptoHash.SHA256);
  let clientNetworkID = lazy.ClientID.getClientID() + currentNetworkID;
  hasher.update(
    clientNetworkID.split("").map(c => c.charCodeAt(0)),
    clientNetworkID.length
  );
  return hasher.finish(true);
}

export const DoHController = {
  _heuristicsAreEnabled: false,

  async init() {
    await lazy.DoHConfigController.initComplete;

    Services.obs.addObserver(this, lazy.DoHConfigController.kConfigUpdateTopic);
    Services.prefs.addObserver(NETWORK_TRR_MODE_PREF, this);
    Services.prefs.addObserver(NETWORK_TRR_URI_PREF, this);

    if (lazy.DoHConfigController.currentConfig.enabled) {
      for (let key of lazy.Heuristics.Telemetry.heuristicNames()) {
      }

      await this.maybeEnableHeuristics();
    } else if (Services.prefs.getBoolPref(FIRST_RUN_PREF, false)) {
      await this.rollback();
    }

    this._asyncShutdownBlocker = async () => {
      await this.disableHeuristics("shutdown");
    };

    lazy.AsyncShutdown.profileBeforeChange.addBlocker(
      "DoHController: clear state and remove observers",
      this._asyncShutdownBlocker
    );

    Services.prefs.setBoolPref(FIRST_RUN_PREF, true);
  },

  async cleanupPrefs() {
    const branch = Services.prefs.getBranch("doh-rollout.");
    for (const pref of branch.getChildList("")) {
      branch.clearUserPref(pref);
    }
  },

  async _uninit() {
    Services.obs.removeObserver(
      this,
      lazy.DoHConfigController.kConfigUpdateTopic
    );
    Services.prefs.removeObserver(NETWORK_TRR_MODE_PREF, this);
    Services.prefs.removeObserver(NETWORK_TRR_URI_PREF, this);
    lazy.AsyncShutdown.profileBeforeChange.removeBlocker(
      this._asyncShutdownBlocker
    );
    await this.disableHeuristics("shutdown");
  },

  resetPromise: Promise.resolve(),
  async reset() {
    this.resetPromise = this.resetPromise.then(async () => {
      await this._uninit();
      await this.init();
      Services.obs.notifyObservers(null, "doh:controller-reloaded");
    });

    return this.resetPromise;
  },

  async maybeEnableHeuristics() {
    if (Services.prefs.getBoolPref(DISABLED_PREF, false)) {
      return;
    }

    let policyResult = await lazy.Heuristics.checkEnterprisePolicy();

    if (policyResult != "no_policy_set") {
      switch (policyResult) {
        case "policy_without_doh":
          await this.setState("policyDisabled");
          break;
        case "disable_doh":
          await this.setState("policyDisabled");
          break;
        case "enable_doh":
          break;
      }
      Services.prefs.setBoolPref(SKIP_HEURISTICS_PREF, true);
      return;
    }

    Services.prefs.clearUserPref(SKIP_HEURISTICS_PREF);

    if (
      Services.prefs.prefHasUserValue(NETWORK_TRR_MODE_PREF) ||
      Services.prefs.prefHasUserValue(NETWORK_TRR_URI_PREF)
    ) {
      await this.setState("manuallyDisabled");
      Services.prefs.setBoolPref(DISABLED_PREF, true);
      return;
    }

    await this.runTRRSelection();
    if (!Services.prefs.prefHasUserValue(ROLLOUT_URI_PREF)) {
      let uri = lazy.DoHConfigController.currentConfig.fallbackProviderURI;

      try {
        let ohttpURI = lazy.NimbusFeatures.dooh.getVariable("ohttpUri");
        if (ohttpURI) {
          uri = ohttpURI;
        }
      } catch (e) {
        console.error(`Error getting dooh.ohttpURI: ${e.message}`);
      }

      Services.prefs.setStringPref(ROLLOUT_URI_PREF, uri || "");
    }
    this.runHeuristicsThrottled("startup");
    Services.obs.addObserver(this, kLinkStatusChangedTopic);
    Services.obs.addObserver(this, kConnectivityTopic);

    this._heuristicsAreEnabled = true;
  },

  _runsWhileThrottling: 0,
  _wasThrottleExtended: false,
  _throttleHeuristics() {
    if (lazy.kHeuristicsThrottleTimeout < 0) {
      return false;
    }

    if (this._throttleTimer) {
      this._runsWhileThrottling++;
      return true;
    }

    this._runsWhileThrottling = 0;

    this._throttleTimer = lazy.setTimeout(
      this._handleThrottleTimeout.bind(this),
      lazy.kHeuristicsThrottleTimeout
    );

    return false;
  },

  _handleThrottleTimeout() {
    delete this._throttleTimer;
    if (this._runsWhileThrottling > lazy.kHeuristicsRateLimit) {
      this._wasThrottleExtended = true;
      this._throttleHeuristics();
      return;
    }

    if (this._runsWhileThrottling > 0 || this._wasThrottleExtended) {
      this.runHeuristicsThrottled("throttled");
    }

    this._wasThrottleExtended = false;

  },

  runHeuristicsThrottled(evaluateReason) {
    if (this._throttleHeuristics()) {
      return;
    }

    this.runHeuristics(evaluateReason);
  },

  async runHeuristics(evaluateReason) {
    let start = Date.now();

    let results = await lazy.Heuristics.run();

    if (
      !lazy.gNetworkLinkService.isLinkUp ||
      this._lastDebounceTimestamp > start ||
      lazy.gCaptivePortalService.state ==
        lazy.gCaptivePortalService.LOCKED_PORTAL
    ) {
      return;
    }

    let decision = Object.values(results).includes(lazy.Heuristics.DISABLE_DOH)
      ? lazy.Heuristics.DISABLE_DOH
      : lazy.Heuristics.ENABLE_DOH;

    let getCaptiveStateString = () => {
      switch (lazy.gCaptivePortalService.state) {
        case lazy.gCaptivePortalService.NOT_CAPTIVE:
          return "not_captive";
        case lazy.gCaptivePortalService.UNLOCKED_PORTAL:
          return "unlocked";
        case lazy.gCaptivePortalService.LOCKED_PORTAL:
          return "locked";
        default:
          return "unknown";
      }
    };

    let resultsForTelemetry = {
      evaluateReason,
      steeredProvider: "",
      captiveState: getCaptiveStateString(),
      networkID: getHashedNetworkID(),
    };

    const oHTTPexperiment = lazy.NimbusFeatures.dooh.getEnrollmentMetadata(
      lazy.EnrollmentType.EXPERIMENT
    );

    if (results.steeredProvider && !oHTTPexperiment) {
      Services.dns.setDetectedTrrURI(results.steeredProvider.uri);
      resultsForTelemetry.steeredProvider = results.steeredProvider.id;
    }

    this.setHeuristicResult(Ci.nsITRRSkipReason.TRR_UNSET);
    if (decision === lazy.Heuristics.DISABLE_DOH) {
      await this.setState("disabled");
    } else {
      await this.setState("enabled");
    }

    let canaries = [];
    let filtering = [];
    let enterprise = [];
    let platform = [];

    for (let [heuristicName, result] of Object.entries(results)) {
      if (result !== lazy.Heuristics.DISABLE_DOH) {
        continue;
      }

      if (["canary", "zscalerCanary"].includes(heuristicName)) {
        canaries.push(heuristicName);
      } else if (
        ["browserParent", "google", "youtube"].includes(heuristicName)
      ) {
        filtering.push(heuristicName);
      } else if (
        ["policy", "modifiedRoots", "thirdPartyRoots"].includes(heuristicName)
      ) {
        enterprise.push(heuristicName);
      } else if (["vpn", "proxy", "nrpt"].includes(heuristicName)) {
        platform.push(heuristicName);
      }

      if (lazy.Heuristics.Telemetry.heuristicNames().includes(heuristicName)) {
      }
    }

    resultsForTelemetry.canaries = canaries.join(",");
    resultsForTelemetry.filtering = filtering.join(",");
    resultsForTelemetry.enterprise = enterprise.join(",");
    resultsForTelemetry.platform = platform.join(",");
    resultsForTelemetry.value = decision;

  },

  async setState(state) {
    switch (state) {
      case "disabled":
        Services.prefs.setIntPref(ROLLOUT_MODE_PREF, 0);
        break;
      case "enabled":
        if (
          Services.appinfo.OS === "Android" &&
          Services.prefs.getBoolPref(
            ANDROID_DOH_START_CONFIRMATION_FAILED_PREF,
            true
          )
        ) {
          Services.prefs.setBoolPref(
            NETWORK_TRR_START_CONFIRMATION_FAILED_PREF,
            true
          );
        }
        Services.prefs.setIntPref(ROLLOUT_MODE_PREF, 2);
        Services.prefs.setBoolPref(BREADCRUMB_PREF, true);
        break;
      case "policyDisabled":
      case "manuallyDisabled":
      case "UIDisabled":
        Services.prefs.clearUserPref(BREADCRUMB_PREF);
      case "rollback":
        this.setHeuristicResult(Ci.nsITRRSkipReason.TRR_UNSET);
        Services.prefs.clearUserPref(ROLLOUT_MODE_PREF);
        break;
      case "shutdown":
        this.setHeuristicResult(Ci.nsITRRSkipReason.TRR_UNSET);
        if (Services.prefs.getBoolPref(CLEAR_ON_SHUTDOWN_PREF, true)) {
          Services.prefs.clearUserPref(ROLLOUT_MODE_PREF);
        }
        break;
    }


    let modePref = Services.prefs.getIntPref(NETWORK_TRR_MODE_PREF, 0);
    if (state == "manuallyDisabled") {
      if (
        modePref == Ci.nsIDNSService.MODE_TRRFIRST ||
        modePref == Ci.nsIDNSService.MODE_TRRONLY
      ) {
      } else if (
        Services.prefs.getStringPref("doh-rollout.doorhanger-decision", "") ==
        "UIDisabled"
      ) {
      } else {
      }
    }
  },

  async disableHeuristics(state) {
    await this.setState(state);

    if (!this._heuristicsAreEnabled) {
      return;
    }

    Services.obs.removeObserver(this, kLinkStatusChangedTopic);
    Services.obs.removeObserver(this, kConnectivityTopic);
    if (this._debounceTimer) {
      lazy.clearTimeout(this._debounceTimer);
      delete this._debounceTimer;
    }
    if (this._throttleTimer) {
      lazy.clearTimeout(this._throttleTimer);
      delete this._throttleTimer;
    }
    this._heuristicsAreEnabled = false;
  },

  async rollback() {
    await this.disableHeuristics("rollback");
  },

  async runTRRSelection() {
    if (!lazy.DoHConfigController.currentConfig.trrSelection.commitResult) {
      Services.prefs.clearUserPref(ROLLOUT_URI_PREF);
    }

    if (!lazy.DoHConfigController.currentConfig.trrSelection.enabled) {
      return;
    }

    if (
      Services.prefs.prefHasUserValue(ROLLOUT_URI_PREF) &&
      Services.prefs.getStringPref(ROLLOUT_URI_PREF, "") ==
        Services.prefs.getStringPref(TRR_SELECT_DRY_RUN_RESULT_PREF, "")
    ) {
      return;
    }

    await this.runTRRSelectionDryRun();

    if (!lazy.DoHConfigController.currentConfig.trrSelection.commitResult) {
      return;
    }

    Services.prefs.setStringPref(
      ROLLOUT_URI_PREF,
      Services.prefs.getStringPref(TRR_SELECT_DRY_RUN_RESULT_PREF, "")
    );
  },

  async runTRRSelectionDryRun() {
    if (Services.prefs.prefHasUserValue(TRR_SELECT_DRY_RUN_RESULT_PREF)) {
      let dryRunResult = Services.prefs.getStringPref(
        TRR_SELECT_DRY_RUN_RESULT_PREF,
        ""
      );
      let dryRunResultIsValid =
        lazy.DoHConfigController.currentConfig.providerList.some(
          trr => trr.uri == dryRunResult
        );
      if (dryRunResultIsValid) {
        return;
      }
    }

    let setDryRunResultAndRecordTelemetry = trrUri => {
      Services.prefs.setStringPref(TRR_SELECT_DRY_RUN_RESULT_PREF, trrUri);
    };

    let { TRRRacer } = ChromeUtils.importESModule(
      "moz-src:///toolkit/components/doh/TRRPerformance.sys.mjs"
    );
    await new Promise(resolve => {
      let trrList =
        lazy.DoHConfigController.currentConfig.trrSelection.providerList.map(
          trr => trr.uri
        );
      let racer = new TRRRacer(() => {
        setDryRunResultAndRecordTelemetry(racer.getFastestTRR(true));
        resolve();
      }, trrList);
      racer.run();
    });
  },

  observe(subject, topic, data) {
    switch (topic) {
      case kLinkStatusChangedTopic:
        this.onConnectionChanged();
        break;
      case kConnectivityTopic:
        this.onConnectivityAvailable();
        break;
      case kPrefChangedTopic:
        this.onPrefChanged(data);
        break;
      case lazy.DoHConfigController.kConfigUpdateTopic:
        this.reset();
        break;
    }
  },

  setHeuristicResult(skipReason) {
    try {
      Services.dns.setHeuristicDetectionResult(skipReason);
    } catch (e) {}
  },

  async onPrefChanged(pref) {
    switch (pref) {
      case NETWORK_TRR_URI_PREF:
      case NETWORK_TRR_MODE_PREF:
        Services.prefs.setBoolPref(DISABLED_PREF, true);
        await this.disableHeuristics("manuallyDisabled");
        break;
    }
  },

  _debounceTimer: null,
  _cancelDebounce() {
    if (!this._debounceTimer) {
      return;
    }

    lazy.clearTimeout(this._debounceTimer);
    this._debounceTimer = null;
  },

  _lastDebounceTimestamp: 0,
  onConnectionChanged() {
    if (!lazy.gNetworkLinkService.isLinkUp) {
      this._cancelDebounce();
      return;
    }

    if (this._debounceTimer) {
      return;
    }

    if (lazy.kNetworkDebounceTimeout < 0) {
      this.onConnectionChangedDebounced();
      return;
    }

    this._lastDebounceTimestamp = Date.now();
    this._debounceTimer = lazy.setTimeout(() => {
      this._cancelDebounce();
      this.onConnectionChangedDebounced();
    }, lazy.kNetworkDebounceTimeout);
  },

  onConnectionChangedDebounced() {
    if (!lazy.gNetworkLinkService.isLinkUp) {
      return;
    }

    if (
      lazy.gCaptivePortalService.state ==
      lazy.gCaptivePortalService.LOCKED_PORTAL
    ) {
      return;
    }

    this.runHeuristicsThrottled("netchange");
  },

  onConnectivityAvailable() {
    if (this._debounceTimer) {
      return;
    }

    this.runHeuristicsThrottled("connectivity");
  },
};
