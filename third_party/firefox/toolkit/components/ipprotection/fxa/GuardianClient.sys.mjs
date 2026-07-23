/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import {
  Entitlement,
  ProxyPass,
  ProxyUsage,
} from "moz-src:///toolkit/components/ipprotection/GuardianTypes.sys.mjs";

const lazy = {};

ChromeUtils.defineLazyGetter(
  lazy,
  "hiddenBrowserManager",
  () =>
    ChromeUtils.importESModule("resource://gre/modules/HiddenFrame.sys.mjs")
      .HiddenBrowserManager
);
ChromeUtils.defineLazyGetter(lazy, "logConsole", () =>
  console.createInstance({
    prefix: "GuardianClient",
    maxLogLevel: Services.prefs.getBoolPref("browser.ipProtection.log", false)
      ? "Debug"
      : "Warn",
  })
);

if (Services.appinfo.processType !== Services.appinfo.PROCESS_TYPE_DEFAULT) {
  throw new Error("Guardian.sys.mjs should only run in the parent process");
}

export const GUARDIAN_EXPERIMENT_TYPE = "alpha";

export class GuardianClient {
  constructor() {
    XPCOMUtils.defineLazyPreferenceGetter(
      this,
      "guardianEndpoint",
      "browser.ipProtection.guardian.endpoint",
      "https://vpn.mozilla.com"
    );
    XPCOMUtils.defineLazyPreferenceGetter(
      this,
      "fxaOrigin",
      "identity.fxaccounts.remote.root"
    );
  }
  async enrollWithFxa(
    aExperimentType = GUARDIAN_EXPERIMENT_TYPE,
    aAbortSignal = null
  ) {
    const allowedOrigins = [
      new URL(this.guardianEndpoint).origin,
      new URL(this.fxaOrigin).origin,
    ];
    const { loginURL, successURL, errorURL } =
      this.enrollmentURLs(aExperimentType);
    const finalizerURLs = [successURL, errorURL];
    return await lazy.hiddenBrowserManager.withHiddenBrowser(async browser => {
      const aborted = new Promise((_, reject) => {
        aAbortSignal?.addEventListener("abort", () => {
          browser.stop();
          browser.remove();
          reject(new Error("aborted"));
        });
      });
      const finalEndpoint = waitUntilURL(browser, url => {
        const urlObj = new URL(url);
        if (url === "about:blank") {
          return false;
        }
        if (!allowedOrigins.includes(urlObj.origin)) {
          browser.stop();
          browser.remove();
          throw new Error(
            `URL ${url} with origin ${urlObj.origin} is not allowed.`
          );
        }
        if (
          finalizerURLs.some(
            finalizer =>
              urlObj.pathname === finalizer.pathname &&
              urlObj.origin === finalizer.origin
          )
        ) {
          return true;
        }
        return false;
      });
      const loginURI = Services.io.newURI(loginURL.href);
      if (!allowedOrigins.includes(loginURL.origin)) {
        throw new Error(`Login URL origin ${loginURL.origin} is not allowed.`);
      }
      browser.loadURI(loginURI, {
        triggeringPrincipal:
          Services.scriptSecurityManager.createContentPrincipal(loginURI, {}),
      });

      const result = await Promise.race([finalEndpoint, aborted]);
      return GuardianClient._parseGuardianSuccessURL(result);
    });
  }

  static _parseGuardianSuccessURL(aUrl) {
    if (!aUrl) {
      return { error: "timeout", ok: false };
    }
    const url = new URL(aUrl);
    const params = new URLSearchParams(url.search);
    const error = params.get("error");
    if (error) {
      return { error, ok: false };
    }
    if (!params.has("code")) {
      return { error: "missing_code", ok: false };
    }
    return { ok: true };
  }

  async fetchProxyPass(tokenHandle, abortSignal = null) {
    const response = await fetch(this.#tokenURL, {
      method: "GET",
      cache: "no-cache",
      headers: {
        Authorization: `Bearer ${tokenHandle.token}`,
        "Content-Type": "application/json",
      },
      signal: abortSignal,
    });
    if (!response) {
      return { error: "login_needed", usage: null };
    }
    const status = response.status;

    let usage = null;
    try {
      usage = ProxyUsage.fromResponse(response);
    } catch (error) {
      lazy.logConsole.warn(
        "Usage headers missing or invalid, continuing without usage:",
        error
      );
    }

    if (status === 429) {
      const retryAfter = response.headers.get("Retry-After");
      return {
        status,
        error: "quota_exceeded",
        usage,
        retryAfter,
      };
    }

