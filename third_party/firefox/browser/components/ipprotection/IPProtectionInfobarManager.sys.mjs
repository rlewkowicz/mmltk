/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { formatRemainingBandwidth } from "chrome://browser/content/ipprotection/ipprotection-utils.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const BANDWIDTH_WARNING_DISMISSED_PREF =
  "browser.ipProtection.bandwidthWarningDismissedThreshold";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  IPProtection:
    "moz-src:///browser/components/ipprotection/IPProtection.sys.mjs",
  IPPProxyManager:
    "moz-src:///toolkit/components/ipprotection/IPPProxyManager.sys.mjs",
  IPProtectionService:
    "moz-src:///toolkit/components/ipprotection/IPProtectionService.sys.mjs",
  IPProtectionStates:
    "moz-src:///toolkit/components/ipprotection/IPProtectionService.sys.mjs",
  IPPUsageHelper:
    "moz-src:///browser/components/ipprotection/IPPUsageHelper.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "BANDWIDTH_USAGE_ENABLED",
  "browser.ipProtection.bandwidth.enabled",
  true,
  (_pref, _prev, value) => {
    if (value) {
      IPProtectionInfobarManager.init();
    } else {
      IPProtectionInfobarManager.uninit();
    }
  }
);

class IPProtectionInfobarManagerClass {
  #initialized = false;
  #lastThreshold = null;
  #lastUsage = null;
  #windowListener = null;
  #prefObserver = null;

  get initialized() {
    return this.#initialized;
  }

  init() {
    if (this.#initialized || !lazy.BANDWIDTH_USAGE_ENABLED) {
      return;
    }

    lazy.IPPProxyManager.addEventListener("IPPProxyManager:UsageChanged", this);
    lazy.IPProtectionService.addEventListener(
      "IPProtectionService:StateChanged",
      this
    );

    this.#windowListener = {
      onOpenWindow: xulWindow => {
        const win = xulWindow.docShell.domWindow;
        win.addEventListener(
          "load",
          () => {
            if (
              win.document.documentElement.getAttribute("windowtype") !==
              "navigator:browser"
            ) {
              return;
            }
            if (this.#lastThreshold && this.#lastUsage) {
              this.#showInfobar(this.#lastThreshold, this.#lastUsage, win);
            }
          },
          { once: true }
        );
      },
    };
    Services.wm.addListener(this.#windowListener);

    this.#prefObserver = this.#handlePrefChange.bind(this);
    Services.prefs.addObserver(
      BANDWIDTH_WARNING_DISMISSED_PREF,
      this.#prefObserver
    );

    this.#initialized = true;
  }

  uninit() {
    if (!this.#initialized) {
      return;
    }

    this.hideInfobars();

    lazy.IPPProxyManager.removeEventListener(
      "IPPProxyManager:UsageChanged",
      this
    );
    lazy.IPProtectionService.removeEventListener(
      "IPProtectionService:StateChanged",
      this
    );

    Services.wm.removeListener(this.#windowListener);
    this.#windowListener = null;
    this.#lastThreshold = null;
    this.#lastUsage = null;

    Services.prefs.removeObserver(
      BANDWIDTH_WARNING_DISMISSED_PREF,
      this.#prefObserver
    );
    this.#prefObserver = null;

    this.#initialized = false;
  }

  #handlePrefChange(_subject, _topic, data) {
    if (data !== BANDWIDTH_WARNING_DISMISSED_PREF) {
      return;
    }
    const { infobar } = lazy.IPPUsageHelper.getDismissedThresholds();
    if (infobar >= 75) {
      this.#hideInfobar(75);
    }
    if (infobar >= 90) {
      this.#hideInfobar(90);
    }
  }

