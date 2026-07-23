/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PermissionUI: "resource:///modules/PermissionUI.sys.mjs",
});
const ContentPermissionIntegration = {
  createPermissionPrompt(type, request) {
    switch (type) {
      case "persistent-storage": {
        return new lazy.PermissionUI.PersistentStoragePermissionPrompt(request);
      }
      case "storage-access": {
        return new lazy.PermissionUI.StorageAccessPermissionPrompt(request);
      }
      case "loopback-network": {
        return new lazy.PermissionUI.LoopbackNetworkPermissionPrompt(request);
      }
      case "local-network": {
        return new lazy.PermissionUI.LocalNetworkPermissionPrompt(request);
      }
    }
    return undefined;
  },
};

export function ContentPermissionPrompt() {}

ContentPermissionPrompt.prototype = {
  classID: Components.ID("{d8903bf6-68d5-4e97-bcd1-e4d3012f721a}"),

  QueryInterface: ChromeUtils.generateQI(["nsIContentPermissionPrompt"]),

  prompt(request) {
    let type;
    try {
      let types = request.types.QueryInterface(Ci.nsIArray);
      if (types.length != 1) {
        throw Components.Exception(
          "Expected an nsIContentPermissionRequest with only 1 type.",
          Cr.NS_ERROR_UNEXPECTED
        );
      }

      type = types.queryElementAt(0, Ci.nsIContentPermissionType).type;
      let permissionPrompt = ContentPermissionIntegration.createPermissionPrompt(
        type,
        request
      );
      if (!permissionPrompt) {
        throw Components.Exception(
          `Failed to handle permission of type ${type}`,
          Cr.NS_ERROR_FAILURE
        );
      }

      permissionPrompt.prompt();
    } catch (ex) {
      console.error(ex);
      request.cancel();
      throw ex;
    }
  },
};
