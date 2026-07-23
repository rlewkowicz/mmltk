/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "serviceWorkerManager",
  "@mozilla.org/serviceworkers/manager;1",
  Ci.nsIServiceWorkerManager
);

let logConsole;
function log(msg) {
  if (!logConsole) {
    logConsole = console.createInstance({
      prefix: "PrincipalsCollector",
      maxLogLevelPref: "browser.sanitizer.loglevel",
    });
  }

  logConsole.log(msg);
}

export class PrincipalsCollector {
  #pendingCollection = null;
  constructor() {
    this.principals = null;
  }

  static isSupportedPrincipal(principal) {
    return ["http", "https", "file"].some(scheme => principal.schemeIs(scheme));
  }

  async getAllPrincipals(progress = {}) {
    if (this.principals) {
      return this.principals;
    }
    if (!this.#pendingCollection) {
      this.#pendingCollection = this._getAllPrincipalsInternal(progress);
      this.principals = await this.#pendingCollection;
      this.#pendingCollection = null;
      return this.principals;
    }
    await this.#pendingCollection;
    return this.principals;
  }

  async _getAllPrincipalsInternal(progress = {}) {
    progress.step = "principals-quota-manager";
    let principals = await new Promise(resolve => {
      Services.qms.listOrigins().callback = request => {
        progress.step = "principals-quota-manager-listOrigins";
        if (request.resultCode != Cr.NS_OK) {
          resolve([]);
          return;
        }

        let principalsMap = new Map();
        for (const origin of request.result) {
          let principal =
            Services.scriptSecurityManager.createContentPrincipalFromOrigin(
              origin
            );
          if (PrincipalsCollector.isSupportedPrincipal(principal)) {
            principalsMap.set(principal.origin, principal);
          }
        }

        progress.step = "principals-quota-manager-completed";
        resolve(principalsMap);
      };
    }).catch(ex => {
      console.error("QuotaManagerService promise failed: ", ex);
      return [];
    });

    progress.step = "principals-service-workers";
    let serviceWorkers = lazy.serviceWorkerManager.getAllRegistrations();
    for (let i = 0; i < serviceWorkers.length; i++) {
      let sw = serviceWorkers.queryElementAt(
        i,
        Ci.nsIServiceWorkerRegistrationInfo
      );
      principals.set(sw.principal.origin, sw.principal);
    }

    progress.step = "principals-cookies";
    let cookies = Services.cookies.cookies;
    let hosts = new Set();
    for (let cookie of cookies) {
      hosts.add(
        cookie.rawHost +
          ChromeUtils.originAttributesToSuffix(cookie.originAttributes)
      );
    }

    progress.step = "principals-host-cookie";
    hosts.forEach(host => {
      let principal;
      try {
        principal =
          Services.scriptSecurityManager.createContentPrincipalFromOrigin(
            "https://" + host
          );
      } catch (e) {
        log(
          `ERROR: Could not create content principal for host '${host}' ${e.message}`
        );
      }
      if (principal) {
        principals.set(principal.origin, principal);
      }
    });

    principals = Array.from(principals.values());
    progress.step = "total-principals:" + principals.length;
    return principals;
  }
}
