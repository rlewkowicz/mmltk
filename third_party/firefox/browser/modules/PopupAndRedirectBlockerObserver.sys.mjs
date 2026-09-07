/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

export var PopupAndRedirectBlockerObserver = {
  mNotificationPromise: null,

  handleEvent(aEvent) {
    switch (aEvent.type) {
      case "DOMUpdateBlockedPopups":
        this.onDOMUpdateBlockedPopupsAndRedirect(aEvent);
        break;
      case "DOMUpdateBlockedRedirect":
        this.onDOMUpdateBlockedPopupsAndRedirect(aEvent);
        break;
      case "command":
        this.onCommand(aEvent);
        break;
      case "popupshowing":
        this.onPopupShowing(aEvent);
        break;
      case "popuphiding":
        this.onPopupHiding(aEvent);
        break;
    }
  },

  onDOMUpdateBlockedPopupsAndRedirect(aEvent) {
    const window = aEvent.originalTarget.documentGlobal;
    const { gBrowser, gPermissionPanel } = window;
    if (aEvent.originalTarget != gBrowser.selectedBrowser) {
      return;
    }

    gPermissionPanel.refreshPermissionIcons();

    const popupCount =
      gBrowser.selectedBrowser.popupAndRedirectBlocker.getBlockedPopupCount();
    const isRedirectBlocked =
      gBrowser.selectedBrowser.popupAndRedirectBlocker.isRedirectBlocked();
    if (!popupCount && !isRedirectBlocked) {
      this.hideNotification(gBrowser);
      return;
    }

    if (Services.prefs.getBoolPref("privacy.popups.showBrowserMessage")) {
      this.ensureInitializedForWindow(window);
      this.showBrowserMessage(gBrowser, popupCount, isRedirectBlocked);
    }
  },

  hideNotification(aBrowser) {
    const notificationBox = aBrowser.getNotificationBox();
    const notification =
      notificationBox.getNotificationWithValue("popup-blocked");
    if (notification) {
      notificationBox.removeNotification(notification);
    }
  },

  ensureInitializedForWindow(aWindow) {
    const popup = aWindow.document.getElementById("blockedPopupOptions");
    if (popup.getAttribute("initialized")) {
      return;
    }

    popup.setAttribute("initialized", true);
    popup.addEventListener("command", this);
    popup.addEventListener("popupshowing", this);
    popup.addEventListener("popuphiding", this);
  },

  async showBrowserMessage(aBrowser, aPopupCount, aIsRedirectBlocked) {
    const selectedBrowser = aBrowser.selectedBrowser;
    const popupAndRedirectBlocker = selectedBrowser.popupAndRedirectBlocker;

    if (popupAndRedirectBlocker.hasBeenDismissed()) {
      return;
    }

    const l10nId = (() => {
      if (aPopupCount >= this.maxReportedPopups) {
        return aIsRedirectBlocked
          ? "popup-warning-exceeded-with-redirect-message"
          : "popup-warning-exceeded-message";
      }

      return aIsRedirectBlocked
        ? "redirect-warning-with-popup-message"
        : "popup-warning-message";
    })();
    const label = {
      "l10n-id": l10nId,
      "l10n-args": {
        popupCount: aPopupCount,
      },
    };
    const notificationBox = aBrowser.getNotificationBox();
    const notification = this.mNotificationPromise
      ? await this.mNotificationPromise
      : notificationBox.getNotificationWithValue("popup-blocked");
    if (notification) {
      notification.label = label;
      return;
    }

    const image = "chrome://browser/skin/notification-icons/popup.svg";
    const priority = notificationBox.PRIORITY_INFO_MEDIUM;
    const eventCallback = popupAndRedirectBlocker.eventCallback.bind(
      popupAndRedirectBlocker
    );

    this.mNotificationPromise = notificationBox.appendNotification(
      "popup-blocked",
      { label, image, priority, eventCallback },
      [
        {
          "l10n-id": "popup-warning-button",
          popup: "blockedPopupOptions",
          callback: null,
        },
      ]
    );
    await this.mNotificationPromise;
    this.mNotificationPromise = null;
  },

  async onPopupShowing(aEvent) {
    const window = aEvent.originalTarget.documentGlobal;
    const { gBrowser, document } = window;

    const browser = gBrowser.selectedBrowser;
    const uriOrPrincipal = browser.isContentPrincipal
      ? browser.contentPrincipal
      : browser.currentURI;
    const uriHost = uriOrPrincipal.asciiHost
      ? uriOrPrincipal.displayHost
      : uriOrPrincipal.spec;

    const blockedPopupAllowSite = document.getElementById(
      "blockedPopupAllowSite"
    );
    if (Services.prefs.prefIsLocked("dom.disable_open_during_load")) {
      blockedPopupAllowSite.setAttribute("hidden", "true");
    } else {
      blockedPopupAllowSite.removeAttribute("hidden");
      document.l10n.setAttributes(
        blockedPopupAllowSite,
        "popups-infobar-allow2",
        { uriHost }
      );
    }

    const blockedPopupDontShowMessage = document.getElementById(
      "blockedPopupDontShowMessage"
    );
    blockedPopupDontShowMessage.removeAttribute("checked");

    gBrowser.selectedBrowser.popupAndRedirectBlocker
      .getBlockedRedirect()
      .then(blockedRedirect => {
        this.onPopupShowingBlockedRedirect(blockedRedirect, window);
      });
    gBrowser.selectedBrowser.popupAndRedirectBlocker
      .getBlockedPopups()
      .then(blockedPopups => {
        this.onPopupShowingBlockedPopups(blockedPopups, window);
      });
  },

  onPopupShowingBlockedRedirect(aBlockedRedirect, aWindow) {
    const { gBrowser, document } = aWindow;
    const browser = gBrowser.selectedBrowser;

    const blockedRedirectSeparator = document.getElementById(
      "blockedRedirectSeparator"
    );
    blockedRedirectSeparator.hidden = !aBlockedRedirect;

    if (!aBlockedRedirect) {
      return;
    }

    const nextElement = blockedRedirectSeparator.nextElementSibling;
    if (nextElement?.hasAttribute("redirectInnerWindowId")) {
      return;
    }

    const menuitem = document.createXULElement("menuitem");
    document.l10n.setAttributes(menuitem, "popup-trigger-redirect-menuitem", {
      redirectURI: aBlockedRedirect.redirectURISpec,
    });
    menuitem.setAttribute("redirectURISpec", aBlockedRedirect.redirectURISpec);
    menuitem.setAttribute(
      "redirectInnerWindowId",
      aBlockedRedirect.innerWindowId
    );
    menuitem.browser = browser;
    menuitem.browsingContext = aBlockedRedirect.browsingContext;

    blockedRedirectSeparator.after(menuitem);
  },

  onPopupShowingBlockedPopups(aBlockedPopups, aWindow) {
    const { gBrowser, document } = aWindow;
    const browser = gBrowser.selectedBrowser;

    const blockedPopupsSeparator = document.getElementById(
      "blockedPopupsSeparator"
    );
    blockedPopupsSeparator.hidden = !aBlockedPopups.length;

    if (!aBlockedPopups.length) {
      return;
    }

    const nextElement = blockedPopupsSeparator.nextElementSibling;
    if (nextElement?.hasAttribute("popupInnerWindowId")) {
      return;
    }

    for (let i = 0; i < aBlockedPopups.length; ++i) {
      const blockedPopup = aBlockedPopups[i];

      const menuitem = document.createXULElement("menuitem");
      document.l10n.setAttributes(menuitem, "popup-show-popup-menuitem", {
        popupURI: blockedPopup.popupWindowURISpec,
      });
      menuitem.setAttribute("popupReportIndex", blockedPopup.reportIndex);
      menuitem.setAttribute("popupInnerWindowId", blockedPopup.innerWindowId);
      menuitem.browser = browser;
      menuitem.browsingContext = blockedPopup.browsingContext;

      blockedPopupsSeparator.after(menuitem);
    }
  },

  onPopupHiding(aEvent) {
    const window = aEvent.originalTarget.documentGlobal;
    const { document } = window;

    const blockedRedirectSeparator = document.getElementById(
      "blockedRedirectSeparator"
    );
    let item = blockedRedirectSeparator.nextElementSibling;
    if (item?.hasAttribute("redirectInnerWindowId")) {
      item.remove();
    }

    const blockedPopupsSeparator = document.getElementById(
      "blockedPopupsSeparator"
    );
    let next = null;
    for (
      item = blockedPopupsSeparator.nextElementSibling;
      item?.hasAttribute("popupInnerWindowId");
      item = next
    ) {
      next = item.nextElementSibling;
      item.remove();
    }
  },

  onCommand(aEvent) {
    if (aEvent.target.hasAttribute("popupReportIndex")) {
      this.showBlockedPopup(aEvent);
      return;
    }

    if (aEvent.target.hasAttribute("redirectURISpec")) {
      this.navigateToBlockedRedirect(aEvent);
      return;
    }

    switch (aEvent.target.id) {
      case "blockedPopupAllowSite":
        this.toggleAllowPopupsForSite(aEvent);
        break;
      case "blockedPopupEdit":
        this.editPopupSettings(aEvent);
        break;
      case "blockedPopupDontShowMessage":
        this.dontShowMessage(aEvent);
        break;
    }
  },

  showBlockedPopup(aEvent) {
    const { browser, browsingContext } = aEvent.target;
    const innerWindowId = aEvent.target.getAttribute("popupInnerWindowId");
    const reportIndex = aEvent.target.getAttribute("popupReportIndex");

    browser.popupAndRedirectBlocker.unblockPopup(
      browsingContext,
      innerWindowId,
      reportIndex
    );
  },

  navigateToBlockedRedirect(aEvent) {
    const { browser, browsingContext } = aEvent.target;
    const innerWindowId = aEvent.target.getAttribute("redirectInnerWindowId");
    const redirectURISpec = aEvent.target.getAttribute("redirectURISpec");

    browser.popupAndRedirectBlocker.unblockRedirect(
      browsingContext,
      innerWindowId,
      redirectURISpec
    );
  },

  async toggleAllowPopupsForSite(aEvent) {
    const window = aEvent.originalTarget.documentGlobal;
    const { gBrowser } = window;

    if (Services.prefs.prefIsLocked("dom.disable_open_during_load")) {
      return;
    }

    Services.perms.addFromPrincipal(
      gBrowser.contentPrincipal,
      "popup",
      Services.perms.ALLOW_ACTION
    );
    gBrowser.getNotificationBox().removeCurrentNotification();

    await gBrowser.selectedBrowser.popupAndRedirectBlocker.unblockAllPopups();
    await gBrowser.selectedBrowser.popupAndRedirectBlocker.unblockFirstRedirect();
  },

  editPopupSettings(aEvent) {
    const window = aEvent.originalTarget.documentGlobal;
    const { openPreferences } = window;

    openPreferences("privacy-permissions-block-popups");
  },

  dontShowMessage(aEvent) {
    const window = aEvent.originalTarget.documentGlobal;
    const { gBrowser } = window;

    Services.prefs.setBoolPref("privacy.popups.showBrowserMessage", false);
    gBrowser.getNotificationBox().removeCurrentNotification();
  },
};

XPCOMUtils.defineLazyPreferenceGetter(
  PopupAndRedirectBlockerObserver,
  "maxReportedPopups",
  "privacy.popups.maxReported"
);
