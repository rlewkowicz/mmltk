/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  CustomizableUI:
    "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs",
  IPPExceptionsManager:
    "moz-src:///toolkit/components/ipprotection/IPPExceptionsManager.sys.mjs",
  IPPPrincipalRules:
    "moz-src:///toolkit/components/ipprotection/IPPExceptionsManager.sys.mjs",
  IPPProxyManager:
    "moz-src:///toolkit/components/ipprotection/IPPProxyManager.sys.mjs",
  IPProtectionService:
    "moz-src:///toolkit/components/ipprotection/IPProtectionService.sys.mjs",
  IPPProxyStates:
    "moz-src:///toolkit/components/ipprotection/IPPProxyManager.sys.mjs",
  ERRORS: "moz-src:///toolkit/components/ipprotection/IPPProxyManager.sys.mjs",
});

const OPENED_WITH_LOCATION_PREF =
  "browser.ipProtection.openedPanelWithLocation";

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "siteExceptionsFeaturePref",
  "browser.ipProtection.features.siteExceptions",
  false
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "siteExceptionsHintsPref",
  "browser.ipProtection.siteExceptionsHintsEnabled",
  true
);

export class IPProtectionToolbarButton {
  #window = null;
  #progressListener = null;
  #widgetId = null;
  #previousIsExcluded = null;
  #prefObserver = null;
  #visitedExcludedSites = new Set();

  static CONFIRMATION_HINT_MESSAGE_ID =
    "confirmation-hint-ipprotection-navigated-to-excluded-site";

  get gBrowser() {
    const win = this.#window.get();
    return win?.gBrowser;
  }

  get isExceptionsFeatureEnabled() {
    return lazy.siteExceptionsFeaturePref;
  }

  get isExceptionsHintsEnabled() {
    return lazy.siteExceptionsHintsPref;
  }

  get toolbaritem() {
    const win = this.#window.get();
    if (!win) {
      return null;
    }

    return lazy.CustomizableUI.getWidget(this.#widgetId)?.forWindow(win).node;
  }

  constructor(window, widgetId, toolbaritem = null) {
    this.#window = Cu.getWeakReference(window);
    this.#widgetId = widgetId;
    this.handleEvent = this.#handleEvent.bind(this);

    this.#addProgressListener();
    lazy.IPProtectionService.addEventListener(
      "IPProtectionService:StateChanged",
      this.handleEvent
    );
    lazy.IPPProxyManager.addEventListener(
      "IPPProxyManager:StateChanged",
      this.handleEvent
    );
    lazy.IPPExceptionsManager.addEventListener(
      "IPPExceptionsManager:ExclusionChanged",
      this.handleEvent
    );

    if (this.gBrowser?.tabContainer) {
      this.gBrowser.tabContainer.addEventListener("TabSelect", this);
    }

    this.#prefObserver = { observe: () => this.#updateBadge() };
    Services.prefs.addObserver(OPENED_WITH_LOCATION_PREF, this.#prefObserver);

    if (toolbaritem) {
      toolbaritem.classList.add("subviewbutton-nav"); 
      this.updateState(toolbaritem);
    }
  }

  #addProgressListener() {
    if (!this.gBrowser) {
      return;
    }

    this.#progressListener = {
      onLocationChange: (
        aBrowser,
        aWebProgress,
        _aRequest,
        aLocationURI,
        aFlags
      ) => {
        if (!aWebProgress.isTopLevel) {
          return;
        }

        if (aBrowser !== this.gBrowser?.selectedBrowser) {
          return;
        }

        if (!aLocationURI) {
          return;
        }

        const isReload =
          aFlags & Ci.nsIWebProgressListener.LOCATION_CHANGE_RELOAD;

        this.updateState(null, { showConfirmationHint: !isReload });
      },
    };

    this.gBrowser.addTabsProgressListener(this.#progressListener);
  }

  #handleEvent(event) {
    if (
      event.type !== "IPProtectionService:StateChanged" &&
      event.type !== "IPPProxyManager:StateChanged" &&
      event.type !== "IPPExceptionsManager:ExclusionChanged" &&
      event.type !== "TabSelect"
    ) {
      return;
    }

    let exclusionChanged =
      event.type === "IPPExceptionsManager:ExclusionChanged";

    if (
      event.type === "IPPProxyManager:StateChanged" &&
      lazy.IPPProxyManager.state !== lazy.IPPProxyStates.ACTIVE
    ) {
      this.#visitedExcludedSites.clear();
    }

    this.updateState(null, { showConfirmationHint: !exclusionChanged });
  }

