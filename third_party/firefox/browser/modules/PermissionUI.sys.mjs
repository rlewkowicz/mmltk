/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  SitePermissions: "resource:///modules/SitePermissions.sys.mjs",
  clearTimeout: "resource://gre/modules/Timer.sys.mjs",
  setTimeout: "resource://gre/modules/Timer.sys.mjs",
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "IDNService",
  "@mozilla.org/network/idn-service;1",
  Ci.nsIIDNService
);
ChromeUtils.defineLazyGetter(lazy, "gBrandBundle", function () {
  return Services.strings.createBundle(
    "chrome://branding/locale/brand.properties"
  );
});

ChromeUtils.defineLazyGetter(lazy, "gBrowserBundle", function () {
  return Services.strings.createBundle(
    "chrome://browser/locale/browser.properties"
  );
});

ChromeUtils.defineLazyGetter(lazy, "gFluentStrings", function () {
  return new Localization(["browser/permissions.ftl"], true );
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "lnaPromptTimeoutMs",
  "network.lna.prompt.timeout",
  300000
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "lnaTemporaryPermissionExpireTimeMs",
  "network.lna.temporary_permission_expire_time_ms",
  24 * 3600 * 1000 
);

class PermissionPrompt {
  get browser() {
    throw new Error("Not implemented.");
  }

  get principal() {
    throw new Error("Not implemented.");
  }

  get type() {
    return undefined;
  }

  get permissionKey() {
    return undefined;
  }

  get usePermissionManager() {
    return true;
  }

  get temporaryPermissionURI() {
    return undefined;
  }

  get temporaryPermissionExpireTimeMS() {
    return undefined;
  }

  get popupOptions() {
    return {};
  }

  get postPromptEnabled() {
    return false;
  }

  get requiresUserInput() {
    return false;
  }

  get notificationID() {
    throw new Error("Not implemented.");
  }

  get anchorID() {
    return "default-notification-icon";
  }

  get message() {
    throw new Error("Not implemented.");
  }

  get hintText() {
    return undefined;
  }

  getPrincipalName(principal = this.principal) {
    return principal.hostPort;
  }

  cancel() {
    throw new Error("Not implemented.");
  }

  allow() {
    throw new Error("Not implemented.");
  }

  get promptActions() {
    return [];
  }

  get postPromptActions() {
    return null;
  }

  onBeforeShow() {
    return true;
  }

  onBeforeShowAsync() {}

  onShown() {}

  onAfterShow() {}

  async prompt() {
    let requestingURI = this.principal.URI;
    if (!(requestingURI instanceof Ci.nsIStandardURL)) {
      return;
    }

    if (this.usePermissionManager && this.permissionKey) {
      let { state } = lazy.SitePermissions.getForPrincipal(
        this.principal,
        this.permissionKey,
        this.browser,
        this.temporaryPermissionURI
      );

      if (state == lazy.SitePermissions.BLOCK) {
        this.cancel();
        return;
      }

      if (
        state == lazy.SitePermissions.ALLOW &&
        !this.request.isRequestDelegatedToUnsafeThirdParty &&
        !this.request.ignoreAllowSitePermission
      ) {
        this.allow();
        return;
      }
    } else if (this.permissionKey) {
      let { state } = lazy.SitePermissions.getForPrincipal(
        null,
        this.permissionKey,
        this.browser,
        this.temporaryPermissionURI
      );

      if (state == lazy.SitePermissions.BLOCK) {
        this.cancel();
        return;
      }
    }

    if (
      this.requiresUserInput &&
      !this.request.hasValidTransientUserGestureActivation
    ) {
      if (this.postPromptEnabled) {
        let beforeShow = this.onBeforeShowAsync();
        if (beforeShow) {
          await beforeShow;
        }
        this.postPrompt();
      }
      this.cancel();
      return;
    }

    let chromeWin = this.browser.documentGlobal;
    if (!chromeWin.PopupNotifications) {
      this.cancel();
      return;
    }

    let beforeShow = this.onBeforeShowAsync();
    if (beforeShow) {
      await beforeShow;
    }

    let popupNotificationActions = [];
    for (let promptAction of this.promptActions) {
      let action = {
        label: promptAction.label,
        accessKey: promptAction.accessKey,
        callback: state => {
          if (promptAction.callback) {
            promptAction.callback();
          }

          if (this.usePermissionManager && this.permissionKey) {
            if (
              (state && state.checkboxChecked && state.source != "esc-press") ||
              promptAction.scope == lazy.SitePermissions.SCOPE_PERSISTENT
            ) {
              let scope = lazy.SitePermissions.SCOPE_PERSISTENT;
              if (lazy.PrivateBrowsingUtils.isBrowserPrivate(this.browser)) {
                scope = lazy.SitePermissions.SCOPE_SESSION;
              }
              lazy.SitePermissions.setForPrincipal(
                this.principal,
                this.permissionKey,
                promptAction.action,
                scope
              );
            } else {
              lazy.SitePermissions.setForPrincipal(
                this.principal,
                this.permissionKey,
                promptAction.action,
                lazy.SitePermissions.SCOPE_TEMPORARY,
                this.browser,
                this.temporaryPermissionExpireTimeMS
              );
            }

            if (promptAction.action == lazy.SitePermissions.ALLOW) {
              this.allow();
            } else {
              this.cancel();
            }
          } else if (this.permissionKey) {
            lazy.SitePermissions.setForPrincipal(
              null,
              this.permissionKey,
              promptAction.action,
              lazy.SitePermissions.SCOPE_TEMPORARY,
              this.browser,
              this.temporaryPermissionExpireTimeMS
            );
          }
        },
      };
      if (promptAction.dismiss) {
        action.dismiss = promptAction.dismiss;
      }

      if (promptAction.action === lazy.SitePermissions.BLOCK) {
        action.disableSecurityDelay = true;
      }

      popupNotificationActions.push(action);
    }

    this.#showNotification(popupNotificationActions);
  }

  postPrompt() {
    let browser = this.browser;
    let principal = this.principal;
    let chromeWin = browser.documentGlobal;
    if (!chromeWin.PopupNotifications) {
      return;
    }

    if (!this.permissionKey) {
      throw new Error("permissionKey is required to show a post-prompt");
    }

    if (!this.postPromptActions) {
      throw new Error("postPromptActions are required to show a post-prompt");
    }

    let popupNotificationActions = [];
    for (let promptAction of this.postPromptActions) {
      let action = {
        label: promptAction.label,
        accessKey: promptAction.accessKey,
        callback: () => {
          if (promptAction.callback) {
            promptAction.callback();
          }

          let scope = lazy.SitePermissions.SCOPE_PERSISTENT;
          if (lazy.PrivateBrowsingUtils.isBrowserPrivate(browser)) {
            scope = lazy.SitePermissions.SCOPE_SESSION;
          }
          lazy.SitePermissions.setForPrincipal(
            principal,
            this.permissionKey,
            promptAction.action,
            scope
          );
        },
      };
      popupNotificationActions.push(action);
    }

    if (!chromeWin.gReduceMotion) {
      let anchor = chromeWin.document.getElementById(this.anchorID);
      anchor.addEventListener(
        "animationend",
        () => anchor.removeAttribute("animate"),
        { once: true }
      );
      anchor.setAttribute("animate", "true");
    }

    this.#showNotification(popupNotificationActions, true);
  }

  #showNotification(actions, postPrompt = false) {
    let chromeWin = this.browser.documentGlobal;
    let mainAction = actions.length ? actions[0] : null;
    let secondaryActions = actions.splice(1);

    let options = this.popupOptions;

    if (!options.hasOwnProperty("displayURI") || options.displayURI) {
      options.displayURI = this.principal.URI;
    }

    if (!postPrompt) {
      options.persistent = true;
      options.hideClose = true;
    }

    options.eventCallback = (topic, nextRemovalReason, withoutUserResponse) => {
      if (topic == "swapping") {
        return true;
      }
      if (topic == "shown" && !postPrompt) {
        this.onShown();
      }
      if (topic == "removed" && !postPrompt) {
        if (withoutUserResponse) {
          this.cancel();
        }
        this.onAfterShow();
      }
      return false;
    };

    options.hintText = this.hintText;
    options.dismissed = postPrompt;

    if (postPrompt || this.onBeforeShow() !== false) {
      chromeWin.PopupNotifications.show(
        this.browser,
        this.notificationID,
        this.message,
        this.anchorID,
        mainAction,
        secondaryActions,
        options
      );
    }
  }
}

