/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

var gStringBundle = Services.strings.createBundle(
  "chrome://browser/locale/sitePermissions.properties"
);

Services.obs.addObserver(
  {
    observe(subject, _topic, _data) {
      let permission = subject.QueryInterface(Ci.nsIPermission);
      let browserId = permission.browserId;
      if (!browserId) {
        return;
      }
      let bc = BrowsingContext.getCurrentTopByBrowserId(browserId);
      let browser = bc?.embedderElement;
      if (browser?.documentGlobal) {
        browser.dispatchEvent(
          new browser.documentGlobal.CustomEvent("PermissionStateChange")
        );
      }
    },
  },
  "browser-perm-changed"
);

const GloballyBlockedPermissions = {
  _stateByBrowser: new WeakMap(),

  set(browser, id) {
    if (!this._stateByBrowser.has(browser)) {
      this._stateByBrowser.set(browser, {});
    }
    let entry = this._stateByBrowser.get(browser);
    let origin = browser.contentPrincipal.origin;
    if (!entry[origin]) {
      entry[origin] = {};
    }

    if (entry[origin][id]) {
      return false;
    }
    entry[origin][id] = true;

    let { prePath } = browser.currentURI;
    browser.addProgressListener(
      {
        QueryInterface: ChromeUtils.generateQI([
          "nsIWebProgressListener",
          "nsISupportsWeakReference",
        ]),
        onLocationChange(aWebProgress, aRequest, aLocation, aFlags) {
          let hasLeftPage =
            aLocation.prePath != prePath ||
            !(aFlags & Ci.nsIWebProgressListener.LOCATION_CHANGE_SAME_DOCUMENT);
          let isReload = !!(
            aFlags & Ci.nsIWebProgressListener.LOCATION_CHANGE_RELOAD
          );

          if (aWebProgress.isTopLevel && (hasLeftPage || isReload)) {
            GloballyBlockedPermissions.remove(browser, id, origin);
            browser.removeProgressListener(this);
          }
        },
      },
      Ci.nsIWebProgress.NOTIFY_LOCATION
    );
    return true;
  },

  remove(browser, id, origin = null) {
    let entry = this._stateByBrowser.get(browser);
    if (!origin) {
      origin = browser.contentPrincipal.origin;
    }
    if (entry && entry[origin]) {
      delete entry[origin][id];
    }
  },

  getAll(browser) {
    let permissions = [];
    let entry = this._stateByBrowser.get(browser);
    let origin = browser.contentPrincipal.origin;
    if (entry && entry[origin]) {
      let timeStamps = entry[origin];
      for (let id of Object.keys(timeStamps)) {
        permissions.push({
          id,
          state: gPermissions.get(id).getDefault(),
          scope: SitePermissions.SCOPE_GLOBAL,
        });
      }
    }
    return permissions;
  },

  copy(browser, newBrowser) {
    let entry = this._stateByBrowser.get(browser);
    if (entry) {
      this._stateByBrowser.set(newBrowser, entry);
    }
  },
};

