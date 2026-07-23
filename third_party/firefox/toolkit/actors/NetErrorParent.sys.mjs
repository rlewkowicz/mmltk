/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const PREF_SSL_IMPACT_ROOTS = [
  "security.tls.version.",
  "security.ssl3.",
  "security.tls13.",
];

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  HomePage: "resource:///modules/HomePage.sys.mjs",
});

class CaptivePortalObserver {
  constructor(actor) {
    this.actor = actor;
    Services.obs.addObserver(this, "captive-portal-login-abort");
    Services.obs.addObserver(this, "captive-portal-login-success");
  }

  stop() {
    Services.obs.removeObserver(this, "captive-portal-login-abort");
    Services.obs.removeObserver(this, "captive-portal-login-success");
  }

  observe(aSubject, aTopic) {
    switch (aTopic) {
      case "captive-portal-login-abort":
      case "captive-portal-login-success":
        this.actor.sendAsyncMessage("AboutNetErrorCaptivePortalFreed");
        break;
    }
  }
}

export class EscapablePageParent extends JSWindowActorParent {
  leaveErrorPage(browser, allowGoingBack = true) {
    if (!browser.canGoBack || !allowGoingBack) {
      let safePage = "about:blank";

      if (AppConstants.MOZ_BUILD_APP == "browser") {
        safePage = lazy.HomePage.getForErrorPage(browser.documentGlobal);
      }
      browser.fixupAndLoadURIString(safePage, {
        triggeringPrincipal:
          Services.scriptSecurityManager.getSystemPrincipal(),
      });
    } else {
      browser.goBack();
    }
  }
}

export class NetErrorParent extends EscapablePageParent {
  constructor() {
    super();
    this.captivePortalObserver = new CaptivePortalObserver(this);
  }

  didDestroy() {
    if (this.captivePortalObserver) {
      this.captivePortalObserver.stop();
    }
  }

  get browser() {
    return this.browsingContext.top.embedderElement;
  }

  hasChangedCertPrefs() {
    let prefSSLImpact = PREF_SSL_IMPACT_ROOTS.reduce((prefs, root) => {
      return prefs.concat(Services.prefs.getChildList(root));
    }, []);
    for (let prefName of prefSSLImpact) {
      if (Services.prefs.prefHasUserValue(prefName)) {
        return true;
      }
    }

    return false;
  }

  primeMitm(browser) {
    if (Services.prefs.getStringPref("security.pki.mitm_canary_issuer", null)) {
      return;
    }

    let url = Services.prefs.getStringPref(
      "security.certerrors.mitm.priming.endpoint"
    );
    let request = new XMLHttpRequest({ mozAnon: true });
    request.open("HEAD", url);
    request.channel.loadFlags |= Ci.nsIRequest.LOAD_BYPASS_CACHE;
    request.channel.loadFlags |= Ci.nsIRequest.INHIBIT_CACHING;

    request.addEventListener("error", () => {
      if (!browser.documentURI.spec.startsWith("about:certerror")) {
        return;
      }

      let secInfo = request.channel.securityInfo;
      if (secInfo.errorCodeString != "SEC_ERROR_UNKNOWN_ISSUER") {
        return;
      }

      if (secInfo.serverCert && secInfo.serverCert.issuerName) {
        Services.prefs.setStringPref(
          "security.pki.mitm_canary_issuer",
          secInfo.serverCert.issuerName
        );

        if (
          Services.prefs.getBoolPref(
            "security.certerrors.mitm.auto_enable_enterprise_roots"
          )
        ) {
          if (
            !Services.prefs.getBoolPref("security.enterprise_roots.enabled")
          ) {
            lazy.BrowserUtils.promiseObserved(
              "psm:enterprise-certs-imported"
            ).then(() => {
              if (browser.documentURI.spec.startsWith("about:certerror")) {
                browser.reload();
              }
            });

            Services.prefs.setBoolPref(
              "security.enterprise_roots.enabled",
              true
            );
            Services.prefs.setBoolPref(
              "security.enterprise_roots.auto-enabled",
              true
            );
          }
        } else {
          browser.reload();
        }
      }
    });

    request.send(null);
  }

  displayOfflineSupportPage(supportPageSlug) {
    const AVAILABLE_PAGES = ["connection-not-secure", "time-errors"];
    if (!AVAILABLE_PAGES.includes(supportPageSlug)) {
      console.log(
        `[Not supported] Offline support is not yet available for ${supportPageSlug} errors.`
      );
      return;
    }

    let offlinePagePath = `chrome://global/content/neterror/supportpages/${supportPageSlug}.html`;
    let triggeringPrincipal =
      Services.scriptSecurityManager.getSystemPrincipal();
    this.browser.loadURI(Services.io.newURI(offlinePagePath), {
      triggeringPrincipal,
    });
  }

  receiveMessage(message) {
    switch (message.name) {
      case "Browser:EnableOnlineMode":
        Services.io.offline = false;
        this.browser.reload();
        break;
      case "Browser:OpenCaptivePortalPage":
        this.browser.documentGlobal.CaptivePortalWatcher.ensureCaptivePortalTab();
        break;
      case "Browser:PrimeMitm":
        this.primeMitm(this.browser);
        break;
      case "Browser:ResetEnterpriseRootsPref":
        Services.prefs.clearUserPref("security.enterprise_roots.enabled");
        Services.prefs.clearUserPref("security.enterprise_roots.auto-enabled");
        break;
      case "Browser:ResetSSLPreferences": {
        let prefSSLImpact = PREF_SSL_IMPACT_ROOTS.reduce((prefs, root) => {
          return prefs.concat(Services.prefs.getChildList(root));
        }, []);
        for (let prefName of prefSSLImpact) {
          Services.prefs.clearUserPref(prefName);
        }
        this.browser.reload();
        break;
      }
      case "Browser:SSLErrorGoBack":
        this.leaveErrorPage(this.browser);
        break;
      case "GetChangedCertPrefs": {
        let hasChangedCertPrefs = this.hasChangedCertPrefs();
        this.sendAsyncMessage("HasChangedCertPrefs", {
          hasChangedCertPrefs,
        });
        break;
      }
      case "DisplayOfflineSupportPage":
        this.displayOfflineSupportPage(message.data.supportPageSlug);
        break;
      case "Browser:AddTRRExcludedDomain": {
        let uri = this.browsingContext.currentURI;
        if (uri instanceof Ci.nsINestedURI) {
          uri = uri.QueryInterface(Ci.nsINestedURI).innermostURI;
        }
        let excludedDomains = Services.prefs.getStringPref(
          "network.trr.excluded-domains"
        );
        excludedDomains += `, ${uri.asciiHost}`;
        Services.prefs.setStringPref(
          "network.trr.excluded-domains",
          excludedDomains
        );
        break;
      }
      case "OpenTRRPreferences": {
        let browser = this.browsingContext.top.embedderElement;
        if (!browser) {
          break;
        }

        let win = browser.documentGlobal;
        win.openPreferences("privacy-doh");
        break;
      }
    }
  }
}
