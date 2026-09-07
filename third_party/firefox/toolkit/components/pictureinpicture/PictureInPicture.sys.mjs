/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

import { ShortcutUtils } from "resource://gre/modules/ShortcutUtils.sys.mjs";

const lazy = {};
XPCOMUtils.defineLazyServiceGetters(lazy, {
  WindowsUIUtils: ["@mozilla.org/windows-ui-utils;1", Ci.nsIWindowsUIUtils],
});

ChromeUtils.defineESModuleGetters(lazy, {
  ASRouter:
    // eslint-disable-next-line mozilla/no-browser-refs-in-toolkit
    "resource:///modules/asrouter/ASRouter.sys.mjs",
  PageActions: "resource:///modules/PageActions.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

import { Rect, Point } from "resource://gre/modules/Geometry.sys.mjs";

const PLAYER_URI = "chrome://global/content/pictureinpicture/player.xhtml";
const TITLEBAR = AppConstants.platform == "macosx" ? "yes" : "no";
const PLAYER_FEATURES = `chrome,alwaysontop,lockaspectratio,resizable,dialog,titlebar=${TITLEBAR}`;

const WINDOW_TYPE = "Toolkit:PictureInPicture";
const TOGGLE_ENABLED_PREF =
  "media.videocontrols.picture-in-picture.video-toggle.enabled";
const TOGGLE_FIRST_SEEN_PREF =
  "media.videocontrols.picture-in-picture.video-toggle.first-seen-secs";
const TOGGLE_HAS_USED_PREF =
  "media.videocontrols.picture-in-picture.video-toggle.has-used";
const TOGGLE_POSITION_PREF =
  "media.videocontrols.picture-in-picture.video-toggle.position";
const TOGGLE_POSITION_RIGHT = "right";
const TOGGLE_POSITION_LEFT = "left";
const RESIZE_MARGIN_PX = 16;

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "PIP_ENABLED",
  "media.videocontrols.picture-in-picture.enabled",
  false
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "PIP_URLBAR_BUTTON",
  "media.videocontrols.picture-in-picture.urlbar-button.enabled",
  false
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "RESPECT_PIP_DISABLED",
  "media.videocontrols.picture-in-picture.respect-disablePictureInPicture",
  true
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "PIP_WHEN_SWITCHING_TABS",
  "media.videocontrols.picture-in-picture.enable-when-switching-tabs.enabled",
  true
);

let gNextWindowID = 0;

export class PictureInPictureLauncherParent extends JSWindowActorParent {
  async receiveMessage(aMessage) {
    switch (aMessage.name) {
      case "PictureInPicture:Request": {
        let videoData = aMessage.data;
        return PictureInPicture.handlePictureInPictureRequest(
          this.manager,
          videoData
        );
      }
    }
    return undefined;
  }
}

export class PictureInPictureToggleParent extends JSWindowActorParent {
  receiveMessage(aMessage) {
    let browsingContext = aMessage.target.browsingContext;
    let browser = browsingContext.top.embedderElement;
    switch (aMessage.name) {
      case "PictureInPicture:OpenToggleContextMenu": {
        let win = browser.documentGlobal;
        PictureInPicture.openToggleContextMenu(win, aMessage.data);
        break;
      }
      case "PictureInPicture:UpdateEligiblePipVideoCount": {
        let { pipCount, pipDisabledCount } = aMessage.data;
        PictureInPicture.updateEligiblePipVideoCount(browsingContext, {
          pipCount,
          pipDisabledCount,
        });
        PictureInPicture.updateUrlbarToggle(browser);
        break;
      }
      case "PictureInPicture:SetFirstSeen": {
        let { dateSeconds } = aMessage.data;
        PictureInPicture.setFirstSeen(dateSeconds);
        break;
      }
      case "PictureInPicture:SetHasUsed": {
        let { hasUsed } = aMessage.data;
        PictureInPicture.setHasUsed(hasUsed);
        break;
      }
      case "PictureInPicture:VideoTabHidden": {
        if (!lazy.PIP_ENABLED || !lazy.PIP_WHEN_SWITCHING_TABS) {
          break;
        }
        if (browser.documentGlobal.gBrowser.selectedBrowser == browser) {
          break;
        }
        let actor = browsingContext.currentWindowGlobal.getActor(
          "PictureInPictureLauncher"
        );
        actor.sendAsyncMessage("PictureInPicture:AutoToggle");
        break;
      }
    }
  }
}

export class PictureInPictureParent extends JSWindowActorParent {
  async receiveMessage(aMessage) {
    switch (aMessage.name) {
      case "PictureInPicture:Resize": {
        let videoData = aMessage.data;
        PictureInPicture.resizePictureInPictureWindow(videoData, this);
        break;
      }
      case "PictureInPicture:Close": {
        let reason = aMessage.data.reason;
        return PictureInPicture.closeSinglePipWindow({
          reason,
          actorRef: this,
        });
      }
      case "PictureInPicture:Playing": {
        let player = PictureInPicture.getWeakPipPlayer(this);
        if (player) {
          player.setIsPlayingState(true);
        }
        break;
      }
      case "PictureInPicture:Paused": {
        let player = PictureInPicture.getWeakPipPlayer(this);
        if (player) {
          player.setIsPlayingState(false);
        }
        break;
      }
      case "PictureInPicture:Muting": {
        let player = PictureInPicture.getWeakPipPlayer(this);
        if (player) {
          player.setIsMutedState(true);
        }
        break;
      }
      case "PictureInPicture:Unmuting": {
        let player = PictureInPicture.getWeakPipPlayer(this);
        if (player) {
          player.setIsMutedState(false);
        }
        break;
      }
      case "PictureInPicture:EnableSubtitlesButton": {
        let player = PictureInPicture.getWeakPipPlayer(this);
        if (player) {
          player.enableSubtitlesButton();
        }
        break;
      }
      case "PictureInPicture:DisableSubtitlesButton": {
        let player = PictureInPicture.getWeakPipPlayer(this);
        if (player) {
          player.disableSubtitlesButton();
        }
        break;
      }
      case "PictureInPicture:SetTimestampAndScrubberPosition": {
        let { timestamp, scrubberPosition } = aMessage.data;
        let player = PictureInPicture.getWeakPipPlayer(this);
        if (player) {
          player.setTimestamp(timestamp);
          player.setScrubberPosition(scrubberPosition);
        }
        break;
      }
      case "PictureInPicture:VolumeChange": {
        let { volume } = aMessage.data;
        let player = PictureInPicture.getWeakPipPlayer(this);
        if (player) {
          player.setVolume(volume);
        }
        break;
      }
    }
    return undefined;
  }
}

export var PictureInPicture = {
  weakPipToWin: new WeakMap(),

  weakWinToBrowser: new WeakMap(),

  browserWeakMap: new WeakMap(),

  originatingWinWeakMap: new WeakMap(),

  weakGlobalToEligiblePipCount: new WeakMap(),

  apiPipWindow: null,

  currentPlayerCount: 0,
  maxConcurrentPlayerCount: 0,

  weakAutoPipBrowserToParent: new WeakMap(),

  getWeakPipPlayer(pipActorRef) {
    let playerWin = this.weakPipToWin.get(pipActorRef);
    if (!playerWin || playerWin.closed) {
      return null;
    }
    return playerWin;
  },

  getPanelForBrowser(browser) {
    let panel = browser.ownerDocument.querySelector("#PictureInPicturePanel");

    if (!panel) {
      let template = browser.ownerDocument.querySelector(
        "#PictureInPicturePanelTemplate"
      );
      let clone = template.content.cloneNode(true);
      template.replaceWith(clone);

      panel = this.getPanelForBrowser(browser);
      this._attachEventListeners(panel);
    }
    return panel;
  },

  _attachEventListeners(panel) {
    panel.addEventListener("popupshown", this);
    panel.addEventListener("popuphidden", this);
    panel
      .querySelector("#respect-pipDisabled-switch")
      .addEventListener("click", this);
  },

  handleEvent(event) {
    switch (event.type) {
      case "TabSwapPictureInPicture": {
        this.onPipSwappedBrowsers(event);
        break;
      }
      case "TabSelect": {
        this.unpipAutoPipBrowser(event);
        break;
      }
      case "popupshown":
        this.onPipPanelShown(event);
        break;
      case "popuphidden":
        this.onPipPanelHidden(event);
        break;
      case "click":
        this.toggleRespectDisablePip(event);
        break;
    }
  },

  addPiPBrowserToWeakMap(browser) {
    let count = this.browserWeakMap.has(browser)
      ? this.browserWeakMap.get(browser)
      : 0;
    this.browserWeakMap.set(browser, count + 1);

  },

  addOriginatingWinToWeakMap(browser) {
    let parentWin = browser.documentGlobal;
    let count = this.originatingWinWeakMap.get(parentWin);
    if (!count || count == 0) {
      this.setOriginatingWindowActive(parentWin.browsingContext, true);
      this.originatingWinWeakMap.set(parentWin, 1);

      let gBrowser = browser.getTabBrowser();
      if (gBrowser) {
        gBrowser.tabContainer.addEventListener("TabSelect", this);
      }
    } else {
      this.originatingWinWeakMap.set(parentWin, count + 1);
    }
  },

  removePiPBrowserFromWeakMap(browser) {
    let count = this.browserWeakMap.get(browser);
    if (count <= 1) {
      this.browserWeakMap.delete(browser);
      let tabbrowser = browser.getTabBrowser();
      if (tabbrowser && !tabbrowser.shouldActivateDocShell(browser)) {
        browser.docShellIsActive = false;
      }
    } else {
      this.browserWeakMap.set(browser, count - 1);
    }
  },

  removeOriginatingWinFromWeakMap(browser) {
    let parentWin = browser?.documentGlobal;

    if (!parentWin) {
      return;
    }

    let count = this.originatingWinWeakMap.get(parentWin);
    if (!count || count <= 1) {
      this.originatingWinWeakMap.delete(parentWin, 0);
      this.setOriginatingWindowActive(parentWin.browsingContext, false);

      let gBrowser = browser.getTabBrowser();
      if (gBrowser) {
        gBrowser.tabContainer.removeEventListener("TabSelect", this);
      }
    } else {
      this.originatingWinWeakMap.set(parentWin, count - 1);
    }
  },

  unpipAutoPipBrowser(event) {
    let browser = event.target.linkedBrowser;
    if (this.weakAutoPipBrowserToParent.has(browser)) {
      this.closeSinglePipWindow({
        reason: "Foregrounded",
        actorRef: this.weakAutoPipBrowserToParent.get(browser),
      });
    }
  },

  onPipSwappedBrowsers(event) {
    let otherTab = event.detail;
    if (otherTab) {
      for (let win of Services.wm.getEnumerator(WINDOW_TYPE)) {
        if (this.weakWinToBrowser.get(win) === event.target.linkedBrowser) {
          this.weakWinToBrowser.set(win, otherTab.linkedBrowser);
          this.removePiPBrowserFromWeakMap(event.target.linkedBrowser);
          this.removeOriginatingWinFromWeakMap(event.target.linkedBrowser);
          this.addPiPBrowserToWeakMap(otherTab.linkedBrowser);
          this.addOriginatingWinToWeakMap(otherTab.linkedBrowser);
        }
        if (this.weakAutoPipBrowserToParent.has(event.target.linkedBrowser)) {
          this.weakAutoPipBrowserToParent.set(
            otherTab.linkedBrowser,
            this.weakAutoPipBrowserToParent.get(event.target.linkedBrowser)
          );
          this.weakAutoPipBrowserToParent.delete(event.target.linkedBrowser);
        }
      }
      otherTab.addEventListener("TabSwapPictureInPicture", this);
    }
  },

  onCommand(event) {
    if (!lazy.PIP_ENABLED) {
      return;
    }

    let win = event.target.documentGlobal;
    let bc = Services.focus.focusedContentBrowsingContext;
    if (bc.top == win.gBrowser.selectedBrowser.browsingContext) {
      let actor = bc.currentWindowGlobal.getActor("PictureInPictureLauncher");
      actor.sendAsyncMessage("PictureInPicture:KeyToggle");
    }
  },

  async focusTabAndClosePip(window, pipActor) {
    let browser = this.weakWinToBrowser.get(window);
    if (!browser) {
      return;
    }

    let gBrowser = browser.getTabBrowser();
    let tab = gBrowser.getTabForBrowser(browser);

    tab.documentGlobal.focus();

    gBrowser.selectedTab = tab;
    await this.closeSinglePipWindow({ reason: "Unpip", actorRef: pipActor });
  },

  toggleRespectDisablePip(event) {
    let toggle = event.target;
    let respectPipDisabled = !toggle.pressed;

    Services.prefs.setBoolPref(
      "media.videocontrols.picture-in-picture.respect-disablePictureInPicture",
      respectPipDisabled
    );

  },

  updateEligiblePipVideoCount(browsingContext, object) {
    let windowGlobal = browsingContext.currentWindowGlobal;

    if (windowGlobal) {
      this.weakGlobalToEligiblePipCount.set(windowGlobal, object);
    }
  },

  *windowGlobalPipCountGenerator(browser) {
    let contextsToVisit = [browser.browsingContext];
    while (contextsToVisit.length) {
      let currentBC = contextsToVisit.pop();
      let windowGlobal = currentBC.currentWindowGlobal;

      if (!windowGlobal) {
        continue;
      }

      let { pipCount, pipDisabledCount } =
        this.weakGlobalToEligiblePipCount.get(windowGlobal) || {
          pipCount: 0,
          pipDisabledCount: 0,
        };

      contextsToVisit.push(...currentBC.children);

      yield { windowGlobal, pipCount, pipDisabledCount };
    }
  },

  getEligiblePipVideoCount(browser) {
    let totalPipCount = 0;
    let totalPipDisabled = 0;

    for (let {
      pipCount,
      pipDisabledCount,
    } of this.windowGlobalPipCountGenerator(browser)) {
      totalPipCount += pipCount;
      totalPipDisabled += pipDisabledCount;
    }

    return { totalPipCount, totalPipDisabled };
  },

  updateUrlbarHoverText(document, pipToggle, dataL10nId) {
    let shortcut = document.getElementById("key_togglePictureInPicture");

    document.l10n.setAttributes(pipToggle, dataL10nId, {
      shortcut: ShortcutUtils.prettifyShortcut(shortcut),
    });
  },

  onLocationChange(_window, _locationURI, webProgress, _flags) {
    const browser = webProgress.browsingContext.embedderElement;
    if (!browser) {
      return;
    }
    this.updateUrlbarToggle(browser);
  },

  updateUrlbarToggle(browser) {
    if (!lazy.PIP_ENABLED || !lazy.PIP_URLBAR_BUTTON) {
      return;
    }

    if (!browser) {
      return;
    }

    let win = browser.documentGlobal;
    if (win.closed || win.gBrowser?.selectedBrowser !== browser) {
      return;
    }

    let { totalPipCount, totalPipDisabled } =
      this.getEligiblePipVideoCount(browser);

    let pipToggle = win.document.getElementById("picture-in-picture-button");
    if (
      totalPipCount === 1 ||
      (totalPipDisabled > 0 && lazy.RESPECT_PIP_DISABLED)
    ) {
      pipToggle.hidden = false;
      lazy.PageActions.sendPlacedInUrlbarTrigger(pipToggle);
    } else {
      pipToggle.hidden = true;
    }

    let browserHasPip = !!this.browserWeakMap.get(browser);
    if (browserHasPip) {
      this.setUrlbarPipIconActive(browser.documentGlobal);
    } else {
      this.setUrlbarPipIconInactive(browser.documentGlobal);
    }
  },

  toggleUrlbar(event) {
    let win = event.target.documentGlobal;
    let browser = win.gBrowser.selectedBrowser;

    let pipPanel = this.getPanelForBrowser(browser);

    for (let {
      windowGlobal,
      pipCount,
      pipDisabledCount,
    } of this.windowGlobalPipCountGenerator(browser)) {
      if (
        (pipDisabledCount > 0 && lazy.RESPECT_PIP_DISABLED) ||
        (pipPanel && pipPanel.state !== "closed")
      ) {
        this.togglePipPanel(browser);
        return;
      } else if (pipCount === 1) {
        let eventExtraKeys = {};
        if (
          !Services.prefs.getBoolPref(TOGGLE_HAS_USED_PREF) &&
          lazy.ASRouter.initialized
        ) {
          let { messages, messageImpressions } = lazy.ASRouter.state;
          let pipCallouts = messages.filter(
            message =>
              message.template === "feature_callout" &&
              message.content.screens.some(screen =>
                screen.anchors.some(anchor =>
                  anchor.selector.includes("picture-in-picture-button")
                )
              )
          );
          if (pipCallouts.length) {
            let now = Date.now();
            let callout = pipCallouts.some(message =>
              messageImpressions[message.id]?.some(
                impression => now - impression < 48 * 60 * 60 * 1000
              )
            );
            if (callout) {
              eventExtraKeys.callout = true;
            }
          }
        }
        let actor = windowGlobal.getActor("PictureInPictureToggle");
        actor.sendAsyncMessage("PictureInPicture:UrlbarToggle", eventExtraKeys);
        return;
      }
    }
  },

  onPipPanelShown(event) {
    let toggle = event.target.querySelector("#respect-pipDisabled-switch");
    toggle.pressed = !lazy.RESPECT_PIP_DISABLED;
  },

  onPipPanelHidden(event) {
    this.updateUrlbarToggle(event.view.gBrowser.selectedBrowser);
  },

  togglePipPanel(browser) {
    let pipPanel = this.getPanelForBrowser(browser);

    if (pipPanel.state === "closed") {
      let anchor = browser.ownerDocument.querySelector(
        "#picture-in-picture-button"
      );

      pipPanel.openPopup(anchor, "bottomright topright");
    } else {
      pipPanel.hidePopup();
    }
  },

  setUrlbarPipIconActive(win) {
    let pipToggle = win.document.getElementById("picture-in-picture-button");
    pipToggle.toggleAttribute("pipactive", true);

    this.updateUrlbarHoverText(
      win.document,
      pipToggle,
      "picture-in-picture-urlbar-button-close"
    );
  },

  setUrlbarPipIconInactive(win) {
    if (!win) {
      return;
    }
    let pipToggle = win.document.getElementById("picture-in-picture-button");
    pipToggle.toggleAttribute("pipactive", false);

    this.updateUrlbarHoverText(
      win.document,
      pipToggle,
      "picture-in-picture-urlbar-button-open"
    );
  },

  clearPipTabIcon(window) {
    const browser = this.weakWinToBrowser.get(window);
    if (!browser) {
      return;
    }

    for (let win of Services.wm.getEnumerator(WINDOW_TYPE)) {
      if (
        win !== window &&
        this.weakWinToBrowser.has(win) &&
        this.weakWinToBrowser.get(win) === browser
      ) {
        return;
      }
    }

    let gBrowser = browser.getTabBrowser();
    let tab = gBrowser?.getTabForBrowser(browser);
    if (tab) {
      tab.removeAttribute("pictureinpicture");
    }
  },

  async closePipWindow(pipWin) {
    if (pipWin.closed) {
      return;
    }
    let closedPromise = new Promise(resolve => {
      pipWin.addEventListener("unload", resolve, { once: true });
    });
    pipWin.close();
    await closedPromise;
  },

  async closeSinglePipWindow(closeData) {
    const { reason, actorRef } = closeData;
    const win = this.getWeakPipPlayer(actorRef);
    if (!win) {
      return;
    }
    this.removePiPBrowserFromWeakMap(this.weakWinToBrowser.get(win));
    this.weakAutoPipBrowserToParent.delete(this.weakWinToBrowser.get(win));

    await this.closePipWindow(win);
  },

  setApiWindow(window) {
    if (this.apiPipWindow != null) {
      console.error(`PIP API Window reference not properly cleared.`);
    }
    this.apiPipWindow = Cu.getWeakReference(window);
    window.addEventListener("unload", () => {
      this.clearApiWindow(window);
    });
  },

  clearApiWindow(window) {
    let currentWindow = this.apiPipWindow?.get();
    if (currentWindow == window) {
      this.apiPipWindow = null;
    } else {
      console.error(`PIP API Window state not properly cleared.`);
    }
  },

  async closeApiPipWindowIfOpen(reason = "Api") {
    const pipApiWindow = this.apiPipWindow?.get();
    if (pipApiWindow) {
      await this.closePipWindow(pipApiWindow);
      this.apiPipWindow = null;
    }
  },

  async handlePictureInPictureRequest(wgp, videoData) {
    const isApiRequest = !!videoData.isPipApiRequest;

    if (isApiRequest) {
      await this.closeApiPipWindowIfOpen();
    }

    this.currentPlayerCount += 1;
    this.maxConcurrentPlayerCount = Math.max(
      this.maxConcurrentPlayerCount,
      this.currentPlayerCount
    );

    let browser = wgp.browsingContext.top.embedderElement;
    let parentWin = browser.documentGlobal;

    let win = await this.openPipWindow(parentWin, videoData);
    win.setIsPlayingState(videoData.playing);
    win.setIsMutedState(videoData.isMuted);

    let tab = parentWin.gBrowser.getTabForBrowser(browser);
    tab.setAttribute("pictureinpicture", true);

    this.setUrlbarPipIconActive(parentWin);

    tab.addEventListener("TabSwapPictureInPicture", this);

    let pipId = gNextWindowID.toString();
    const { actor: actorRef, setupPromise } = win.setupPlayer(
      pipId,
      wgp,
      videoData.videoRef,
      isApiRequest,
      videoData.autoFocus
    );
    gNextWindowID++;

    this.weakWinToBrowser.set(win, browser);
    this.addPiPBrowserToWeakMap(browser);
    this.addOriginatingWinToWeakMap(browser);
    if (lazy.PIP_WHEN_SWITCHING_TABS && !browser.docShellIsActive) {
      browser.docShellIsActive = true;
      this.weakAutoPipBrowserToParent.set(browser, actorRef);
    }

    if (isApiRequest) {
      this.setApiWindow(win);
    }

    win.setScrubberPosition(videoData.scrubberPosition);
    win.setTimestamp(videoData.timestamp);
    win.setVolume(videoData.volume);

    Services.prefs.setBoolPref(TOGGLE_HAS_USED_PREF, true);

    await setupPromise;
  },

  setOriginatingWindowActive(browsingContext, isActive) {
    browsingContext.forceAppWindowActive = isActive;
  },

  unload(window) {

    let browser = this.weakWinToBrowser.get(window);
    this.removeOriginatingWinFromWeakMap(browser);

    this.currentPlayerCount -= 1;
    this.savePosition(window);
    this.clearPipTabIcon(window);
    this.setUrlbarPipIconInactive(browser?.documentGlobal);
  },

  async openPipWindow(parentWin, videoData) {
    let { top, left, width, height } = this.fitToScreen(parentWin, videoData);

    let { left: resolvedLeft, top: resolvedTop } = this.resolveOverlapConflicts(
      left,
      top,
      width,
      height
    );

    top = Math.round(resolvedTop);
    left = Math.round(resolvedLeft);
    width = Math.round(width);
    height = Math.round(height);

    let features =
      `${PLAYER_FEATURES},top=${top},left=${left},outerWidth=${width},` +
      `outerHeight=${height}`;
    let isPrivate = lazy.PrivateBrowsingUtils.isWindowPrivate(parentWin);

    if (isPrivate) {
      features += ",private";
    }

    let pipWindow = Services.ww.openWindow(
      parentWin,
      PLAYER_URI,
      null,
      features,
      null
    );

    pipWindow.windowUtils.setResizeMargin(RESIZE_MARGIN_PX);

    if (Services.appinfo.OS == "WINNT" && !isPrivate) {
      lazy.WindowsUIUtils.setWindowIconNoData(pipWindow);
    }

    return new Promise(resolve => {
      pipWindow.addEventListener(
        "load",
        () => {
          resolve(pipWindow);
        },
        { once: true }
      );
    });
  },

  fitToScreen(requestingWin, videoData) {
    let { videoHeight, videoWidth } = videoData;

    const isPlayer = requestingWin.document.location.href == PLAYER_URI;

    let requestingCssToDesktopScale =
      requestingWin.devicePixelRatio / requestingWin.desktopToDeviceScale;

    let top, left, width, height;
    if (!isPlayer) {
      ({ top, left, width, height } = this.loadPosition());
    } else if (requestingWin.windowState === requestingWin.STATE_FULLSCREEN) {
      ({ top, left, width, height } = requestingWin.getDeferredResize());
      left *= requestingCssToDesktopScale;
      top *= requestingCssToDesktopScale;
    } else {
      left = requestingWin.screenX * requestingCssToDesktopScale;
      top = requestingWin.screenY * requestingCssToDesktopScale;
      width = requestingWin.outerWidth;
      height = requestingWin.outerHeight;
    }

    if (!isNaN(top) && !isNaN(left) && !isNaN(width) && !isNaN(height)) {
      let PiPScreen = this.getWorkingScreen(left, top);

      let PipScreenCssToDesktopScale =
        PiPScreen.defaultCSSScaleFactor / PiPScreen.contentsScaleFactor;
      let centerX = left + (width * PipScreenCssToDesktopScale) / 2;
      let centerY = top + (height * PipScreenCssToDesktopScale) / 2;

      let [PiPScreenLeft, PiPScreenTop, PiPScreenWidth, PiPScreenHeight] =
        this.getAvailScreenSize(PiPScreen);

      if (
        PiPScreenLeft <= centerX &&
        centerX <= PiPScreenLeft + PiPScreenWidth &&
        PiPScreenTop <= centerY &&
        centerY <= PiPScreenTop + PiPScreenHeight
      ) {
        let oldWidthDesktopPix = width * PipScreenCssToDesktopScale;

        width = Math.round((height * videoWidth) / videoHeight);

        if (AppConstants.platform == "win") {
          width = 136 > width ? 136 : width;
        }

        let widthDesktopPix = width * PipScreenCssToDesktopScale;
        let heightDesktopPix = height * PipScreenCssToDesktopScale;

        const WIGGLE_ROOM = 5;
        let rightScreen = PiPScreenLeft + PiPScreenWidth;
        let distFromRight = rightScreen - (left + widthDesktopPix);
        if (
          0 < distFromRight &&
          distFromRight <= WIGGLE_ROOM + (oldWidthDesktopPix - widthDesktopPix)
        ) {
          left += distFromRight;
        }

        if (left < PiPScreenLeft) {
          left = PiPScreenLeft;
        }
        if (top < PiPScreenTop) {
          top = PiPScreenTop;
        }
        if (left + widthDesktopPix > PiPScreenLeft + PiPScreenWidth) {
          left = PiPScreenLeft + PiPScreenWidth - widthDesktopPix;
        }
        if (top + heightDesktopPix > PiPScreenTop + PiPScreenHeight) {
          top = PiPScreenTop + PiPScreenHeight - heightDesktopPix;
        }
        top /= requestingCssToDesktopScale;
        left /= requestingCssToDesktopScale;
        return { top, left, width, height };
      }
    }

    let screen = this.getWorkingScreen(
      requestingWin.screenX * requestingCssToDesktopScale,
      requestingWin.screenY * requestingCssToDesktopScale,
      requestingWin.outerWidth * requestingCssToDesktopScale,
      requestingWin.outerHeight * requestingCssToDesktopScale
    );
    let [screenLeft, screenTop, screenWidth, screenHeight] =
      this.getAvailScreenSize(screen);

    let screenCssToDesktopScale =
      screen.defaultCSSScaleFactor / screen.contentsScaleFactor;

    const MAX_HEIGHT = screenHeight / 4;
    const MAX_WIDTH = screenWidth / 3;

    width = videoWidth * screenCssToDesktopScale;
    height = videoHeight * screenCssToDesktopScale;
    let aspectRatio = videoWidth / videoHeight;

    if (videoHeight > MAX_HEIGHT || videoWidth > MAX_WIDTH) {
      if (videoWidth >= videoHeight) {
        width = MAX_WIDTH;
        height = Math.round(MAX_WIDTH / aspectRatio);
      } else {
        height = MAX_HEIGHT;
        width = Math.round(MAX_HEIGHT * aspectRatio);
      }
    }

    let isRTL = Services.locale.isAppLocaleRTL;
    left = isRTL ? screenLeft : screenLeft + screenWidth - width;
    top = screenTop + screenHeight - height;

    top /= requestingCssToDesktopScale;
    left /= requestingCssToDesktopScale;
    width /= screenCssToDesktopScale;
    height /= screenCssToDesktopScale;

    return { top, left, width, height };
  },

  resolveOverlapConflicts(left, top, width, height) {
    let playerRects = [];

    for (let playerWin of Services.wm.getEnumerator(WINDOW_TYPE)) {
      playerRects.push(
        new Rect(
          playerWin.screenX,
          playerWin.screenY,
          playerWin.outerWidth,
          playerWin.outerHeight
        )
      );
    }

    const newPlayerRect = new Rect(left, top, width, height);
    let conflictingPipRect = playerRects.find(rect =>
      rect.intersects(newPlayerRect)
    );

    if (!conflictingPipRect) {
      return { left, top };
    }

    const conflictLoc = conflictingPipRect.center();

    const conflictScreen = this.getWorkingScreen(conflictLoc.x, conflictLoc.y);

    const [screenTop, screenLeft, screenWidth, screenHeight] =
      this.getAvailScreenSize(conflictScreen);

    const screenRect = new Rect(
      screenTop,
      screenLeft,
      screenWidth,
      screenHeight
    );

    const getEdgeCandidates = rect => {
      return [
        new Point(rect.left - newPlayerRect.width, rect.top),
        new Point(rect.left, rect.top - newPlayerRect.height),
        new Point(rect.right + newPlayerRect.width, rect.top),
        new Point(rect.left, rect.bottom),
      ];
    };

    let candidateLocations = [];
    for (const playerRect of playerRects) {
      for (let candidateLoc of getEdgeCandidates(playerRect)) {
        const candidateRect = new Rect(
          candidateLoc.x,
          candidateLoc.y,
          width,
          height
        );

        if (!screenRect.contains(candidateRect)) {
          continue;
        }

        if (playerRects.some(rect => rect.intersects(candidateRect))) {
          continue;
        }

        const candidateCenter = candidateRect.center();
        const candidateDistanceToConflict =
          Math.abs(conflictLoc.x - candidateCenter.x) +
          Math.abs(conflictLoc.y - candidateCenter.y);

        candidateLocations.push({
          distanceToConflict: candidateDistanceToConflict,
          location: candidateLoc,
        });
      }
    }

    if (!candidateLocations.length) {
      return { left, top };
    }

    const closestCandidate = candidateLocations.sort(
      (firstCand, secondCand) =>
        firstCand.distanceToConflict - secondCand.distanceToConflict
    )[0];

    if (!closestCandidate) {
      return { left, top };
    }

    const resolvedX = closestCandidate.location.x;
    const resolvedY = closestCandidate.location.y;

    return { left: resolvedX, top: resolvedY };
  },

  resizePictureInPictureWindow(videoData, actorRef) {
    let win = this.getWeakPipPlayer(actorRef);

    if (!win) {
      return;
    }

    win.resizeToVideo(this.fitToScreen(win, videoData));
  },

  openToggleContextMenu(window, data) {
    let document = window.document;
    let popup = document.getElementById("pictureInPictureToggleContextMenu");
    let contextMoveToggle = document.getElementById(
      "context_MovePictureInPictureToggle"
    );

    let position = Services.prefs.getStringPref(
      TOGGLE_POSITION_PREF,
      TOGGLE_POSITION_RIGHT
    );
    switch (position) {
      case TOGGLE_POSITION_RIGHT:
        document.l10n.setAttributes(
          contextMoveToggle,
          "picture-in-picture-move-toggle-left"
        );
        break;
      case TOGGLE_POSITION_LEFT:
        document.l10n.setAttributes(
          contextMoveToggle,
          "picture-in-picture-move-toggle-right"
        );
        break;
    }

    let newEvent = new PointerEvent("contextmenu", {
      bubbles: true,
      cancelable: true,
      screenX: data.screenXDevPx / window.devicePixelRatio,
      screenY: data.screenYDevPx / window.devicePixelRatio,
      pointerType: (() => {
        switch (data.inputSource) {
          case MouseEvent.MOZ_SOURCE_MOUSE:
            return "mouse";
          case MouseEvent.MOZ_SOURCE_PEN:
            return "pen";
          case MouseEvent.MOZ_SOURCE_ERASER:
            return "eraser";
          case MouseEvent.MOZ_SOURCE_CURSOR:
            return "cursor";
          case MouseEvent.MOZ_SOURCE_TOUCH:
            return "touch";
          case MouseEvent.MOZ_SOURCE_KEYBOARD:
            return "keyboard";
          default:
            return "";
        }
      })(),
    });
    popup.openPopupAtScreen(newEvent.screenX, newEvent.screenY, true, newEvent);
  },

  hideToggle() {
    Services.prefs.setBoolPref(TOGGLE_ENABLED_PREF, false);
  },

  isOriginatingBrowser(browser) {
    return this.browserWeakMap.has(browser);
  },

  moveToggle() {
    let position = Services.prefs.getStringPref(
      TOGGLE_POSITION_PREF,
      TOGGLE_POSITION_RIGHT
    );
    let newPosition = "";
    switch (position) {
      case TOGGLE_POSITION_RIGHT:
        newPosition = TOGGLE_POSITION_LEFT;
        break;
      case TOGGLE_POSITION_LEFT:
        newPosition = TOGGLE_POSITION_RIGHT;
        break;
    }
    if (newPosition) {
      Services.prefs.setStringPref(TOGGLE_POSITION_PREF, newPosition);
    }
  },

  getAvailScreenSize(screen) {
    let screenLeft = {},
      screenTop = {},
      screenWidth = {},
      screenHeight = {};
    screen.GetAvailRectDisplayPix(
      screenLeft,
      screenTop,
      screenWidth,
      screenHeight
    );
    return [
      screenLeft.value,
      screenTop.value,
      screenWidth.value,
      screenHeight.value,
    ];
  },

  getWorkingScreen(left, top, width = 1, height = 1) {
    let screenManager = Cc["@mozilla.org/gfx/screenmanager;1"].getService(
      Ci.nsIScreenManager
    );
    return screenManager.screenForRect(left, top, width, height);
  },

  savePosition(win) {
    let xulStore = Services.xulStore;

    let cssToDesktopScale = win.devicePixelRatio / win.desktopToDeviceScale;

    let left = win.screenX * cssToDesktopScale;
    let top = win.screenY * cssToDesktopScale;
    let width = win.outerWidth;
    let height = win.outerHeight;

    xulStore.setValue(PLAYER_URI, "picture-in-picture", "left", left);
    xulStore.setValue(PLAYER_URI, "picture-in-picture", "top", top);
    xulStore.setValue(PLAYER_URI, "picture-in-picture", "width", width);
    xulStore.setValue(PLAYER_URI, "picture-in-picture", "height", height);
  },

  loadPosition() {
    let xulStore = Services.xulStore;

    let left = parseInt(
      xulStore.getValue(PLAYER_URI, "picture-in-picture", "left")
    );
    let top = parseInt(
      xulStore.getValue(PLAYER_URI, "picture-in-picture", "top")
    );
    let width = parseInt(
      xulStore.getValue(PLAYER_URI, "picture-in-picture", "width")
    );
    let height = parseInt(
      xulStore.getValue(PLAYER_URI, "picture-in-picture", "height")
    );

    return { top, left, width, height };
  },

  setFirstSeen(dateSeconds) {
    if (!dateSeconds) {
      return;
    }

    Services.prefs.setIntPref(TOGGLE_FIRST_SEEN_PREF, dateSeconds);
  },

  setHasUsed(hasUsed) {
    Services.prefs.setBoolPref(TOGGLE_HAS_USED_PREF, !!hasUsed);
  },
};