export var SitePermissions = {
  UNKNOWN: Services.perms.UNKNOWN_ACTION,
  ALLOW: Services.perms.ALLOW_ACTION,
  BLOCK: Services.perms.DENY_ACTION,
  PROMPT: Services.perms.PROMPT_ACTION,
  ALLOW_COOKIES_FOR_SESSION: Ci.nsICookiePermission.ACCESS_SESSION,
  AUTOPLAY_BLOCKED_ALL: Ci.nsIAutoplay.BLOCKED_ALL,

  SCOPE_REQUEST: "{SitePermissions.SCOPE_REQUEST}",
  SCOPE_TEMPORARY: "{SitePermissions.SCOPE_TEMPORARY}",
  SCOPE_SESSION: "{SitePermissions.SCOPE_SESSION}",
  SCOPE_PERSISTENT: "{SitePermissions.SCOPE_PERSISTENT}",
  SCOPE_GLOBAL: "{SitePermissions.SCOPE_GLOBAL}",

  PERM_KEY_DELIMITER: "^",

  _permissionsArray: null,
  _defaultPrefBranch: Services.prefs.getBranch("permissions.default."),

  getAllByPrincipal(principal) {
    if (!principal) {
      throw new Error("principal argument cannot be null.");
    }
    if (!this.isSupportedPrincipal(principal)) {
      return [];
    }

    let permissions = Services.perms
      .getAllForPrincipal(principal)
      .filter(permission => {
        let entry = gPermissions.get(permission.type);
        return entry && !entry.disabled;
      });

    return permissions.map(permission => {
      let scope = this.SCOPE_PERSISTENT;
      if (permission.expireType == Services.perms.EXPIRE_SESSION) {
        scope = this.SCOPE_SESSION;
      }

      return {
        id: permission.type,
        scope,
        state: permission.capability,
      };
    });
  },

  getAllForBrowser(browser) {
    let permissions = {};

    let browserId = browser.browserId;
    if (browserId && this.isSupportedPrincipal(browser.contentPrincipal)) {
      let bcPerms = Services.perms.getAllForBrowser(
        browser.contentPrincipal,
        browserId
      );
      for (let perm of bcPerms) {
        permissions[perm.type] = {
          id: perm.type,
          state: perm.capability,
          scope: this.SCOPE_TEMPORARY,
        };
      }
    }

    for (let permission of GloballyBlockedPermissions.getAll(browser)) {
      permissions[permission.id] = permission;
    }

    for (let permission of this.getAllByPrincipal(browser.contentPrincipal)) {
      permissions[permission.id] = permission;
    }

    return Object.values(permissions);
  },

  getAllPermissionDetailsForBrowser(browser) {
    return this.getAllForBrowser(browser).map(({ id, scope, state }) => ({
      id,
      scope,
      state,
      label: this.getPermissionLabel(id),
    }));
  },

  isSupportedPrincipal(principal) {
    if (!principal) {
      return false;
    }
    if (!(principal instanceof Ci.nsIPrincipal)) {
      throw new Error(
        "Argument passed as principal is not an instance of Ci.nsIPrincipal"
      );
    }
    return this.isSupportedScheme(principal.scheme);
  },

  isSupportedScheme(scheme) {
    return ["http", "https", "file"].includes(scheme);
  },

  listPermissions() {
    if (this._permissionsArray === null) {
      this._permissionsArray = gPermissions.getEnabledPermissions();
    }
    return this._permissionsArray;
  },

  isSitePermission(type) {
    return gPermissions.has(type);
  },

  invalidatePermissionList() {
    this._permissionsArray = null;
  },

  getAvailableStates(permissionID) {
    if (
      gPermissions.has(permissionID) &&
      gPermissions.get(permissionID).states
    ) {
      return gPermissions.get(permissionID).states;
    }

    if (this.getDefault(permissionID) == this.UNKNOWN) {
      return [
        SitePermissions.UNKNOWN,
        SitePermissions.ALLOW,
        SitePermissions.BLOCK,
      ];
    }

    return [
      SitePermissions.PROMPT,
      SitePermissions.ALLOW,
      SitePermissions.BLOCK,
    ];
  },

  getDefault(permissionID) {
    if (
      gPermissions.has(permissionID) &&
      gPermissions.get(permissionID).getDefault
    ) {
      return gPermissions.get(permissionID).getDefault();
    }

    return this._defaultPrefBranch.getIntPref(permissionID, this.UNKNOWN);
  },

  setDefault(permissionID, state) {
    if (
      gPermissions.has(permissionID) &&
      gPermissions.get(permissionID).setDefault
    ) {
      return gPermissions.get(permissionID).setDefault(state);
    }
    let key = "permissions.default." + permissionID;
    return Services.prefs.setIntPref(key, state);
  },

  getForPrincipal(principal, permissionID, browser) {
    if (!principal && !browser) {
      throw new Error(
        "Atleast one of the arguments, either principal or browser should not be null."
      );
    }
    let defaultState = this.getDefault(permissionID);
    let result = { state: defaultState, scope: this.SCOPE_PERSISTENT };
    if (this.isSupportedPrincipal(principal)) {
      let permission = null;
      if (
        gPermissions.has(permissionID) &&
        gPermissions.get(permissionID).exactHostMatch
      ) {
        permission = Services.perms.getPermissionObject(
          principal,
          permissionID,
          true
        );
      } else {
        permission = Services.perms.getPermissionObject(
          principal,
          permissionID,
          false
        );
      }

      if (permission) {
        result.state = permission.capability;
        if (permission.expireType == Services.perms.EXPIRE_SESSION) {
          result.scope = this.SCOPE_SESSION;
        }
      }
    }

    if (
      result.state == defaultState ||
      result.state == SitePermissions.PROMPT
    ) {
      if (browser) {
        let browserId = browser.browserId;
        if (browserId) {
          let tempPerm = Services.perms.getForBrowser(
            principal ?? browser.contentPrincipal,
            permissionID,
            browserId
          );
          if (tempPerm) {
            result.state = tempPerm.capability;
            result.scope = this.SCOPE_TEMPORARY;
          }
        }
      }
    }

    return result;
  },

  setForPrincipal(
    principal,
    permissionID,
    state,
    scope = this.SCOPE_PERSISTENT,
    browser = null,
    expireTimeMS = SitePermissions.temporaryPermissionExpireTime
  ) {
    if (!principal && !browser) {
      throw new Error(
        "Atleast one of the arguments, either principal or browser should not be null."
      );
    }
    if (scope == this.SCOPE_GLOBAL && state == this.BLOCK) {
      if (GloballyBlockedPermissions.set(browser, permissionID)) {
        browser.dispatchEvent(
          new browser.documentGlobal.CustomEvent("PermissionStateChange")
        );
      }
      return;
    }

    if (state == this.UNKNOWN || state == this.getDefault(permissionID)) {
      if (permissionID != "cookie") {
        this.removeFromPrincipal(principal, permissionID, browser);
        return;
      }
    }

    if (state == this.ALLOW_COOKIES_FOR_SESSION && permissionID != "cookie") {
      throw new Error(
        "ALLOW_COOKIES_FOR_SESSION can only be set on the cookie permission"
      );
    }

    if (scope == this.SCOPE_TEMPORARY) {
      if (!browser) {
        throw new Error(
          "TEMPORARY scoped permissions require a browser object"
        );
      }
      if (!Number.isInteger(expireTimeMS) || expireTimeMS <= 0) {
        throw new Error("expireTime must be a positive integer");
      }

      let browserId = browser.browserId;
      if (browserId) {
        Services.perms.addFromPrincipalForBrowser(
          principal ?? browser.contentPrincipal,
          permissionID,
          state,
          browserId,
          expireTimeMS
        );
      }
    } else if (this.isSupportedPrincipal(principal)) {
      let perms_scope = Services.perms.EXPIRE_NEVER;
      if (scope == this.SCOPE_SESSION) {
        perms_scope = Services.perms.EXPIRE_SESSION;
      }

      Services.perms.addFromPrincipal(
        principal,
        permissionID,
        state,
        perms_scope
      );
    }
  },

  removeFromPrincipal(principal, permissionID, browser) {
    if (!principal && !browser) {
      throw new Error(
        "Atleast one of the arguments, either principal or browser should not be null."
      );
    }
    if (this.isSupportedPrincipal(principal)) {
      Services.perms.removeFromPrincipal(principal, permissionID);
    }

    if (browser) {
      let browserId = browser.browserId;
      if (browserId) {
        Services.perms.removeFromPrincipalForBrowser(
          principal ?? browser.contentPrincipal,
          permissionID,
          browserId
        );
      }
    }
  },

  clearTemporaryBlockPermissions(browser) {
    let browserId = browser.browserId;
    if (browserId) {
      Services.perms.removeByActionForBrowser(
        browserId,
        Services.perms.DENY_ACTION
      );
    }
  },

  copyTemporaryPermissions(srcBrowserId, srcBrowser, destBrowser) {
    let destBrowserId = destBrowser.browserId;
    if (srcBrowserId && destBrowserId && srcBrowserId !== destBrowserId) {
      Services.perms.copyBrowserPermissions(srcBrowserId, destBrowserId);
    }
    GloballyBlockedPermissions.copy(srcBrowser, destBrowser);
  },

  getPermissionLabel(permissionID) {
    let [id, key] = permissionID.split(this.PERM_KEY_DELIMITER);
    if (!gPermissions.has(id)) {
      return null;
    }
    if (
      "labelID" in gPermissions.get(id) &&
      gPermissions.get(id).labelID === null
    ) {
      return null;
    }
    if (id == "3rdPartyStorage" || id == "3rdPartyFrameStorage") {
      return key;
    }
    let labelID = gPermissions.get(id).labelID || id;
    return gStringBundle.formatStringFromName(`permission.${labelID}.label`, [
      key,
    ]);
  },

  getMultichoiceStateLabel(permissionID, state) {
    if (
      gPermissions.has(permissionID) &&
      gPermissions.get(permissionID).getMultichoiceStateLabel
    ) {
      return gPermissions.get(permissionID).getMultichoiceStateLabel(state);
    }

    switch (state) {
      case this.UNKNOWN:
      case this.PROMPT:
        return gStringBundle.GetStringFromName("state.multichoice.alwaysAsk");
      case this.ALLOW:
        return gStringBundle.GetStringFromName("state.multichoice.allow");
      case this.ALLOW_COOKIES_FOR_SESSION:
        return gStringBundle.GetStringFromName(
          "state.multichoice.allowForSession"
        );
      case this.BLOCK:
        return gStringBundle.GetStringFromName("state.multichoice.block");
      default:
        return null;
    }
  },

  getCurrentStateLabel(state, id, scope = null) {
    switch (state) {
      case this.PROMPT:
        return gStringBundle.GetStringFromName("state.current.prompt");
      case this.ALLOW:
        if (
          scope &&
          scope != this.SCOPE_PERSISTENT
        ) {
          return gStringBundle.GetStringFromName(
            "state.current.allowedTemporarily"
          );
        }
        return gStringBundle.GetStringFromName("state.current.allowed");
      case this.ALLOW_COOKIES_FOR_SESSION:
        return gStringBundle.GetStringFromName(
          "state.current.allowedForSession"
        );
      case this.BLOCK:
        if (
          scope &&
          scope != this.SCOPE_PERSISTENT &&
          scope != this.SCOPE_GLOBAL
        ) {
          return gStringBundle.GetStringFromName(
            "state.current.blockedTemporarily"
          );
        }
        return gStringBundle.GetStringFromName("state.current.blocked");
      default:
        return null;
    }
  },
};

