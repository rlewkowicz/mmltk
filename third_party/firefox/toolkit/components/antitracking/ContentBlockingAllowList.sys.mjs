/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

export const ContentBlockingAllowList = {
  _observingLastPBContext: false,

  _maybeSetupLastPBContextObserver() {
    if (!this._observingLastPBContext) {
      this._observer = {
        QueryInterface: ChromeUtils.generateQI([
          "nsIObserver",
          "nsISupportsWeakReference",
        ]),

        observe(subject, topic) {
          if (topic == "last-pb-context-exited") {
            Services.perms.removeByType("trackingprotection-pb");
          }
        },
      };
      Services.obs.addObserver(this._observer, "last-pb-context-exited", true);
      this._observingLastPBContext = true;
    }
  },

  _basePrincipalForAntiTrackingCommon(browser) {
    let principal =
      browser.browsingContext.currentWindowGlobal
        ?.contentBlockingAllowListPrincipal;
    if (!principal || !principal.isContentPrincipal) {
      return null;
    }
    return principal;
  },

  _permissionTypeFor(browser) {
    return lazy.PrivateBrowsingUtils.isBrowserPrivate(browser)
      ? "trackingprotection-pb"
      : "trackingprotection";
  },

  _expiryFor(browser) {
    return lazy.PrivateBrowsingUtils.isBrowserPrivate(browser)
      ? Ci.nsIPermissionManager.EXPIRE_SESSION
      : Ci.nsIPermissionManager.EXPIRE_NEVER;
  },

  canHandle(browser) {
    return this._basePrincipalForAntiTrackingCommon(browser) != null;
  },

  add(browser) {
    this._maybeSetupLastPBContextObserver();

    let prin = this._basePrincipalForAntiTrackingCommon(browser);
    let type = this._permissionTypeFor(browser);
    let expire = this._expiryFor(browser);
    Services.perms.addFromPrincipal(
      prin,
      type,
      Services.perms.ALLOW_ACTION,
      expire
    );
  },

  remove(browser) {
    let prin = this._basePrincipalForAntiTrackingCommon(browser);
    let type = this._permissionTypeFor(browser);
    Services.perms.removeFromPrincipal(prin, type);
  },

  includes(browser) {
    let prin = this._basePrincipalForAntiTrackingCommon(browser);
    let type = this._permissionTypeFor(browser);
    return (
      Services.perms.testExactPermissionFromPrincipal(prin, type) ==
      Services.perms.ALLOW_ACTION
    );
  },
};
