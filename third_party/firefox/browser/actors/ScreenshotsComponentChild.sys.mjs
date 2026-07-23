/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  DeferredTask: "resource://gre/modules/DeferredTask.sys.mjs",
  ScreenshotsOverlay:
    "moz-src:///browser/components/screenshots/ScreenshotsOverlayChild.sys.mjs",
});

const SCREENSHOTS_PREVENT_CONTENT_EVENTS_PREF =
  "screenshots.browser.component.preventContentEvents";

export class ScreenshotsComponentChild extends JSWindowActorChild {
  #resizeTask;
  #scrollTask;
  #overlay;
  #preventableEventsAdded = false;

  static OVERLAY_EVENTS = [
    "click",
    "pointerdown",
    "pointermove",
    "pointerup",
    "keyup",
    "keydown",
  ];

  static PREVENTABLE_EVENTS = [
    "mousemove",
    "mousedown",
    "mouseup",
    "mouseenter",
    "mouseover",
    "mouseout",
    "mouseleave",
    "touchstart",
    "touchmove",
    "touchend",
    "dblclick",
    "auxclick",
    "keypress",
    "contextmenu",
    "pointerenter",
    "pointerover",
    "pointerout",
    "pointerleave",
  ];

  get overlay() {
    return this.#overlay;
  }

  receiveMessage(message) {
    switch (message.name) {
      case "Screenshots:ShowOverlay":
        return this.startScreenshotsOverlay();
      case "Screenshots:HideOverlay":
        return this.endScreenshotsOverlay(message.data);
      case "Screenshots:isOverlayShowing":
        return this.overlay?.initialized;
      case "Screenshots:getFullPageBounds":
        return this.getFullPageBounds();
      case "Screenshots:getVisibleBounds":
        return this.getVisibleBounds();
      case "Screenshots:getDocumentTitle":
        return this.getDocumentTitle();
      case "Screenshots:GetMethodsUsed":
        return this.getMethodsUsed();
      case "Screenshots:RemoveEventListeners":
        return this.removeEventListeners();
      case "Screenshots:AddEventListeners":
        return this.addEventListeners();
      case "Screenshots:MoveFocusToContent":
        return this.focusOverlay(message.data);
      case "Screenshots:ClearFocus":
        Services.focus.clearFocus(this.contentWindow);
        return null;
    }
    return null;
  }

