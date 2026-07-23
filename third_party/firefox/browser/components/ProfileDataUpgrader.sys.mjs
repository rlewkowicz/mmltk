/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  FormAutofillUtils: "resource://gre/modules/shared/FormAutofillUtils.sys.mjs",
  LoginHelper: "resource://gre/modules/LoginHelper.sys.mjs",
  PlacesUIUtils: "moz-src:///browser/components/places/PlacesUIUtils.sys.mjs",
});

export let ProfileDataUpgrader = {
  _migrateXULStoreForDocument(fromURL, toURL) {
    Array.from(Services.xulStore.getIDsEnumerator(fromURL)).forEach(id => {
      Array.from(Services.xulStore.getAttributeEnumerator(fromURL, id)).forEach(
        attr => {
          let value = Services.xulStore.getValue(fromURL, id, attr);
          Services.xulStore.setValue(toURL, id, attr, value);
        }
      );
    });
  },

  _migrateHashedKeysForXULStoreForDocument(docUrl) {
    Array.from(Services.xulStore.getIDsEnumerator(docUrl))
      .filter(id => id.startsWith("place:"))
      .forEach(id => {
        Services.xulStore.removeValue(docUrl, id, "open");
        let hashedId = lazy.PlacesUIUtils.obfuscateUrlForXulStore(id);
        Services.xulStore.setValue(docUrl, hashedId, "open", "true");
      });
  },

  // eslint-disable-next-line complexity
  upgrade(existingDataVersion, newVersion) {
    const BROWSER_DOCURL = AppConstants.BROWSER_CHROME_URL;

    let xulStore = Services.xulStore;

    if (existingDataVersion < 90) {
      this._migrateXULStoreForDocument(
        "chrome://browser/content/places/historySidebar.xul",
        "chrome://browser/content/places/historySidebar.xhtml"
      );
      this._migrateXULStoreForDocument(
        "chrome://browser/content/places/places.xul",
        "chrome://browser/content/places/places.xhtml"
      );
      this._migrateXULStoreForDocument(
        "chrome://browser/content/places/bookmarksSidebar.xul",
        "chrome://browser/content/places/bookmarksSidebar.xhtml"
      );
    }

    if (
      existingDataVersion < 91 &&
      Services.prefs.getBoolPref("network.proxy.share_proxy_settings", false) &&
      Services.prefs.getIntPref("network.proxy.type", 0) == 1
    ) {
      let httpProxy = Services.prefs.getCharPref("network.proxy.http", "");
      let httpPort = Services.prefs.getIntPref("network.proxy.http_port", 0);
      let socksProxy = Services.prefs.getCharPref("network.proxy.socks", "");
      let socksPort = Services.prefs.getIntPref("network.proxy.socks_port", 0);
      if (httpProxy && httpProxy == socksProxy && httpPort == socksPort) {
        Services.prefs.setCharPref(
          "network.proxy.socks",
          Services.prefs.getCharPref("network.proxy.backup.socks", "")
        );
        Services.prefs.setIntPref(
          "network.proxy.socks_port",
          Services.prefs.getIntPref("network.proxy.backup.socks_port", 0)
        );
      }
    }

    if (existingDataVersion < 92) {
      let longpress = Services.prefs.getIntPref(
        "privacy.userContext.longPressBehavior",
        0
      );
      if (longpress == 1) {
        Services.prefs.setBoolPref(
          "privacy.userContext.newTabContainerOnLeftClick.enabled",
          true
        );
      }
    }

    if (existingDataVersion < 94) {
      let backup = Services.prefs.getCharPref("network.proxy.backup.socks", "");
      let backupPort = Services.prefs.getIntPref(
        "network.proxy.backup.socks_port",
        0
      );
      let socksProxy = Services.prefs.getCharPref("network.proxy.socks", "");
      let socksPort = Services.prefs.getIntPref("network.proxy.socks_port", 0);
      if (backup == socksProxy) {
        Services.prefs.clearUserPref("network.proxy.backup.socks");
      }
      if (backupPort == socksPort) {
        Services.prefs.clearUserPref("network.proxy.backup.socks_port");
      }
    }

    if (existingDataVersion < 95) {
      const oldPrefName = "media.autoplay.enabled.user-gestures-needed";
      const oldPrefValue = Services.prefs.getBoolPref(oldPrefName, true);
      const newPrefValue = oldPrefValue ? 0 : 1;
      Services.prefs.setIntPref("media.autoplay.blocking_policy", newPrefValue);
      Services.prefs.clearUserPref(oldPrefName);
    }

    if (existingDataVersion < 96) {
      const oldPrefName = "browser.urlbar.openViewOnFocus";
      const oldPrefValue = Services.prefs.getBoolPref(oldPrefName, true);
      Services.prefs.setBoolPref(
        "browser.urlbar.suggest.topsites",
        oldPrefValue
      );
      Services.prefs.clearUserPref(oldPrefName);
    }

    if (existingDataVersion < 97) {
      let userCustomizedWheelMax = Services.prefs.prefHasUserValue(
        "general.smoothScroll.mouseWheel.durationMaxMS"
      );
      let userCustomizedWheelMin = Services.prefs.prefHasUserValue(
        "general.smoothScroll.mouseWheel.durationMinMS"
      );

      if (!userCustomizedWheelMin && !userCustomizedWheelMax) {
      } else if (userCustomizedWheelMin && !userCustomizedWheelMax) {
        Services.prefs.setIntPref(
          "general.smoothScroll.mouseWheel.durationMaxMS",
          400
        );
      } else if (!userCustomizedWheelMin && userCustomizedWheelMax) {
        Services.prefs.setIntPref(
          "general.smoothScroll.mouseWheel.durationMinMS",
          200
        );
      } else {
      }
    }

    if (existingDataVersion < 98) {
      Services.prefs.clearUserPref("browser.search.cohort");
    }

    if (existingDataVersion < 99) {
      Services.prefs.clearUserPref("security.tls.version.enable-deprecated");
    }

    if (existingDataVersion < 102) {
      const { CustomizableUI } = ChromeUtils.importESModule(
        "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs"
      );
      CustomizableUI.removeWidgetFromArea("managed-bookmarks");
    }

    if (existingDataVersion < 103) {
      let bookmarksToolbarWasVisible =
        Services.xulStore.getValue(
          BROWSER_DOCURL,
          "PersonalToolbar",
          "collapsed"
        ) == "false";
      if (bookmarksToolbarWasVisible) {
        Services.prefs.setCharPref(
          "browser.toolbars.bookmarks.visibility",
          "always"
        );
      }
      Services.xulStore.removeValue(
        BROWSER_DOCURL,
        "PersonalToolbar",
        "collapsed"
      );

      Services.prefs.clearUserPref(
        "browser.livebookmarks.migrationAttemptsLeft"
      );
    }

    if (existingDataVersion < 104) {
      Services.prefs.setCharPref(
        "browser.bookmarks.defaultLocation",
        "unfiled"
      );
    }

    if (existingDataVersion < 105) {
      const oldPrefName = "browser.urlbar.imeCompositionClosesPanel";
      const oldPrefValue = Services.prefs.getBoolPref(oldPrefName, true);
      Services.prefs.setBoolPref(
        "browser.urlbar.keepPanelOpenDuringImeComposition",
        !oldPrefValue
      );
      Services.prefs.clearUserPref(oldPrefName);
    }

    if (existingDataVersion < 107) {
      const kPref = "browser.handlers.migrations";
      let migrations = Services.prefs
        .getCharPref(kPref, "")
        .split(",")
        .filter(x => !!x);
      migrations.push("secure-mail");
      Services.prefs.setCharPref(kPref, migrations.join(","));
    }

    if (existingDataVersion < 108) {
      let defaultValue = false;
      let oldPrefName = "browser.ctrlTab.recentlyUsedOrder";
      let oldPrefDefault = true;
      if (Services.prefs.getBoolPref("browser.engagement.ctrlTab.has-used")) {
        let newPrefValue = Services.prefs.getBoolPref(
          oldPrefName,
          oldPrefDefault
        );
        Services.prefs.setBoolPref(
          "browser.ctrlTab.sortByRecentlyUsed",
          newPrefValue
        );
      } else {
        Services.prefs.setBoolPref(
          "browser.ctrlTab.sortByRecentlyUsed",
          defaultValue
        );
      }
    }

    if (existingDataVersion < 109) {
      if (
        Services.prefs.prefHasUserValue("signon.recipes.remoteRecipesEnabled")
      ) {
        Services.prefs.setBoolPref(
          "signon.recipes.remoteRecipes.enabled",
          Services.prefs.getBoolPref(
            "signon.recipes.remoteRecipesEnabled",
            true
          )
        );
        Services.prefs.clearUserPref("signon.recipes.remoteRecipesEnabled");
      }
    }

    if (existingDataVersion < 120) {
      const oldPref = "browser.tabs.drawInTitlebar";
      const newPref = "browser.tabs.inTitlebar";
      if (Services.prefs.prefHasUserValue(oldPref)) {
        const oldPrefType = Services.prefs.getPrefType(oldPref);
        if (oldPrefType == Services.prefs.PREF_BOOL) {
          Services.prefs.setIntPref(
            newPref,
            Services.prefs.getBoolPref(oldPref) ? 1 : 0
          );
        } else {
          Services.prefs.setIntPref(
            newPref,
            Services.prefs.getIntPref(oldPref)
          );
        }
        Services.prefs.clearUserPref(oldPref);
      }
    }

    if (existingDataVersion < 121) {
      this._migrateHashedKeysForXULStoreForDocument(BROWSER_DOCURL);
      this._migrateHashedKeysForXULStoreForDocument(
        "chrome://browser/content/places/bookmarksSidebar.xhtml"
      );
      this._migrateHashedKeysForXULStoreForDocument(
        "chrome://browser/content/places/historySidebar.xhtml"
      );
    }

    if (existingDataVersion < 122) {
      try {
        const oldPref = "widget.use-xdg-desktop-portal";
        if (Services.prefs.getBoolPref(oldPref)) {
          Services.prefs.setIntPref(
            "widget.use-xdg-desktop-portal.file-picker",
            1
          );
          Services.prefs.setIntPref(
            "widget.use-xdg-desktop-portal.mime-handler",
            1
          );
        }
        Services.prefs.clearUserPref(oldPref);
      } catch (ex) {}
    }


    if (existingDataVersion < 124) {
      const oldFormAutofillModule = "extensions.formautofill.available";
      const oldCreditCardsAvailable =
        "extensions.formautofill.creditCards.available";
      const newCreditCardsAvailable =
        "extensions.formautofill.creditCards.supported";
      const newAddressesAvailable =
        "extensions.formautofill.addresses.supported";
      if (Services.prefs.prefHasUserValue(oldFormAutofillModule)) {
        let moduleAvailability = Services.prefs.getCharPref(
          oldFormAutofillModule
        );
        if (moduleAvailability == "on") {
          Services.prefs.setCharPref(newAddressesAvailable, moduleAvailability);
          Services.prefs.setCharPref(
            newCreditCardsAvailable,
            Services.prefs.getBoolPref(oldCreditCardsAvailable) ? "on" : "off"
          );
        }

        if (moduleAvailability == "off") {
          Services.prefs.setCharPref(
            newCreditCardsAvailable,
            moduleAvailability
          );
          Services.prefs.setCharPref(newAddressesAvailable, moduleAvailability);
        }
      }

      Services.prefs.clearUserPref(oldFormAutofillModule);
      Services.prefs.clearUserPref(oldCreditCardsAvailable);
    }

    if (existingDataVersion < 125) {
      const PIP_PLAYER_URI =
        "chrome://global/content/pictureinpicture/player.xhtml";
      try {
        for (let value of ["left", "top", "width", "height"]) {
          Services.xulStore.removeValue(
            PIP_PLAYER_URI,
            "picture-in-picture",
            value
          );
        }
      } catch (ex) {
        console.error("Failed to clear XULStore PiP values: ", ex);
      }
    }

    function migrateXULAttributeToStyle(url, id, attr) {
      try {
        let value = Services.xulStore.getValue(url, id, attr);
        if (value) {
          Services.xulStore.setValue(url, id, "style", `${attr}: ${value}px;`);
        }
      } catch (ex) {
        console.error(`Error migrating ${id}'s ${attr} value: `, ex);
      }
    }


    if (existingDataVersion < 130) {
      migrateXULAttributeToStyle(BROWSER_DOCURL, "sidebar-box", "width");
    }


    if (existingDataVersion < 132) {
      for (let url of [
        "chrome://browser/content/places/bookmarkProperties.xhtml",
        "chrome://browser/content/places/bookmarkProperties2.xhtml",
      ]) {
        for (let attr of ["width", "screenX", "screenY"]) {
          xulStore.removeValue(url, "bookmarkproperties", attr);
        }
      }
    }

    if (existingDataVersion < 133) {
      xulStore.removeValue(BROWSER_DOCURL, "urlbar-container", "width");
    }


    if (existingDataVersion < 135 && AppConstants.platform == "linux") {
      try {
        if (!Services.prefs.prefHasUserValue("browser.tabs.inTitlebar")) {
          let de = Services.appinfo.desktopEnvironment;
          let oldDefault = de.includes("gnome") || de.includes("pantheon");
          if (!oldDefault) {
            Services.prefs.setIntPref("browser.tabs.inTitlebar", 0);
          }
        }
      } catch (e) {
        console.error("Error migrating tabsInTitlebar setting", e);
      }
    }

    if (existingDataVersion < 136) {
      migrateXULAttributeToStyle(
        "chrome://browser/content/places/places.xhtml",
        "placesList",
        "width"
      );
    }

    if (existingDataVersion < 137) {
      if (
        !Services.prefs.prefHasUserValue("general.smoothScroll") &&
        Services.appinfo.prefersReducedMotion
      ) {
        Services.prefs.setBoolPref("general.smoothScroll", true);
      }
    }

    if (existingDataVersion < 138) {
      try {
        Services.perms
          .getAllByTypes(["https-only-load-insecure"])
          .filter(permission => permission.principal.schemeIs("https"))
          .forEach(permission => {
            const capability = permission.capability;
            const uri = permission.principal.URI.mutate()
              .setScheme("http")
              .finalize();
            const principal =
              Services.scriptSecurityManager.createContentPrincipal(uri, {});
            Services.perms.removePermission(permission);
            Services.perms.addFromPrincipal(
              principal,
              "https-only-load-insecure",
              capability
            );
          });
      } catch (e) {
        console.error("Error migrating https-only-load-insecure permission", e);
      }
    }

    if (existingDataVersion < 139) {
      [
        ["https://www.mozilla.org", "uitour"],
        ["https://support.mozilla.org", "uitour"],
        ["about:home", "uitour"],
        ["about:newtab", "uitour"],
        ["https://addons.mozilla.org", "install"],
        ["https://support.mozilla.org", "remote-troubleshooting"],
        ["about:welcome", "autoplay-media"],
      ].forEach(originInfo => {
        if (
          Services.perms.UNKNOWN_ACTION ==
          Services.perms.testPermissionFromPrincipal(
            Services.scriptSecurityManager.createContentPrincipalFromOrigin(
              originInfo[0]
            ),
            originInfo[1]
          )
        ) {
          Services.perms.addFromPrincipal(
            Services.scriptSecurityManager.createContentPrincipalFromOrigin(
              originInfo[0]
            ),
            originInfo[1],
            Services.perms.ALLOW_ACTION
          );
        }
      });
    }

    if (existingDataVersion < 140) {
      Services.prefs.clearUserPref("browser.fixup.alternate.enabled");
    }

    if (existingDataVersion < 141) {
      for (const filename of ["signons.sqlite", "signons.sqlite.corrupt"]) {
        const filePath = PathUtils.join(PathUtils.profileDir, filename);
        IOUtils.remove(filePath, { ignoreAbsent: true }).catch(console.error);
      }
    }

    if (existingDataVersion < 142) {
      try {
        let value = xulStore.getValue(BROWSER_DOCURL, "sidebar-box", "style");
        if (value) {
          value = value
            .split(";")
            .filter(v => !v.trim().startsWith("--"))
            .join(";");
          xulStore.setValue(BROWSER_DOCURL, "sidebar-box", "style", value);
        }
      } catch (ex) {
        console.error(ex);
      }
    }

    if (existingDataVersion < 143) {
    }

    if (existingDataVersion < 144) {
      for (const filename of [
        "ShutdownDuration.json",
        "ShutdownDuration.json.tmp",
      ]) {
        const filePath = PathUtils.join(PathUtils.profileDir, filename);
        IOUtils.remove(filePath, { ignoreAbsent: true }).catch(console.error);
      }
    }

    let hasRun146Migration = false;
    if (existingDataVersion < 147) {
      const prevOsAuthForCc = !Services.prefs.getBoolPref(
        "signon.management.page.os-auth.enabled",
        false
      );
      const newOSAuthNameForCc =
        "extensions.formautofill.creditCards.os-auth.locked.enabled";
      Services.prefs.setBoolPref(newOSAuthNameForCc, prevOsAuthForCc);
      Services.prefs.lockPref(newOSAuthNameForCc);

      const prevOsAuthForPw = !Services.prefs.getBoolPref(
        "extensions.formautofill.reauth.enabled",
        false
      );
      const newOSAuthNameForPw =
        "signon.management.page.os-auth.locked.enabled";
      Services.prefs.setBoolPref(newOSAuthNameForPw, prevOsAuthForPw);
      Services.prefs.lockPref(newOSAuthNameForPw);

      hasRun146Migration = true;
      Services.prefs.clearUserPref("extensions.formautofill.reauth.enabled");
      Services.prefs.clearUserPref("signon.management.page.os-auth.enabled");
    }

    if (existingDataVersion < 149) {
      [
        "other",
        "script",
        "image",
        "stylesheet",
        "object",
        "document",
        "subdocument",
        "refresh",
        "xbl",
        "ping",
        "xmlhttprequest",
        "objectsubrequest",
        "dtd",
        "font",
        "websocket",
        "csp_report",
        "xslt",
        "beacon",
        "fetch",
        "manifest",
        "speculative",
      ].forEach(type => {
        Services.perms.removeByType(type);
      });
    }

    if (
      existingDataVersion < 153 &&
      Services.prefs.getBoolPref("sidebar.revamp") &&
      !Services.prefs.prefHasUserValue("sidebar.main.tools")
    ) {
      Services.prefs.setCharPref(
        "sidebar.main.tools",
        "syncedtabs,history"
      );
    }

    if (existingDataVersion < 154) {
      const kPref = "browser.handlers.migrations";
      let migrations = Services.prefs
        .getCharPref(kPref, "")
        .split(",")
        .filter(x => !!x);
      migrations.push("mibbit");
      Services.prefs.setCharPref(kPref, migrations.join(","));
    }

    if (existingDataVersion < 155) {
      for (const attr of [
        "checked",
        "positionend",
        "sidebarcommand",
        "style",
      ]) {
        Services.xulStore.removeValue(BROWSER_DOCURL, "sidebar-box", attr);
      }
    }

    if (existingDataVersion < 156) {
      const customBlockListEnabled = Services.prefs.getBoolPref(
        "browser.contentblocking.customBlockList.preferences.ui.enabled",
        false
      );
      if (customBlockListEnabled) {
        Services.prefs.clearUserPref(
          "browser.contentblocking.customBlockList.preferences.ui.enabled"
        );
        Services.prefs.clearUserPref("urlclassifier.trackingTable");
      }
    }

    if (existingDataVersion < 157) {
      if (!hasRun146Migration) {
        const prevOsAuthForCc = !Services.prefs.getStringPref(
          "extensions.formautofill.creditCards.reauth.optout",
          ""
        );
        const prevOsAuthForPw = !Services.prefs.getStringPref(
          "signon.management.page.os-auth.optout",
          ""
        );

        lazy.LoginHelper.setOSAuthEnabled(prevOsAuthForPw);
        lazy.FormAutofillUtils.setOSAuthEnabled(prevOsAuthForCc);

        Services.prefs.clearUserPref(
          "extensions.formautofill.creditCards.reauth.optout"
        );
        Services.prefs.clearUserPref("signon.management.page.os-auth.optout");
      }
    }

    if (AppConstants.NIGHTLY_BUILD && existingDataVersion === 158) {
      lazy.LoginHelper.setOSAuthEnabled(false);
      lazy.FormAutofillUtils.setOSAuthEnabled(false);
    }

    if (existingDataVersion < 159) {
      let menubarWasEnabled =
        Services.xulStore.getValue(
          BROWSER_DOCURL,
          "toolbar-menubar",
          "autohide"
        ) == "false";
      if (menubarWasEnabled) {
        Services.xulStore.setValue(
          BROWSER_DOCURL,
          "toolbar-menubar",
          "autohide",
          "-moz-missing\n"
        );
      }
    }

    if (existingDataVersion < 160) {
      Services.prefs.setBoolPref("signon.reencryptionNeeded", true);
    }

    if (existingDataVersion < 166) {
      try {
        Services.perms.getAllByTypes(["localhost"]).forEach(permission => {
          Services.perms.removePermission(permission);
          Services.perms.addFromPrincipal(
            permission.principal,
            "loopback-network",
            permission.capability,
            permission.expireType,
            permission.expireTime
          );
        });
      } catch (e) {
        console.error("Error migrating localhost permission", e);
      }

      try {
        const oldValue = Services.prefs.getIntPref(
          "permissions.default.localhost"
        );
        Services.prefs.setIntPref(
          "permissions.default.loopback-network",
          oldValue
        );
        Services.prefs.clearUserPref("permissions.default.localhost");
      } catch (e) {}
    }

    if (existingDataVersion < 169) {
      Services.prefs.clearUserPref("widget.macos.native-anchored-menulists");
      Services.prefs.clearUserPref("widget.macos.native-anchored-select");
    }


    if (existingDataVersion < 174) {
      for (let perm of Services.perms.getAllWithTypePrefix(
        "3rdPartyFrameStorage^"
      )) {
        let typeSite = perm.type.substring("3rdPartyFrameStorage^".length);
        try {
          let originSite = Services.eTLD.getSite(perm.principal.URI);
          if (typeSite === originSite) {
            Services.perms.removePermission(perm);
          }
        } catch (e) {
          continue;
        }
      }
    }

    if (existingDataVersion < 175) {
      Services.prefs.setBoolPref("signon.rustMirror.migrationNeeded", true);
    }

    if (existingDataVersion < 176) {
      Services.perms.getAllByTypes(["cookie"]).forEach(p => {
        if (p.expireType != Services.perms.EXPIRE_NEVER) {
          return;
        }
        if (p.capability == Ci.nsICookiePermission.ACCESS_ALLOW) {
          Services.perms.addFromPrincipal(
            p.principal,
            "persist-data-on-shutdown",
            Ci.nsICookiePermission.ACCESS_ALLOW
          );
        }
      });
    }

    Services.prefs.setIntPref("browser.migration.version", newVersion);
  },
};
