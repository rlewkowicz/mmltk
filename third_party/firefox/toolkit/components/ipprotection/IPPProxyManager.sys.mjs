/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  IPPChannelFilter:
    "moz-src:///toolkit/components/ipprotection/IPPChannelFilter.sys.mjs",
  IPPNetworkUtils:
    "moz-src:///toolkit/components/ipprotection/IPPNetworkUtils.sys.mjs",
  IPPNetworkErrorObserver:
    "moz-src:///toolkit/components/ipprotection/IPPNetworkErrorObserver.sys.mjs",
  IPProtectionServerlist:
    "moz-src:///toolkit/components/ipprotection/IPProtectionServerlist.sys.mjs",
  IPProtectionService:
    "moz-src:///toolkit/components/ipprotection/IPProtectionService.sys.mjs",
  IPProtectionStates:
    "moz-src:///toolkit/components/ipprotection/IPProtectionService.sys.mjs",
  IPPStartupCache:
    "moz-src:///toolkit/components/ipprotection/IPPStartupCache.sys.mjs",
});

ChromeUtils.defineLazyGetter(
  lazy,
  "setTimeout",
  () =>
    ChromeUtils.importESModule("resource://gre/modules/Timer.sys.mjs")
      .setTimeout
);
ChromeUtils.defineLazyGetter(
  lazy,
  "clearTimeout",
  () =>
    ChromeUtils.importESModule("resource://gre/modules/Timer.sys.mjs")
      .clearTimeout
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "timeout",
  "browser.ipProtection.guardian.timeout",
  Temporal.Duration.from({ seconds: 30 }).total("milliseconds")
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "retryAfter",
  "browser.ipProtection.guardian.retryAfter",
  500
);

export const ERRORS = Object.freeze({
  GENERIC: "generic-error",
  NETWORK: "network-error",
  CATASTROPHIC: "catastrophic-error",
  TIMEOUT: "timeout-error", 
  MISSING_PROMISE: "missing-activation-promise", 
  MISSING_ABORT: "missing-abort-controller", 
  PASS_UNAVAILABLE: "pass-unavailable", 
  SERVER_NOT_FOUND: "server-not-found", 
  CANCELED: "activation-canceled", 
});

const LOG_PREF = "browser.ipProtection.log";

ChromeUtils.defineLazyGetter(lazy, "logConsole", function () {
  return console.createInstance({
    prefix: "IPPProxyManager",
    maxLogLevel: Services.prefs.getBoolPref(LOG_PREF, false) ? "Debug" : "Warn",
  });
});

export const IPPProxyStates = Object.freeze({
  NOT_READY: "not-ready",
  READY: "ready",
  ACTIVATING: "activating",
  ACTIVE: "active",
  ERROR: "error",
  PAUSED: "paused",
});

export async function scheduleCallback(
  callback,
  timepoint,
  abortSignal,
  imports = lazy
) {
  const getNow = imports.getNow || (() => Temporal.Now.instant());
  while (getNow().until(timepoint).total("milliseconds") > 0) {
    if (abortSignal.aborted) {
      return;
    }
    const msUntilTrigger = getNow().until(timepoint).total("milliseconds");
    const clampedMs = Math.min(msUntilTrigger, 2147483647);
    await new Promise(resolve => {
      const timeoutId = imports.setTimeout(resolve, clampedMs);
      abortSignal.addEventListener(
        "abort",
        () => {
          imports.clearTimeout(timeoutId);
          resolve();
        },
        { once: true }
      );
    });
  }
  if (abortSignal.aborted) {
    return;
  }
  callback();
}

class IPPProxyManagerSingleton extends EventTarget {
  #state = IPPProxyStates.NOT_READY;

  #activatingPromise = null;
  #activationAbortController = null;

  #pass = null;
  #usage = null;
  #connection = null;
  #networkErrorObserver = null;
  #rotation = null;
  #activatedAt = 0;

  #rotationTimer = 0;
  #usageRefreshAbortController = null;
  #errorType = null;
  #refreshUsageAbortController = null;

  constructor() {
    super();

    this.setErrorState = this.#setErrorState.bind(this);
    this.handleProxyErrorEvent = this.#handleProxyErrorEvent.bind(this);
    this.handleEvent = this.#handleEvent.bind(this);
  }

