/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const PREF_TEST_ORIGINS = "browser.uitour.testingOrigins";
const UITOUR_PERMISSION = "uitour";

export let UITourUtils = {
  isTestingOrigin(uri) {
    let testingOrigins = Services.prefs.getStringPref(PREF_TEST_ORIGINS, "");
    if (!testingOrigins) {
      return false;
    }

    for (let origin of testingOrigins.split(/\s*,\s*/)) {
      try {
        let testingURI = Services.io.newURI(origin);
        if (uri.prePath == testingURI.prePath) {
          return true;
        }
      } catch (ex) {
        console.error(ex);
      }
    }
    return false;
  },

  ensureTrustedOrigin(windowGlobal) {
    if (windowGlobal.browsingContext.parent || !windowGlobal.isCurrentGlobal) {
      return false;
    }

    let principal, uri;
    if (WindowGlobalParent.isInstance(windowGlobal)) {
      if (!windowGlobal.browsingContext.secureBrowserUI?.isSecureContext) {
        return false;
      }
      principal = windowGlobal.documentPrincipal;
      uri = windowGlobal.documentURI;
    } else {
      if (!windowGlobal.contentWindow?.isSecureContext) {
        return false;
      }
      let document = windowGlobal.contentWindow.document;
      principal = document?.nodePrincipal;
      uri = document?.documentURIObject;
    }

    if (!principal) {
      return false;
    }

    let permission = Services.perms.testPermissionFromPrincipal(
      principal,
      UITOUR_PERMISSION
    );
    if (permission == Services.perms.ALLOW_ACTION) {
      return true;
    }

    return uri && this.isTestingOrigin(uri);
  },
};