let gPermissions = {
  _getId(type) {
    let [id] = type.split(SitePermissions.PERM_KEY_DELIMITER);
    return id;
  },

  has(type) {
    return this._getId(type) in this._permissions;
  },

  get(type) {
    let id = this._getId(type);
    let perm = this._permissions[id];
    if (perm) {
      perm.id = id;
    }
    return perm;
  },

  getEnabledPermissions() {
    return Object.keys(this._permissions).filter(
      id => !this._permissions[id].disabled
    );
  },

  _permissions: {
    "autoplay-media": {
      exactHostMatch: true,
      getDefault() {
        let pref = Services.prefs.getIntPref(
          "media.autoplay.default",
          Ci.nsIAutoplay.BLOCKED
        );
        if (pref == Ci.nsIAutoplay.ALLOWED) {
          return SitePermissions.ALLOW;
        }
        if (pref == Ci.nsIAutoplay.BLOCKED_ALL) {
          return SitePermissions.AUTOPLAY_BLOCKED_ALL;
        }
        return SitePermissions.BLOCK;
      },
      setDefault(value) {
        let prefValue = Ci.nsIAutoplay.BLOCKED;
        if (value == SitePermissions.ALLOW) {
          prefValue = Ci.nsIAutoplay.ALLOWED;
        } else if (value == SitePermissions.AUTOPLAY_BLOCKED_ALL) {
          prefValue = Ci.nsIAutoplay.BLOCKED_ALL;
        }
        Services.prefs.setIntPref("media.autoplay.default", prefValue);
      },
      labelID: "autoplay",
      states: [
        SitePermissions.ALLOW,
        SitePermissions.BLOCK,
        SitePermissions.AUTOPLAY_BLOCKED_ALL,
      ],
      getMultichoiceStateLabel(state) {
        switch (state) {
          case SitePermissions.AUTOPLAY_BLOCKED_ALL:
            return gStringBundle.GetStringFromName(
              "state.multichoice.autoplayblockall"
            );
          case SitePermissions.BLOCK:
            return gStringBundle.GetStringFromName(
              "state.multichoice.autoplayblock"
            );
          case SitePermissions.ALLOW:
            return gStringBundle.GetStringFromName(
              "state.multichoice.autoplayallow"
            );
        }
        throw new Error(`Unknown state: ${state}`);
      },
    },

    cookie: {
      states: [
        SitePermissions.ALLOW,
        SitePermissions.ALLOW_COOKIES_FOR_SESSION,
        SitePermissions.BLOCK,
      ],
      getDefault() {
        if (
          Services.cookies.getCookieBehavior(false) ==
          Ci.nsICookieService.BEHAVIOR_REJECT
        ) {
          return SitePermissions.BLOCK;
        }

        return SitePermissions.ALLOW;
      },
    },

    "loopback-network": {
      exactHostMatch: true,
      labelID: "localhost",
      get disabled() {
        return !SitePermissions.localNetworkAccessPermissionsEnabled;
      },
    },

    "local-network": {
      exactHostMatch: true,
      get disabled() {
        return !SitePermissions.localNetworkAccessPermissionsEnabled;
      },
    },

    popup: {
      get labelID() {
        if (
          SitePermissions.popupBlockerEnabled &&
          !SitePermissions.framebustingInterventionEnabled
        ) {
          return "popup-only";
        }
        if (
          !SitePermissions.popupBlockerEnabled &&
          SitePermissions.framebustingInterventionEnabled
        ) {
          return "framebusting-only";
        }
        return "popup-and-framebusting";
      },
      states: [SitePermissions.ALLOW, SitePermissions.BLOCK],
      get disabled() {
        return (
          !SitePermissions.popupBlockerEnabled &&
          !SitePermissions.framebustingInterventionEnabled
        );
      },
      getDefault() {
        return SitePermissions.BLOCK;
      },
    },

    "open-protocol-handler": {
      labelID: "open-protocol-handler",
      exactHostMatch: true,
      states: [SitePermissions.UNKNOWN, SitePermissions.ALLOW],
    },

    "focus-tab-by-prompt": {
      exactHostMatch: true,
      states: [SitePermissions.UNKNOWN, SitePermissions.ALLOW],
    },
    "persistent-storage": {
      exactHostMatch: true,
    },

    "persist-data-on-shutdown": {
      exactHostMatch: false,
      get disabled() {
        return !SitePermissions.sanitizeOnShutdownEnabled;
      },
      getDefault() {
        return SitePermissions.UNKNOWN;
      },
      states: [SitePermissions.UNKNOWN, SitePermissions.ALLOW],
      getMultichoiceStateLabel(state) {
        switch (state) {
          case SitePermissions.UNKNOWN:
            return gStringBundle.GetStringFromName(
              "state.multichoice.clearDataOnShutdown"
            );
          case SitePermissions.ALLOW:
            return gStringBundle.GetStringFromName(
              "state.multichoice.persistDataOnShutdown"
            );
        }
        throw new Error(`Unknown state: ${state}`);
      },
    },

    shortcuts: {
      states: [SitePermissions.ALLOW, SitePermissions.BLOCK],
    },

    canvas: {
      get disabled() {
        return !SitePermissions.resistFingerprinting;
      },
    },

    "storage-access": {
      labelID: null,
      getDefault() {
        return SitePermissions.UNKNOWN;
      },
    },

    "3rdPartyStorage": {},
    "3rdPartyFrameStorage": {},
  },
};