  init() {
    lazy.IPProtectionService.addEventListener(
      "IPProtectionService:StateChanged",
      this.handleEvent
    );

    if (!this.#usage) {
      this.#usage = lazy.IPPStartupCache.usageInfo;
    }

    if (this.#usage && !this.#usage.unlimited) {
      this.#scheduleUsageCheck(this.#usage);
    }
  }

  initOnStartupCompleted() {}

  uninit() {
    lazy.IPProtectionService.removeEventListener(
      "IPProtectionService:StateChanged",
      this.handleEvent
    );

    if (
      this.#state === IPPProxyStates.ACTIVE ||
      this.#state === IPPProxyStates.ACTIVATING
    ) {
      this.stop(false);
    }
    if (this.#usageRefreshAbortController) {
      this.#usageRefreshAbortController.abort();
      this.#usageRefreshAbortController = null;
    }

    this.#pass = null;
    this.#usage = null;
    this.#connection = null;
  }

  get activatedAt() {
    return this.#state === IPPProxyStates.ACTIVE && this.#activatedAt;
  }

  get networkErrorObserver() {
    if (!this.#networkErrorObserver) {
      this.#networkErrorObserver = new lazy.IPPNetworkErrorObserver();
      this.#networkErrorObserver.addEventListener(
        "proxy-http-error",
        this.handleProxyErrorEvent
      );
    }
    return this.#networkErrorObserver;
  }

  get active() {
    return this.#connection?.active;
  }

  get isolationKey() {
    return this.#connection?.isolationKey;
  }

  get hasValidProxyPass() {
    return !!this.#pass?.isValid();
  }

  get usageInfo() {
    return this.#usage;
  }

  channelFilter() {
    return this.#connection;
  }

  createChannelFilter() {
    if (!this.#connection) {
      this.#connection = lazy.IPPChannelFilter.create();
      this.#connection.start();
    }
  }

  cancelChannelFilter() {
    if (this.#connection) {
      this.#connection.stop();
      this.#connection = null;
    }
  }

  get state() {
    return this.#state;
  }

  get errorType() {
    return this.#errorType;
  }

  async start(userAction = true, inPrivateBrowsing = false, country) {
    if (this.#state === IPPProxyStates.ACTIVATING) {
      if (!this.#activatingPromise) {
        throw new Error(ERRORS.MISSING_PROMISE);
      }

      return this.#activatingPromise;
    }

    if (
      this.#state === IPPProxyStates.NOT_READY ||
      this.#state === IPPProxyStates.ERROR ||
      this.#state === IPPProxyStates.PAUSED
    ) {
      return { started: false };
    }

    this.#activationAbortController = new AbortController();
    const abortSignal = this.#activationAbortController.signal;

    lazy.setTimeout(
      () => {
        this.#activationAbortController?.abort(ERRORS.TIMEOUT);
      },
      Temporal.Duration.from({ seconds: 30 }).total("milliseconds")
    );

    this.#setState(IPPProxyStates.ACTIVATING);

    const { promise: abortPromise, reject } = Promise.withResolvers();
    abortSignal.addEventListener(
      "abort",
      () => {
        reject(abortSignal.reason);
      },
      { once: true }
    );

    this.#activatingPromise = Promise.race([
      this.#startInternal(abortSignal, country),
      abortPromise,
    ])
      .then(
        started => {
          if (
            this.#state === IPPProxyStates.ERROR ||
            this.#state === IPPProxyStates.PAUSED
          ) {
            return { started: false };
          }
          if (!started) {
            this.cancelChannelFilter();
            this.updateState();
            return { started: false };
          }
          this.#setState(IPPProxyStates.ACTIVE);
          return { started: true };
        },
        error => {
          this.#activationAbortController = null;
          this.cancelChannelFilter();
          this.#setErrorState(error);
          return { started: false, error };
        }
      )
      .finally(() => {
        this.#activatingPromise = null;
        this.#activationAbortController = null;
      });
    return this.#activatingPromise;
  }

  async #startInternal(abortSignal, country) {
    if (lazy.IPPNetworkUtils.isOffline) {
      throw ERRORS.NETWORK;
    }

    await lazy.IPProtectionServerlist.maybeFetchList();

    const notReady = await lazy.IPProtectionService.authProvider.aboutToStart();
    if (notReady) {
      throw notReady.error || ERRORS.GENERIC;
    }

    if (abortSignal?.aborted) {
      return false;
    }

    this.createChannelFilter();

    if (this.#pass == null || this.#pass.shouldRotate()) {
      const { pass, usage, error, status } =
        await this.#getPassAndUsage(abortSignal);
      if (usage) {
        this.#setUsage(usage);
        if (usage.quotaExhausted) {
          this.#setPausedState();
          return false;
        }
      }

      if (error || !pass) {
        if (status === 500) {
          throw ERRORS.CATASTROPHIC;
        }
        throw ERRORS.PASS_UNAVAILABLE;
      }
      this.#pass = pass;
    }
    this.#schedulePassRotation(this.#pass);

    const location = country
      ? lazy.IPProtectionServerlist.getLocation(country)
      : lazy.IPProtectionServerlist.getRecommendedLocation();
    const server = lazy.IPProtectionServerlist.selectServer(location?.city);
    if (!server) {
      throw ERRORS.SERVER_NOT_FOUND;
    }

    lazy.logConsole.debug("Server:", server?.hostname);

    this.#connection.initialize(this.#pass, server);

    this.networkErrorObserver.start();
    this.networkErrorObserver.addIsolationKey(this.#connection.isolationKey);

    lazy.logConsole.info("Started");

    if (!!this.#connection?.active && !!this.#connection?.proxyInfo) {
      this.#activatedAt = ChromeUtils.now();
      return true;
    }

    return false;
  }

  async stop(userAction = true) {
    if (this.#state === IPPProxyStates.ACTIVATING) {
      if (!this.#activatingPromise) {
        throw new Error(ERRORS.MISSING_PROMISE);
      }
      if (!this.#activationAbortController) {
        throw new Error(ERRORS.MISSING_ABORT);
      }
      this.#activationAbortController?.abort(ERRORS.CANCELED);
      await this.#activatingPromise.then(() => this.stop(userAction));
      return;
    }

    if (
      this.#state !== IPPProxyStates.PAUSED &&
      this.#state !== IPPProxyStates.ACTIVE &&
      this.#state !== IPPProxyStates.ERROR
    ) {
      return;
    }

    if (this.#connection) {
      this.cancelChannelFilter();

      lazy.clearTimeout(this.#rotationTimer);
      this.#rotationTimer = 0;

      this.#rotation?.controller.abort();

      this.networkErrorObserver.stop();
    }

    lazy.logConsole.info("Stopped");

    const sessionLength = this.#activatedAt
      ? ChromeUtils.now() - this.#activatedAt
      : 0;

    this.updateState();
    if (
      userAction &&
      this.#state !== IPPProxyStates.PAUSED &&
      !this.#usage?.unlimited
    ) {
      this.refreshUsage();
    }
  }

  switch(country) {
    if (this.#state !== IPPProxyStates.ACTIVE) {
      return { switched: false };
    }

    const location = country
      ? lazy.IPProtectionServerlist.getLocation(country)
      : lazy.IPProtectionServerlist.getRecommendedLocation();
    const server = lazy.IPProtectionServerlist.selectServer(location?.city);

    if (!server) {
      this.#setErrorState(ERRORS.SERVER_NOT_FOUND);
      return { switched: false, error: ERRORS.SERVER_NOT_FOUND };
    }

    lazy.logConsole.debug("Switching to server:", server?.hostname);

    this.#connection.suspend();
    this.#connection.initialize(this.#pass, server);

    this.networkErrorObserver.addIsolationKey(this.#connection.isolationKey);

    return { switched: true };
  }

  async reset() {
    this.#refreshUsageAbortController?.abort();
    this.#rotation?.controller.abort();

    this.#pass = null;
    this.#usage = null;
    if (this.#usageRefreshAbortController) {
      this.#usageRefreshAbortController.abort();
      this.#usageRefreshAbortController = null;
    }
    lazy.IPPStartupCache.storeUsageInfo(null);
    if (
      this.#state === IPPProxyStates.ACTIVE ||
      this.#state === IPPProxyStates.ACTIVATING ||
      this.#state === IPPProxyStates.PAUSED ||
      this.#state === IPPProxyStates.ERROR
    ) {
      await this.stop();
    }
  }

  #setPausedState() {
    this.#rotation?.controller.abort();

    const wasActive = this.#state === IPPProxyStates.ACTIVE;
    this.#pass = null;
    lazy.clearTimeout(this.#rotationTimer);
    this.#rotationTimer = 0;

    if (wasActive) {
      this.#connection?.suspend();
    } else {
      this.cancelChannelFilter();
    }


    this.#setState(IPPProxyStates.PAUSED);
  }

  async #handleEvent(event) {
    const { state, prevState } = event.detail;
    if (state !== lazy.IPProtectionStates.READY) {
      await this.reset();
    } else if (prevState !== lazy.IPProtectionStates.READY) {
      this.refreshUsage();
    }
    this.updateState();
  }

  async #getPassAndUsage(abortSignal = null) {
    let { status, error, pass, usage } =
      await lazy.IPProtectionService.authProvider.fetchProxyPass(abortSignal);
    lazy.logConsole.debug("ProxyPass:", {
      status,
      valid: pass?.isValid(),
      error,
    });

    if (status === 429 && error === "quota_exceeded") {
      lazy.logConsole.info("Quota exceeded", {
        usage: usage ? `${usage.remaining} / ${usage.max}` : "unknown",
      });
      return { pass: null, usage, error, status };
    }

    if (error || status != 200) {
      return { error: error || `Status: ${status}`, status };
    }

    return { pass, usage, status };
  }

  async #attemptPassRotation(abortSignal) {
    let delay = lazy.retryAfter;
    while (!abortSignal.aborted) {
      let result;
      try {
        result = await this.#getPassAndUsage(abortSignal);
      } catch (e) {
        if (abortSignal.aborted) {
          return null;
        }
        if (!lazy.IPPNetworkUtils.isOffline) {
          throw e;
        }
      }
      if (result && !(result.status >= 500 && result.status <= 599)) {
        return result;
      }
      await scheduleCallback(
        () => {
          delay *= 2;
        },
        Temporal.Now.instant().add({ milliseconds: delay }),
        abortSignal
      );
    }
    return null;
  }

  #schedulePassRotation(pass) {
    if (this.#rotationTimer) {
      lazy.clearTimeout(this.#rotationTimer);
      this.#rotationTimer = 0;
    }

    const now = Temporal.Now.instant();
    const rotationTimePoint = pass.rotationTimePoint;
    let msUntilRotation = now.until(rotationTimePoint).total("milliseconds");
    if (msUntilRotation <= 0) {
      msUntilRotation = 0;
    }

    lazy.logConsole.debug(
      `ProxyPass will rotate in ${now.until(rotationTimePoint).total("minutes")} minutes`
    );
    this.#rotationTimer = lazy.setTimeout(async () => {
      this.#rotationTimer = 0;
      if (!this.#connection?.active) {
        return;
      }
      lazy.logConsole.debug(`Starting scheduled ProxyPass rotation`);
      let newPass = await this.rotateProxyPass();
      if (newPass) {
        this.#schedulePassRotation(newPass);
      }
    }, msUntilRotation);
  }

  async rotateProxyPass() {
    if (this.#rotation) {
      return this.#rotation.promise;
    }
    const controller = new AbortController();
    const timeoutId = lazy.setTimeout(() => {
      controller.abort(ERRORS.TIMEOUT);
    }, lazy.timeout);
    let { promise, resolve } = Promise.withResolvers();
    this.#rotation = { promise, controller };
    let resumed = false;
    using scopeGuard = new DisposableStack();
    scopeGuard.defer(() => {
      lazy.clearTimeout(timeoutId);
      if (!resumed) {
        this.#connection?.abortPendingChannels();
      }
      resolve();
      this.#rotation = null;
    });

    if (this.#connection?.active) {
      this.#connection.suspend();
    }

    let result = await this.#attemptPassRotation(controller.signal);
    if (controller.signal.aborted || !result) {
      if (controller.signal.reason === ERRORS.TIMEOUT) {
        this.#setErrorState(ERRORS.TIMEOUT);
      }
      return null;
    }

    const { pass, usage, error } = result;
    if (usage) {
      this.#setUsage(usage);
      if (usage.quotaExhausted) {
        this.#setPausedState();
        return null;
      }
    }

    if (error) {
      this.#setErrorState(error);
      return null;
    }

    if (!pass) {
      lazy.logConsole.debug("Failed to rotate token!");
      this.#setErrorState("missing_pass");
      return null;
    }
    if (this.#connection?.active) {
      this.#connection.replaceAuthTokenAndResume(pass);
      this.networkErrorObserver.addIsolationKey(this.#connection.isolationKey);
      resumed = true;
    }
    lazy.logConsole.debug("Successfully rotated token!");
    this.#pass = pass;
    resolve(pass);
    return promise;
  }

  async refreshUsage() {
    this.#refreshUsageAbortController?.abort();
    this.#refreshUsageAbortController = new AbortController();
    const { signal } = this.#refreshUsageAbortController;
    let newUsage;
    try {
      newUsage =
        await lazy.IPProtectionService.authProvider.fetchProxyUsage(signal);
    } catch (error) {
      lazy.logConsole.error("Error refreshing usage:", error);
    } finally {
      this.#refreshUsageAbortController = null;
    }
    if (!newUsage) {
      lazy.logConsole.debug("Failed to refresh usage info!");
      return;
    }
    this.#setUsage(newUsage);
    this.updateState();
  }

  #handleProxyErrorEvent(event) {
    if (!this.#connection?.active) {
      return null;
    }
    const { isolationKey, level, httpStatus } = event.detail;
    if (isolationKey != this.#connection?.isolationKey) {
      return null;
    }

    if (httpStatus === 401 || httpStatus === 407 || httpStatus == 403) {
      if (level == "error" || this.#pass?.shouldRotate()) {
        return this.rotateProxyPass();
      }
    }
    return null;
  }

  updateState() {
    if (this.#state === IPPProxyStates.ERROR && this.#connection?.active) {
      this.#setState(IPPProxyStates.ERROR);
      return;
    }

    this.#errorType = null;

    if (lazy.IPProtectionService.state !== lazy.IPProtectionStates.READY) {
      this.#setState(IPPProxyStates.NOT_READY);
      return;
    }

    if (this.#usage?.quotaExhausted) {
      this.#setState(IPPProxyStates.PAUSED);
      return;
    }

    if (this.#connection?.active) {
      this.#setState(IPPProxyStates.ACTIVE);
      return;
    }

    this.#setState(IPPProxyStates.READY);
  }

  #setErrorState(error) {
    this.#rotation?.controller.abort();

    this.#errorType = typeof error === "string" ? error : ERRORS.GENERIC;
    if (this.#state === IPPProxyStates.ACTIVE) {
      this.#setState(IPPProxyStates.ERROR);
    } else {
      this.updateState();
    }

    lazy.logConsole.error(error);
  }

  #setUsage(usage) {
    if (usage.unlimited && this.#usage?.equals(usage)) {
      return;
    }
    this.#usage = usage;

    if (!usage.unlimited) {
      const now = Temporal.Now.instant();
      const daysUntilReset = now.until(usage.reset).total("days");
      lazy.logConsole.debug("ProxyUsage:", {
        usage: `${usage.remaining} / ${usage.max}`,
        resetsIn: `${daysUntilReset.toFixed(1)} days`,
      });
      this.#scheduleUsageCheck(usage);
    }

    this.dispatchEvent(
      new CustomEvent("IPPProxyManager:UsageChanged", {
        bubbles: true,
        composed: true,
        detail: {
          usage,
        },
      })
    );
  }

  #scheduleUsageCheck(usage) {
    if (this.#usageRefreshAbortController) {
      this.#usageRefreshAbortController.abort();
      this.#usageRefreshAbortController = null;
    }
    this.#usageRefreshAbortController = new AbortController();
    scheduleCallback(
      async () => {
        await this.refreshUsage();
      },
      usage.reset,
      this.#usageRefreshAbortController.signal
    );
  }

  #setState(state) {
    if (state === this.#state) {
      return;
    }

    this.#state = state;

    this.dispatchEvent(
      new CustomEvent("IPPProxyManager:StateChanged", {
        bubbles: true,
        composed: true,
        detail: {
          state,
        },
      })
    );
  }
}

const IPPProxyManager = new IPPProxyManagerSingleton();

export { IPPProxyManager };