class PermissionPromptForRequest extends PermissionPrompt {
  get browser() {
    if (this.request.element) {
      return this.request.element;
    }
    return this.request.window.docShell.chromeEventHandler;
  }

  get principal() {
    let request = this.request.QueryInterface(Ci.nsIContentPermissionRequest);
    return request.getDelegatePrincipal(this.type);
  }

  cancel() {
    this.request.cancel();
  }

  allow(choices) {
    this.request.allow(choices);
  }
}


class LNAPermissionPromptBase extends PermissionPromptForRequest {
  static DEFAULT_PROMPT_TIMEOUT_MS = 300000;

  #timeoutTimer = null;

  constructor(request) {
    super();
    this.request = request;
  }

  onBeforeShow() {
    if (typeof this.request.notifyShown === "function") {
      this.request.notifyShown();
    }
    return true;
  }

  onShown() {
    this.#startTimeoutTimer();
  }

  onAfterShow() {
    this.#clearTimeoutTimer();
  }

  cancel() {
    super.cancel();
  }

  allow(choices) {
    super.allow(choices);
  }

  get temporaryPermissionExpireTimeMS() {
    return lazy.lnaTemporaryPermissionExpireTimeMs;
  }

  #startTimeoutTimer() {
    this.#clearTimeoutTimer();