  updateState(
    toolbaritem = null,
    options = { showConfirmationHint: true, error: undefined }
  ) {
    const win = this.#window.get();
    if (!win) {
      return;
    }

    toolbaritem ??= this.toolbaritem;

    if (!toolbaritem) {
      return;
    }

    let principal = this.gBrowser?.contentPrincipal;
    let isExcluded =
      !!principal &&
      lazy.IPPExceptionsManager.canManage(principal) &&
      lazy.IPPExceptionsManager.getPrincipalRule(principal) ===
        lazy.IPPPrincipalRules.EXCLUDED;

    let isActive = lazy.IPPProxyManager.state === lazy.IPPProxyStates.ACTIVE;
    let isPaused = lazy.IPPProxyManager.state === lazy.IPPProxyStates.PAUSED;

    let hasProxyError =
      lazy.IPPProxyManager.state === lazy.IPPProxyStates.ERROR;

    let isNetworkError =
      options?.error === lazy.ERRORS.NETWORK ||
      (hasProxyError && lazy.IPPProxyManager.errorType === lazy.ERRORS.NETWORK);

    let isError = hasProxyError || !!options.error;

    const showConfirmationHint = options.showConfirmationHint ?? true;
    if (showConfirmationHint) {
      this.updateConfirmationHint(win.ConfirmationHint, toolbaritem, {
        isActive,
        isError,
        isExcluded,
      });
    }

    if (principal && !principal.isNullPrincipal) {
      this.#previousIsExcluded = isExcluded;
    }

    this.updateIconStatus(toolbaritem, {
      isActive,
      isError,
      isNetworkError,
      isExcluded,
      isPaused,
    });

    this.#updateBadge(toolbaritem);
  }

  #updateBadge(toolbaritem = null) {
    toolbaritem ??= this.toolbaritem;

    if (!toolbaritem) {
      return;
    }

    let everOpenedPanel = Services.prefs.getBoolPref(
      OPENED_WITH_LOCATION_PREF,
      false
    );

    let inPalette = !lazy.CustomizableUI.getPlacementOfWidget(this.#widgetId);

    let badge = toolbaritem.querySelector(".toolbarbutton-badge");

    if (everOpenedPanel || inPalette) {
      toolbaritem.removeAttribute("badged");
      badge?.classList.remove("feature-callout");
    } else {
      toolbaritem.setAttribute("badged", "true");
      badge?.classList.add("feature-callout");
    }
  }

  updateConfirmationHint(
    confirmationHint,
    toolbaritem,
    status = { isActive: false, isError: false, isExcluded: false }
  ) {
    if (!confirmationHint) {
      return;
    }

    let exceptionsPrefsEnabled =
      this.isExceptionsFeatureEnabled && this.isExceptionsHintsEnabled;

    const canShowConfirmationHint =
      exceptionsPrefsEnabled &&
      !status.isError &&
      status.isActive &&
      status.isExcluded &&
      !this.#previousIsExcluded;

    if (!canShowConfirmationHint) {
      return;
    }

    let siteOrigin = this.gBrowser?.contentPrincipal?.origin;
    if (!siteOrigin || this.#visitedExcludedSites.has(siteOrigin)) {
      return;
    }

    this.#visitedExcludedSites.add(siteOrigin);
    confirmationHint.show(
      toolbaritem,
      IPProtectionToolbarButton.CONFIRMATION_HINT_MESSAGE_ID,
      {
        position: "bottomright topright", 
        hideCheckmark: true,
      }
    );
  }

  updateIconStatus(
    toolbaritem,
    status = {
      isActive: false,
      isError: false,
      isExcluded: false,
      isPaused: false,
      isNetworkError: false,
    }
  ) {
    if (!toolbaritem) {
      return;
    }

    let isActive = status.isActive;
    let isNetworkError = status.isNetworkError;
    let isError = status.isError && !isNetworkError;
    let isExcluded = status.isExcluded && this.isExceptionsFeatureEnabled;
    let isPaused = status.isPaused;
    let l10nId =
      isError || isNetworkError
        ? "ipprotection-button-error"
        : "ipprotection-button";

    toolbaritem.classList.remove(
      "ipprotection-on",
      "ipprotection-network-error",
      "ipprotection-error",
      "ipprotection-excluded",
      "ipprotection-paused"
    );

    if (isNetworkError) {
      toolbaritem.classList.add("ipprotection-network-error");
    } else if (isError) {
      toolbaritem.classList.add("ipprotection-error");
    } else if (isPaused) {
      toolbaritem.classList.add("ipprotection-paused");
    } else if (isExcluded && isActive) {
      toolbaritem.classList.add("ipprotection-excluded");
    } else if (isActive) {
      toolbaritem.classList.add("ipprotection-on");
    }

    toolbaritem.setAttribute("data-l10n-id", l10nId);
  }

  uninit() {
    if (this.gBrowser && this.#progressListener) {
      this.gBrowser.removeTabsProgressListener(this.#progressListener);
    }
    this.#progressListener = null;

    Services.prefs.removeObserver(
      OPENED_WITH_LOCATION_PREF,
      this.#prefObserver
    );
    this.#prefObserver = null;

    if (this.gBrowser?.tabContainer) {
      this.gBrowser.tabContainer.removeEventListener("TabSelect", this);
    }

    lazy.IPProtectionService.removeEventListener(
      "IPProtectionService:StateChanged",
      this.handleEvent
    );
    lazy.IPPProxyManager.removeEventListener(
      "IPPProxyManager:StateChanged",
      this.handleEvent
    );
    lazy.IPPExceptionsManager.removeEventListener(
      "IPPExceptionsManager:ExclusionChanged",
      this.handleEvent
    );
  }
}
