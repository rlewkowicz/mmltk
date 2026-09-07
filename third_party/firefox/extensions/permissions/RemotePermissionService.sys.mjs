/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { RemoteSettings } from "resource://services-settings/remote-settings.sys.mjs";

const COLLECTION_NAME = "remote-permissions";

const ALLOWED_PERMISSION_VALUES = {
  "https-only-load-insecure": [
    Ci.nsIHttpsOnlyModePermission.HTTPSFIRST_LOAD_INSECURE_ALLOW,
  ],
  "loopback-network": ["*"],
  "local-network": ["*"],
};

export class RemotePermissionService {
  classId = Components.ID("{a4b1b3b1-b68a-4129-aa2f-eb086162a8c7}");
  QueryInterface = ChromeUtils.generateQI(["nsIRemotePermissionService"]);

  #rs = RemoteSettings(COLLECTION_NAME);
  #initialized = Promise.withResolvers();
  #allowedPermissionValues = ALLOWED_PERMISSION_VALUES;

  constructor() {
    this.init();
  }

  async init() {
    try {
      if (Services.startup.shuttingDown) {
        return;
      }

      if (
        !Services.prefs.getBoolPref("permissions.manager.remote.enabled", false)
      ) {
        return;
      }

      let remotePermissions = await this.#rs.get();
      for (const permission of remotePermissions) {
        this.#addDefaultPermission(permission);
      }

      this.#rs.on("sync", this.#onSync.bind(this));

      this.#initialized.resolve();
    } catch (e) {
      this.#initialized.reject(e);
      throw e;
    }
  }

  get isInitialized() {
    return this.#initialized.promise;
  }

  // eslint-disable-next-line jsdoc/require-param
  #onSync({ data: { created = [], updated = [], deleted = [] } }) {
    const toBeDeletedPermissions = [
      ...deleted,
      ...updated
        .filter(
          ({
            old: { origin: oldOrigin, type: oldType },
            new: { origin: newOrigin, type: newType },
          }) => oldOrigin != newOrigin || oldType != newType
        )
        .map(({ old }) => old),
    ];

    const toBeAddedPermissions = [
      ...created,
      ...updated.map(({ new: newPermission }) => newPermission),
      ...toBeDeletedPermissions.map(({ origin, type }) => ({
        origin,
        type,
        capability: Ci.nsIPermissionManager.UNKNOWN_ACTION,
      })),
    ];

    for (const permission of toBeAddedPermissions) {
      this.#addDefaultPermission(permission);
    }
  }

  #isAllowed(type, capability) {
    if (!this.#allowedPermissionValues[type]) {
      if (this.#allowedPermissionValues["*"]) {
        this.#allowedPermissionValues[type] =
          this.#allowedPermissionValues["*"];
      } else {
        return false;
      }
    }

    return (
      this.#allowedPermissionValues[type].includes("*") ||
      this.#allowedPermissionValues[type].includes(capability) ||
      capability === Ci.nsIPermissionManager.UNKNOWN_ACTION
    );
  }

  #addDefaultPermission({ origin, type, capability }) {
    if (!this.#isAllowed(type, capability)) {
      console.error(
        `Remote Settings contain default permission of disallowed type '${type}' with value '${capability}' for origin '${origin}', skipping import`
      );
      return;
    }

    try {
      let principal = Services.scriptSecurityManager.createContentPrincipal(
        Services.io.newURI(origin),
        {}
      );
      Services.perms.addDefaultFromPrincipal(principal, type, capability);
    } catch (e) {
      console.error(e);
    }
  }
}