    this.#timeoutTimer = lazy.setTimeout(() => {
      let scriptError = Cc["@mozilla.org/scripterror;1"].createInstance(
        Ci.nsIScriptError
      );
      scriptError.initWithWindowID(
        `LNA permission prompt timed out after ${lazy.lnaPromptTimeoutMs / 1000} seconds`,
        null,
        0,
        0,
        Ci.nsIScriptError.warningFlag,
        "content javascript",
        this.browser.browsingContext.currentWindowGlobal.innerWindowId
      );
      Services.console.logMessage(scriptError);

      this.#removePrompt();
      this.cancel();
    }, lazy.lnaPromptTimeoutMs);
  }

  #removePrompt() {
    let chromeWin = this.browser?.documentGlobal;
    let notification = chromeWin?.PopupNotifications.getNotification(
      this.notificationID,
      this.browser
    );
    if (notification) {
      chromeWin.PopupNotifications.remove(notification);
    }
  }

  #clearTimeoutTimer() {
    if (this.#timeoutTimer) {
      lazy.clearTimeout(this.#timeoutTimer);
      this.#timeoutTimer = null;
    }
  }
}

class LoopbackNetworkPermissionPrompt extends LNAPermissionPromptBase {
  get type() {
    return "loopback-network";
  }

  get permissionKey() {
    return "loopback-network";
  }

  get popupOptions() {
    let options = {
      learnMoreURL: Services.urlFormatter.formatURLPref(
        "browser.lna.warning.infoURL"
      ),
      displayURI: false,
      name: this.getPrincipalName(),
    };

    options.checkbox = {
      show: !lazy.PrivateBrowsingUtils.isWindowPrivate(
        this.browser.documentGlobal
      ),
    };

    if (options.checkbox.show) {
      options.checkbox.label = lazy.gBrowserBundle.GetStringFromName(
        "localhost.remember2"
      );
    }

    return options;
  }

  get notificationID() {
    return "loopback-network";
  }

  get anchorID() {
    return "loopback-network-notification-icon";
  }

