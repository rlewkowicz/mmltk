/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  WindowsRegistry: "resource://gre/modules/WindowsRegistry.sys.mjs",
});

export let OsEnvironment = {
  Policy: {
    getAllowedAppSources: () =>
      lazy.WindowsRegistry.readRegKey(
        Ci.nsIWindowsRegKey.ROOT_KEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer",
        "AicEnabled"
      ),
    windowsVersionHasAppSourcesFeature: () => {
      let windowsVersion = parseFloat(Services.sysinfo.getProperty("version"));
      if (isNaN(windowsVersion)) {
        throw new Error("Unable to parse Windows version");
      }
      if (windowsVersion < 10) {
        return false;
      }

      const { buildNumber } = lazy.WindowsVersionInfo.get();
      return buildNumber >= 15063;
    },
  },

  reportAllowedAppSources() {
    if (AppConstants.platform != "win") {
      return;
    }

    let haveAppSourcesFeature;
    try {
      haveAppSourcesFeature =
        OsEnvironment.Policy.windowsVersionHasAppSourcesFeature();
    } catch (ex) {
      console.error(ex);
      return;
    }
    if (!haveAppSourcesFeature) {
      return;
    }

    let allowedAppSources;
    try {
      allowedAppSources = OsEnvironment.Policy.getAllowedAppSources();
    } catch (ex) {
      console.error(ex);
      return;
    }
    if (allowedAppSources === undefined) {
      allowedAppSources = "Anywhere";
    }

    const expectedValues = [
      "Anywhere",
      "Recommendations",
      "PreferStore",
      "StoreOnly",
    ];
    if (!expectedValues.includes(allowedAppSources)) {
      allowedAppSources = "Error";
    }

  },
};