XPCOMUtils.defineLazyPreferenceGetter(
  SitePermissions,
  "temporaryPermissionExpireTime",
  "privacy.temporary_permission_expire_time_ms",
  3600 * 1000
);
XPCOMUtils.defineLazyPreferenceGetter(
  SitePermissions,
  "resistFingerprinting",
  "privacy.resistFingerprinting",
  false,
  SitePermissions.invalidatePermissionList.bind(SitePermissions)
);

XPCOMUtils.defineLazyPreferenceGetter(
  SitePermissions,
  "localNetworkAccessPermissionsEnabled",
  "network.lna.blocking",
  false,
  SitePermissions.invalidatePermissionList.bind(SitePermissions)
);

XPCOMUtils.defineLazyPreferenceGetter(
  SitePermissions,
  "sanitizeOnShutdownEnabled",
  "privacy.sanitize.sanitizeOnShutdown",
  false,
  SitePermissions.invalidatePermissionList.bind(SitePermissions)
);
XPCOMUtils.defineLazyPreferenceGetter(
  SitePermissions,
  "popupBlockerEnabled",
  "dom.disable_open_during_load",
  true,
  SitePermissions.invalidatePermissionList.bind(SitePermissions)
);
XPCOMUtils.defineLazyPreferenceGetter(
  SitePermissions,
  "framebustingInterventionEnabled",
  "dom.security.framebusting_intervention.enabled",
  true,
  SitePermissions.invalidatePermissionList.bind(SitePermissions)
);