  get message() {
    return lazy.gBrowserBundle.formatStringFromName(
      "localhost.allowWithSite2",
      ["<>"]
    );
  }

  get promptActions() {
    return [
      {
        label: lazy.gBrowserBundle.GetStringFromName("localhost.allowlabel"),
        accessKey: lazy.gBrowserBundle.GetStringFromName(
          "localhost.allow.accesskey"
        ),
        action: lazy.SitePermissions.ALLOW,
      },
      {
        label: lazy.gBrowserBundle.GetStringFromName("localhost.blocklabel"),
        accessKey: lazy.gBrowserBundle.GetStringFromName(
          "localhost.block.accesskey"
        ),
        action: lazy.SitePermissions.BLOCK,
      },
    ];
  }
}


class LocalNetworkPermissionPrompt extends LNAPermissionPromptBase {
  get type() {
    return "local-network";
  }

  get permissionKey() {
    return "local-network";
  }

  get popupOptions() {
    let options = {
      learnMoreURL: Services.urlFormatter.formatURLPref(
        "browser.lna.warning.infoURL"
      ),
      displayURI: false,
      name: this.getPrincipalName(),
    };

    options.checkbox = {
      show: !lazy.PrivateBrowsingUtils.isWindowPrivate(
        this.browser.documentGlobal
      ),
    };

    if (options.checkbox.show) {
      options.checkbox.label = lazy.gBrowserBundle.GetStringFromName(
        "localNetwork.remember2"
      );
    }

    return options;
  }

  get notificationID() {
    return "local-network";
  }

  get anchorID() {
    return "local-network-notification-icon";
  }

  get message() {
    return lazy.gBrowserBundle.formatStringFromName(
      "localNetwork.allowWithSite2",
      ["<>"]
    );
  }

  get promptActions() {
    return [
      {
        label: lazy.gBrowserBundle.GetStringFromName("localNetwork.allowLabel"),
        accessKey: lazy.gBrowserBundle.GetStringFromName(
          "localNetwork.allow.accesskey"
        ),
        action: lazy.SitePermissions.ALLOW,
      },
      {
        label: lazy.gBrowserBundle.GetStringFromName("localNetwork.blockLabel"),
        accessKey: lazy.gBrowserBundle.GetStringFromName(
          "localNetwork.block.accesskey"
        ),
        action: lazy.SitePermissions.BLOCK,
      },
    ];
  }
}
class PersistentStoragePermissionPrompt extends PermissionPromptForRequest {
  constructor(request) {
    super();
    this.request = request;
  }

  get type() {
    return "persistent-storage";
  }

  get permissionKey() {
    return "persistent-storage";
  }

  get popupOptions() {
    let learnMoreURL =
      Services.urlFormatter.formatURLPref("app.support.baseURL") +
      "storage-permissions";
    let options = {
      learnMoreURL,
      displayURI: false,
      name: this.getPrincipalName(),
    };

    options.checkbox = {
      show: !lazy.PrivateBrowsingUtils.isWindowPrivate(
        this.browser.documentGlobal
      ),
    };

    if (options.checkbox.show) {
      options.checkbox.label = lazy.gFluentStrings.formatValueSync(
        "perm-persistent-storage-remember"
      );
    }

    return options;
  }

  get notificationID() {
    return "persistent-storage";
  }

  get anchorID() {
    return "persistent-storage-notification-icon";
  }

  get message() {
    return lazy.gBrowserBundle.formatStringFromName(
      "persistentStorage.allowWithSite2",
      ["<>"]
    );
  }

  get promptActions() {
    return [
      {
        label: lazy.gBrowserBundle.GetStringFromName("persistentStorage.allow"),
        accessKey: lazy.gBrowserBundle.GetStringFromName(
          "persistentStorage.allow.accesskey"
        ),
        action: Ci.nsIPermissionManager.ALLOW_ACTION,
        scope: lazy.SitePermissions.SCOPE_PERSISTENT,
      },
      {
        label: lazy.gBrowserBundle.GetStringFromName(
          "persistentStorage.block.label"
        ),
        accessKey: lazy.gBrowserBundle.GetStringFromName(
          "persistentStorage.block.accesskey"
        ),
        action: lazy.SitePermissions.BLOCK,
      },
    ];
  }
}