    try {
      const pass = await ProxyPass.fromResponse(response);
      if (!pass) {
        return { status, error: "invalid_response", usage };
      }
      return { pass, status, usage };
    } catch (error) {
      lazy.logConsole.error("Error parsing pass:", error);
      return { status, error: "parse_error", usage };
    }
  }
  async fetchUserInfo(tokenHandle, abortSignal = null) {
    const response = await fetch(this.#statusURL, {
      method: "GET",
      headers: {
        Authorization: `Bearer ${tokenHandle.token}`,
        "Content-Type": "application/json",
      },
      cache: "no-cache",
      signal: abortSignal,
    });
    if (!response) {
      return { error: "login_needed" };
    }
    const status = response.status;
    try {
      const entitlement = await Entitlement.fromResponse(response);
      if (!entitlement) {
        return { status, error: "parse_error" };
      }
      return {
        status,
        entitlement,
      };
    } catch (error) {
      return { status, error: "parse_error" };
    }
  }

  async fetchProxyUsage(tokenHandle, abortSignal) {
    const response = await fetch(this.#tokenURL, {
      method: "HEAD",
      cache: "no-cache",
      signal: abortSignal,
      headers: {
        Authorization: `Bearer ${tokenHandle.token}`,
        "Content-Type": "application/json",
      },
    });
    if (!response) {
      return null;
    }
    try {
      return ProxyUsage.fromResponse(response);
    } catch (error) {
      lazy.logConsole.warn(
        "Usage headers missing or invalid, continuing without usage:",
        error
      );
    }
    return null;
  }

  async activate(tokenHandle, abortSignal = null) {
    if (!tokenHandle) {
      return { ok: false, error: "login_needed" };
    }
    const response = await fetch(this.#activateURL, {
      method: "POST",
      cache: "no-cache",
      headers: {
        Authorization: `Bearer ${tokenHandle.token}`,
        "Content-Type": "application/json",
      },
      signal: abortSignal,
    });
    if (!response.ok) {
      return { ok: false, error: `status_${response.status}` };
    }
    try {
      const entitlement = await Entitlement.fromResponse(response);
      return { ok: true, entitlement };
    } catch (error) {
      return { ok: false, error: "parse_error" };
    }
  }

  get #activateURL() {
    const url = new URL(this.guardianEndpoint);
    url.pathname = "/api/v1/fpn/activate";
    return url;
  }

  get #tokenURL() {
    const url = new URL(this.guardianEndpoint);
    url.pathname = "/api/v1/fpn/token";
    return url;
  }
  enrollmentURLs(experimentType = GUARDIAN_EXPERIMENT_TYPE) {
    const loginURL = new URL(this.guardianEndpoint);
    loginURL.pathname = "/api/v1/fpn/auth";
    loginURL.searchParams.set("experiment", experimentType);

    const successURL = new URL(this.guardianEndpoint);
    successURL.pathname = "/oauth/success";

    const errorURL = new URL(this.guardianEndpoint);
    errorURL.pathname = "/api/v1/fpn/error";

    return { loginURL, successURL, errorURL };
  }
  get #statusURL() {
    const url = new URL(this.guardianEndpoint);
    url.pathname = "/api/v1/fpn/status";
    return url;
  }
  guardianEndpoint = "";
}

const listeners = new Set();

async function waitUntilURL(browser, predicate) {
  const prom = Promise.withResolvers();
  let done = false;
  const check = arg => {
    if (done) {
      return;
    }
    if (predicate(arg)) {
      done = true;
      listeners.delete(listener);
      browser.removeProgressListener(listener);
      prom.resolve(arg);
    }
  };
  const listener = {
    QueryInterface: ChromeUtils.generateQI([
      "nsIWebProgressListener",
      "nsISupportsWeakReference",
    ]),

    onStateChange(webProgress, request, stateFlags, status) {
      request.QueryInterface(Ci.nsIChannel);

      if (
        webProgress.isTopLevel &&
        stateFlags & Ci.nsIWebProgressListener.STATE_STOP &&
        status !== Cr.NS_BINDING_ABORTED
      ) {
        check(request.URI?.spec);
      }
    },

    onLocationChange() {},
    onProgressChange() {},
    onStatusChange(_, request, status) {
      if (Components.isSuccessCode(status)) {
        return;
      }
      try {
        const url = request.QueryInterface(Ci.nsIChannel).URI.spec;
        check(url);
      } catch (ex) {}
    },
    onSecurityChange() {},
    onContentBlockingEvent() {},
  };
  listeners.add(listener);
  browser.addProgressListener(listener, Ci.nsIWebProgress.NOTIFY_STATE_WINDOW);
  const url = await prom.promise;
  return url;
}