  handleEvent(event) {
    if (
      event.type === "IPProtectionService:StateChanged" &&
      lazy.IPProtectionService.state !== lazy.IPProtectionStates.READY
    ) {
      this.#lastThreshold = null;
      this.#lastUsage = null;
      this.#hideInfobar(75);
      this.#hideInfobar(90);
      return;
    }

    if (event.type === "IPPProxyManager:UsageChanged") {
      const usage = event.detail.usage;
      if (
        !usage ||
        usage.max == null ||
        usage.remaining == null ||
        !usage.reset
      ) {
        return;
      }

      const remainingPercent = Number(usage.remaining) / Number(usage.max);

      if (remainingPercent === 0 || remainingPercent > 0.25) {
        lazy.IPPUsageHelper.setDismissedThresholds({ infobar: 0, panel: 0 });
        this.#lastThreshold = null;
        this.#lastUsage = null;
        this.#hideInfobar(75);
        this.#hideInfobar(90);
        return;
      }

      if (remainingPercent <= 0.1) {
        this.#lastThreshold = 90;
        this.#lastUsage = usage;
        this.#showInfobar(90, usage);
      } else if (remainingPercent > 0.1 && remainingPercent <= 0.25) {
        this.#lastThreshold = 75;
        this.#lastUsage = usage;
        this.#showInfobar(75, usage);
      }
    }
  }

  hideInfobars({ triggeredByPanel = false } = {}) {
    if (triggeredByPanel && this.#lastThreshold) {
      const current = lazy.IPPUsageHelper.getDismissedThresholds();
      if (this.#lastThreshold > current.infobar) {
        lazy.IPPUsageHelper.setDismissedThresholds({
          ...current,
          infobar: this.#lastThreshold,
        });
      }
      this.#lastThreshold = null;
      this.#lastUsage = null;
    }
    this.#hideInfobar(75);
    this.#hideInfobar(90);
  }

  #hideInfobar(threshold) {
    const notificationId = `ip-protection-bandwidth-warning-${threshold}`;
    for (const win of Services.wm.getEnumerator("navigator:browser")) {
      if (win.closed) {
        continue;
      }
      const notification =
        win.gNotificationBox.getNotificationWithValue(notificationId);
      if (notification) {
        win.gNotificationBox.removeNotification(notification);
      }
    }
  }

  #showInfobar(
    threshold,
    usage,
    win = Services.wm.getMostRecentWindow("navigator:browser")
  ) {
    const notificationId = `ip-protection-bandwidth-warning-${threshold}`;

    if (!win || win.closed) {
      return;
    }

    if (lazy.IPPUsageHelper.getDismissedThresholds().infobar >= threshold) {
      return;
    }

    const existing =
      win.gNotificationBox.getNotificationWithValue(notificationId);
    if (existing) {
      return;
    }

    const { value: remainingFormatted, useGB } = formatRemainingBandwidth(
      Number(usage.remaining)
    );

    for (const openWin of Services.wm.getEnumerator("navigator:browser")) {
      if (openWin.closed) {
        continue;
      }
      const panel = lazy.IPProtection.getPanel(openWin);
      if (panel?.active && panel.state.bandwidthWarning) {
        return;
      }
    }

    let usageLeft;
    let l10nId;

    if (!useGB && threshold === 90) {
      usageLeft = String(remainingFormatted);
      l10nId = "ip-protection-bandwidth-warning-infobar-message-90-mb";
    } else {
      usageLeft = String(remainingFormatted);
      l10nId =
        threshold === 90
          ? "ip-protection-bandwidth-warning-infobar-message-90"
          : "ip-protection-bandwidth-warning-infobar-message-75";
    }

    win.gNotificationBox.appendNotification(
      notificationId,
      {
        label: {
          "l10n-id": l10nId,
          "l10n-args": {
            usageLeft,
          },
        },
        priority: win.gNotificationBox.PRIORITY_WARNING_HIGH,
        eventCallback: event => {
          if (event === "dismissed") {
            const current = lazy.IPPUsageHelper.getDismissedThresholds();
            if (threshold > current.infobar) {
              lazy.IPPUsageHelper.setDismissedThresholds({
                ...current,
                infobar: threshold,
              });
            }
          }
        },
      },
      [],
      false,
      true
    );
  }
}

const IPProtectionInfobarManager = new IPProtectionInfobarManagerClass();
export { IPProtectionInfobarManager };