  handleEvent(event) {
    if (!event.isTrusted) {
      return;
    }

    if (
      [
        ...ScreenshotsComponentChild.OVERLAY_EVENTS,
        ...ScreenshotsComponentChild.PREVENTABLE_EVENTS,
        "selectionchange",
      ].includes(event.type)
    ) {
      if (!this.overlay?.initialized) {
        return;
      }

      if (!["contextmenu", "pointerdown"].includes(event.type)) {
        event.preventDefault();
      }

      event.stopImmediatePropagation();
      this.overlay.handleEvent(event);
      return;
    }

    switch (event.type) {
      case "beforeunload":
        this.requestCancelScreenshot("Navigation");
        break;
      case "resize":
        if (!this.#resizeTask && this.overlay?.initialized) {
          this.#resizeTask = new lazy.DeferredTask(() => {
            this.overlay.updateScreenshotsOverlayDimensions("resize");
          }, 16);
        }
        this.#resizeTask.arm();
        break;
      case "scroll":
        if (!this.#scrollTask && this.overlay?.initialized) {
          this.#scrollTask = new lazy.DeferredTask(() => {
            this.overlay.updateScreenshotsOverlayDimensions("scroll");
          }, 16);
        }
        this.#scrollTask.arm();
        break;
      case "Screenshots:Close":
        this.requestCancelScreenshot(event.detail.reason);
        break;
      case "Screenshots:Copy":
        this.requestCopyScreenshot(event.detail.region);
        break;
      case "Screenshots:Download":
        this.requestDownloadScreenshot(event.detail.region);
        break;
      case "Screenshots:OverlaySelection": {
        let { hasSelection, overlayState } = event.detail;
        this.sendOverlaySelection({ hasSelection, overlayState });
        break;
      }
      case "Screenshots:RecordEvent": {
        let { eventName, args } = event.detail;
        break;
      }
      case "Screenshots:ShowPanel":
        this.sendAsyncMessage("Screenshots:ShowPanel");
        break;
      case "Screenshots:HidePanel":
        this.sendAsyncMessage("Screenshots:HidePanel");
        break;
      case "Screenshots:FocusPanel":
        this.sendAsyncMessage("Screenshots:MoveFocusToParent", event.detail);
        break;
    }
  }

  requestCancelScreenshot(reason) {
    this.sendAsyncMessage("Screenshots:CancelScreenshot", {
      closeOverlay: false,
      reason,
    });
    this.endScreenshotsOverlay();
  }

  requestCopyScreenshot(region) {
    region.devicePixelRatio = this.contentWindow.devicePixelRatio;
    this.sendAsyncMessage("Screenshots:CopyScreenshot", { region });
    this.endScreenshotsOverlay({ doNotResetMethods: true });
  }

  requestDownloadScreenshot(region) {
    region.devicePixelRatio = this.contentWindow.devicePixelRatio;
    this.sendAsyncMessage("Screenshots:DownloadScreenshot", {
      title: this.getDocumentTitle(),
      region,
    });
    this.endScreenshotsOverlay({ doNotResetMethods: true });
  }

  getDocumentTitle() {
    return this.document.title;
  }

  sendOverlaySelection(data) {
    this.sendAsyncMessage("Screenshots:OverlaySelection", data);
  }

  getMethodsUsed() {
    let methodsUsed = this.#overlay.methodsUsed;
    this.#overlay.resetMethodsUsed();
    return methodsUsed;
  }

  focusOverlay(direction) {
    this.contentWindow.focus();
    this.#overlay.focus(direction);
  }

  documentIsReady() {
    const document = this.document;
    function readyEnough() {
      return (
        document.readyState !== "uninitialized" && document.documentElement
      );
    }

    if (readyEnough()) {
      return Promise.resolve();
    }
    return new Promise((resolve, reject) => {
      function onChange(event) {
        if (event.type === "pagehide") {
          document.removeEventListener("readystatechange", onChange);
          this.contentWindow.removeEventListener("pagehide", onChange);
          reject(new Error("document unloaded before it was ready"));
        } else if (readyEnough()) {
          document.removeEventListener("readystatechange", onChange);
          this.contentWindow.removeEventListener("pagehide", onChange);
          resolve();
        }
      }
      document.addEventListener("readystatechange", onChange);
      this.contentWindow.addEventListener("pagehide", onChange, { once: true });
    });
  }

  addEventListeners() {
    this.contentWindow.addEventListener("beforeunload", this);
    this.contentWindow.addEventListener("resize", this);
    this.contentWindow.addEventListener("scroll", this);
    this.addOverlayEventListeners();
  }

  addOverlayEventListeners() {
    let chromeEventHandler = this.docShell.chromeEventHandler;
    for (let event of ScreenshotsComponentChild.OVERLAY_EVENTS) {
      chromeEventHandler.addEventListener(event, this, true);
    }

    this.document.addEventListener("selectionchange", this);

    if (Services.prefs.getBoolPref(SCREENSHOTS_PREVENT_CONTENT_EVENTS_PREF)) {
      for (let event of ScreenshotsComponentChild.PREVENTABLE_EVENTS) {
        chromeEventHandler.addEventListener(event, this, true);
      }

      this.#preventableEventsAdded = true;
    }
  }

  async startScreenshotsOverlay() {
    try {
      await this.documentIsReady();
    } catch (ex) {
      console.warn(`ScreenshotsComponentChild: ${ex.message}`);
      return false;
    }
    await this.documentIsReady();
    let overlay =
      this.overlay ||
      (this.#overlay = new lazy.ScreenshotsOverlay(this.document));
    this.addEventListeners();

    overlay.initialize();
    return true;
  }

  removeEventListeners() {
    this.contentWindow.removeEventListener("beforeunload", this);
    this.contentWindow.removeEventListener("resize", this);
    this.contentWindow.removeEventListener("scroll", this);
    this.removeOverlayEventListeners();
  }

  removeOverlayEventListeners() {
    let chromeEventHandler = this.docShell.chromeEventHandler;
    for (let event of ScreenshotsComponentChild.OVERLAY_EVENTS) {
      chromeEventHandler.removeEventListener(event, this, true);
    }

    this.document.removeEventListener("selectionchange", this);

    if (this.#preventableEventsAdded) {
      for (let event of ScreenshotsComponentChild.PREVENTABLE_EVENTS) {
        chromeEventHandler.removeEventListener(event, this, true);
      }
    }

    this.#preventableEventsAdded = false;
  }

  endScreenshotsOverlay(options = {}) {
    this.removeEventListeners();

    this.overlay?.tearDown(options);
    this.#resizeTask?.disarm();
    this.#scrollTask?.disarm();
  }

  didDestroy() {
    this.#resizeTask?.disarm();
    this.#scrollTask?.disarm();
  }

  getFullPageBounds() {
    let {
      scrollMinX,
      scrollMinY,
      scrollWidth,
      scrollHeight,
      devicePixelRatio,
    } = this.#overlay.windowDimensions.dimensions;
    let rect = {
      left: scrollMinX,
      top: scrollMinY,
      right: scrollMinX + scrollWidth,
      bottom: scrollMinY + scrollHeight,
      width: scrollWidth,
      height: scrollHeight,
      devicePixelRatio,
    };
    return rect;
  }

  getVisibleBounds() {
    let {
      pageScrollX,
      pageScrollY,
      clientWidth,
      clientHeight,
      devicePixelRatio,
    } = this.#overlay.windowDimensions.dimensions;
    let rect = {
      left: pageScrollX,
      top: pageScrollY,
      right: pageScrollX + clientWidth,
      bottom: pageScrollY + clientHeight,
      width: clientWidth,
      height: clientHeight,
      devicePixelRatio,
    };
    return rect;
  }
}