class StorageAccessPermissionPrompt extends PermissionPromptForRequest {
  #permissionKey;

  constructor(request) {
    super();
    this.request = request;
    this.siteOption = null;
    this.#permissionKey = `3rdPartyStorage${lazy.SitePermissions.PERM_KEY_DELIMITER}${this.principal.origin}`;

    let types = this.request.types.QueryInterface(Ci.nsIArray);
    let perm = types.queryElementAt(0, Ci.nsIContentPermissionType);
    let options = perm.options.QueryInterface(Ci.nsIArray);
    if (options.length != 2) {
      return;
    }

    let topLevelOption = options.queryElementAt(0, Ci.nsISupportsString).data;
    if (topLevelOption) {
      this.siteOption = topLevelOption;
    }
    let frameOption = options.queryElementAt(1, Ci.nsISupportsString).data;
    if (frameOption) {
      this.#permissionKey = `3rdPartyFrameStorage${lazy.SitePermissions.PERM_KEY_DELIMITER}${this.principal.siteOrigin}`;
    }
  }

  get usePermissionManager() {
    return false;
  }

  get type() {
    return "storage-access";
  }

  get permissionKey() {
    return this.#permissionKey;
  }

  get temporaryPermissionURI() {
    if (this.siteOption) {
      return Services.io.newURI(this.siteOption);
    }
    return undefined;
  }

  prettifyHostPort(hostport) {
    let [host, port] = hostport.split(":");
    host = lazy.IDNService.convertToDisplayIDN(host);
    if (port) {
      return `${host}:${port}`;
    }
    return host;
  }

  get popupOptions() {
    let learnMoreURL =
      Services.urlFormatter.formatURLPref("app.support.baseURL") +
      "third-party-cookies";
    let hostPort = this.prettifyHostPort(this.principal.hostPort);
    let hintText = lazy.gBrowserBundle.formatStringFromName(
      "storageAccess1.hintText",
      [hostPort]
    );
    return {
      learnMoreURL,
      displayURI: false,
      hintText,
      escAction: "secondarybuttoncommand",
    };
  }

  get notificationID() {
    return "storage-access";
  }

  get anchorID() {
    return "storage-access-notification-icon";
  }

  get message() {
    let embeddingHost = this.topLevelPrincipal.host;

    if (this.siteOption) {
      embeddingHost = this.siteOption.split("://").at(-1);
    }

    return lazy.gBrowserBundle.formatStringFromName("storageAccess4.message", [
      this.prettifyHostPort(this.principal.hostPort),
      this.prettifyHostPort(embeddingHost),
    ]);
  }

  get promptActions() {
    let self = this;

    return [
      {
        label: lazy.gBrowserBundle.GetStringFromName(
          "storageAccess1.Allow.label"
        ),
        accessKey: lazy.gBrowserBundle.GetStringFromName(
          "storageAccess1.Allow.accesskey"
        ),
        action: Ci.nsIPermissionManager.ALLOW_ACTION,
        callback() {
          self.allow({ "storage-access": "allow" });
        },
      },
      {
        label: lazy.gBrowserBundle.GetStringFromName(
          "storageAccess1.DontAllow.label"
        ),
        accessKey: lazy.gBrowserBundle.GetStringFromName(
          "storageAccess1.DontAllow.accesskey"
        ),
        action: Ci.nsIPermissionManager.DENY_ACTION,
        callback() {
          self.cancel();
        },
      },
    ];
  }

  get topLevelPrincipal() {
    return this.request.topLevelPrincipal;
  }
}

export const PermissionUI = {
  PermissionPromptForRequest,
  PersistentStoragePermissionPrompt,
  StorageAccessPermissionPrompt,
  LoopbackNetworkPermissionPrompt,
  LocalNetworkPermissionPrompt,
};
