/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export class DOMFullscreenParent extends JSWindowActorParent {
  waitingForChildEnterFullscreen = false;
  waitingForChildExitFullscreen = false;
  nextMsgRecipient = null;

  updateFullscreenWindowReference(aWindow) {
    if (aWindow.document.documentElement.hasAttribute("inDOMFullscreen")) {
      this._fullscreenWindow = aWindow;
    } else {
      delete this._fullscreenWindow;
    }
  }

  cleanupDomFullscreen(aWindow) {
    if (!aWindow.FullScreen) {
      return;
    }

    if (
      !aWindow.FullScreen.cleanupDomFullscreen(this) &&
      !aWindow.document.fullscreen
    ) {
      Services.obs.notifyObservers(aWindow, "fullscreen-painted");
    }
  }

  _cleanupFullscreenStateAndResumeChromeUI(aWindow) {
    this.cleanupDomFullscreen(aWindow);
    if (this.requestOrigin == this && aWindow.document.fullscreen) {
      aWindow.windowUtils.remoteFrameFullscreenReverted();
    }
  }

  didDestroy() {
    this._didDestroy = true;

    let window = this._fullscreenWindow;
    if (!window) {
      let topBrowsingContext = this.browsingContext.top;
      let browser = topBrowsingContext.embedderElement;
      if (!browser) {
        return;
      }

      if (
        this.waitingForChildExitFullscreen ||
        this.waitingForChildEnterFullscreen
      ) {
        this.waitingForChildExitFullscreen = false;
        this.waitingForChildEnterFullscreen = false;
        this._cleanupFullscreenStateAndResumeChromeUI(browser.documentGlobal);
      }

      if (this != this.requestOrigin) {
        this.removeListeners(browser.documentGlobal);
      }
      return;
    }

    if (this.waitingForChildEnterFullscreen) {
      this.waitingForChildEnterFullscreen = false;
      if (window.document.fullscreen) {
        window.document.exitFullscreen().catch(() => {});
        return;
      }
      this.cleanupDomFullscreen(window);
    }

    if (window.document.documentElement.hasAttribute("inDOMFullscreen")) {
      this.cleanupDomFullscreen(window);
      if (window.windowUtils) {
        window.windowUtils.remoteFrameFullscreenReverted();
      }
    } else if (this.waitingForChildExitFullscreen) {
      this.waitingForChildExitFullscreen = false;
      this._cleanupFullscreenStateAndResumeChromeUI(window);
    }
    this.updateFullscreenWindowReference(window);
  }

  receiveMessage(aMessage) {
    let topBrowsingContext = this.browsingContext.top;
    let browser = topBrowsingContext.embedderElement;

    if (!browser) {
      return;
    }

    let window = browser.documentGlobal;
    switch (aMessage.name) {
      case "DOMFullscreen:Request": {
        const keyboardLockEnabled = Services.prefs.getBoolPref(
          "dom.fullscreen.keyboard_lock.enabled",
          false
        );
        this.fullscreenKeyboardLock = keyboardLockEnabled
          ? aMessage.data.fullscreenKeyboardLock
          : "none";
        this.manager.fullscreen = true;
        this.waitingForChildExitFullscreen = false;
        this.requestOrigin = this;
        this.addListeners(window);
        window.windowUtils.remoteFrameFullscreenChanged(
          browser,
          this.fullscreenKeyboardLock == "browser"
        );
        break;
      }
      case "DOMFullscreen:NewOrigin": {
        if (window.document.fullscreen) {
          window.PointerlockFsWarning.showFullScreen(
            topBrowsingContext,
            window.document.fullscreenKeyboardLock == "browser"
          );
        }
        this.updateFullscreenWindowReference(window);
        break;
      }
      case "DOMFullscreen:Entered": {
        this.manager.fullscreen = true;
        this.nextMsgRecipient = null;
        this.waitingForChildEnterFullscreen = false;
        window.FullScreen.enterDomFullscreen(browser, this);
        this.updateFullscreenWindowReference(window);
        break;
      }
      case "DOMFullscreen:Exit": {
        this.manager.fullscreen = false;
        this.waitingForChildEnterFullscreen = false;
        window.windowUtils.remoteFrameFullscreenReverted();
        break;
      }
      case "DOMFullscreen:Exited": {
        this.manager.fullscreen = false;
        this.waitingForChildExitFullscreen = false;
        this.cleanupDomFullscreen(window);
        this.updateFullscreenWindowReference(window);
        break;
      }
      case "DOMFullscreen:Painted": {
        this.waitingForChildExitFullscreen = false;
        Services.obs.notifyObservers(window, "fullscreen-painted");
        this.sendAsyncMessage("DOMFullscreen:Painted", {});
        break;
      }
      case "DOMFullscreen:UpdateKeyboardLock": {
        const keyboardLockEnabled = Services.prefs.getBoolPref(
          "dom.fullscreen.keyboard_lock.enabled",
          false
        );
        let newLock =
          keyboardLockEnabled &&
          (aMessage.data.fullscreenKeyboardLock == "none" ||
            aMessage.data.fullscreenKeyboardLock == "browser")
            ? aMessage.data.fullscreenKeyboardLock
            : "none";
        if (window.document.fullscreenKeyboardLock != newLock) {
          this.manager.updateFullscreenKeyboardLockStatus(newLock);
          window.PointerlockFsWarning.close("fullscreen-warning");
          window.PointerlockFsWarning.showFullScreen(
            this.browsingContext,
            newLock == "browser"
          );
        }
        break;
      }
    }
  }

  handleEvent(aEvent) {
    let window = aEvent.currentTarget;
    let requestOrigin = window.browsingContext.fullscreenRequestOrigin?.get();
    if (this != requestOrigin) {
      this.removeListeners(window);
      return;
    }

    switch (aEvent.type) {
      case "MozDOMFullscreen:Entered": {
        let browser;
        if (aEvent.target.documentGlobal == window) {
          browser = aEvent.target;
        } else {
          browser = aEvent.target.documentGlobal.docShell.chromeEventHandler;
        }

        if (window.gXPInstallObserver) {
          window.gXPInstallObserver.removeAllNotifications(browser);
        }

        window.FullScreen.enterDomFullscreen(browser, this);
        this.updateFullscreenWindowReference(window);

        if (!this.hasBeenDestroyed() && this.requestOrigin) {
          window.PointerlockFsWarning.showFullScreen(
            this.requestOrigin.browsingContext,
            browser.documentGlobal.document.fullscreenKeyboardLock == "browser"
          );
        }
        break;
      }
      case "MozDOMFullscreen:Exited": {
        if (!this.hasBeenDestroyed() && !this.requestOrigin) {
          this.requestOrigin = this;
        }
        this.cleanupDomFullscreen(window);
        this.updateFullscreenWindowReference(window);

        if (!this.manager.fullscreen) {
          this.removeListeners(window);
        }
        break;
      }
      case "MozDOMFullscreen:WarnAboutKeyboardLock": {
        if (!this.hasBeenDestroyed() && this.requestOrigin) {
          window.PointerlockFsWarning.showFullScreen(
            this.requestOrigin.browsingContext,
            window.document.fullscreenKeyboardLock == "browser"
          );
        }
        break;
      }
    }
  }

  addListeners(aWindow) {
    aWindow.addEventListener(
      "MozDOMFullscreen:Entered",
      this,
       true,
      false
    );
    aWindow.addEventListener(
      "MozDOMFullscreen:Exited",
      this,
       true,
       false
    );
    aWindow.addEventListener(
      "MozDOMFullscreen:WarnAboutKeyboardLock",
      this,
       true,
       false
    );
  }

  removeListeners(aWindow) {
    aWindow.removeEventListener("MozDOMFullscreen:Entered", this, true);
    aWindow.removeEventListener("MozDOMFullscreen:Exited", this, true);
    aWindow.removeEventListener(
      "MozDOMFullscreen:WarnAboutKeyboardLock",
      this,
      true
    );
  }

  get requestOrigin() {
    let chromeBC = this.browsingContext.topChromeWindow?.browsingContext;
    let requestOrigin = chromeBC?.fullscreenRequestOrigin;
    return requestOrigin && requestOrigin.get();
  }

  set requestOrigin(aActor) {
    let chromeBC = this.browsingContext.topChromeWindow?.browsingContext;
    if (!chromeBC) {
      console.error("not able to get browsingContext for chrome window.");
      return;
    }

    if (aActor) {
      chromeBC.fullscreenRequestOrigin = Cu.getWeakReference(aActor);
    } else {
      delete chromeBC.fullscreenRequestOrigin;
    }
  }

  hasBeenDestroyed() {
    if (this._didDestroy) {
      return true;
    }

    try {
      return !this.browsingContext;
    } catch {
      return true;
    }
  }
}
