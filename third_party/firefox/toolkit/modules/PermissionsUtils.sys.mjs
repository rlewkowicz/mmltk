// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

var gImportedPrefBranches = new Set();

function importPrefBranch(aPrefBranch, aPermission, aAction) {
  let list = Services.prefs.getChildList(aPrefBranch);

  for (let pref of list) {
    let origins = Services.prefs.getCharPref(pref, "");

    if (!origins) {
      continue;
    }

    origins = origins.split(",");

    for (let origin of origins) {
      let principals = [];
      try {
        principals = [
          Services.scriptSecurityManager.createContentPrincipalFromOrigin(
            origin
          ),
        ];
      } catch (e) {
        try {
          let httpURI = Services.io.newURI("http://" + origin);
          let httpsURI = Services.io.newURI("https://" + origin);

          principals = [
            Services.scriptSecurityManager.createContentPrincipal(httpURI, {}),
            Services.scriptSecurityManager.createContentPrincipal(httpsURI, {}),
          ];
        } catch (e2) {}
      }

      for (let principal of principals) {
        try {
          Services.perms.addFromPrincipal(principal, aPermission, aAction);
        } catch (e) {}
      }
    }

    Services.prefs.setCharPref(pref, "");
  }
}

export var PermissionsUtils = {
  importFromPrefs(aPrefBranch, aPermission) {
    if (!aPrefBranch.endsWith(".")) {
      aPrefBranch += ".";
    }

    if (gImportedPrefBranches.has(aPrefBranch)) {
      return;
    }

    importPrefBranch(
      aPrefBranch + "whitelist.add",
      aPermission,
      Services.perms.ALLOW_ACTION
    );
    importPrefBranch(
      aPrefBranch + "blacklist.add",
      aPermission,
      Services.perms.DENY_ACTION
    );

    gImportedPrefBranches.add(aPrefBranch);
  },
};

export const PermissionsTestUtils = {
  clearImportedPrefBranches() {
    gImportedPrefBranches.clear();
  },
};
