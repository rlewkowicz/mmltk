/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

export var PromptUtils = {
  fireDialogEvent(domWin, eventName, maybeTarget, detail) {
    let target = maybeTarget || domWin;
    let eventOptions = { cancelable: true, bubbles: true };
    if (detail) {
      eventOptions.detail = detail;
    }
    let event = new domWin.CustomEvent(eventName, eventOptions);
    let winUtils = domWin.windowUtils;
    winUtils.dispatchEventToChromeOnly(target, event);
  },

  objectToPropBag(obj) {
    let bag = Cc["@mozilla.org/hash-property-bag;1"].createInstance(
      Ci.nsIWritablePropertyBag2
    );
    bag.QueryInterface(Ci.nsIWritablePropertyBag);

    for (let propName in obj) {
      bag.setProperty(propName, obj[propName]);
    }

    return bag;
  },

  propBagToObject(propBag, obj) {
    for (let propName in obj) {
      obj[propName] = propBag.getProperty(propName);
    }
  },

  getAuthTarget(aChannel, aAuthInfo) {
    if (aAuthInfo.flags & Ci.nsIAuthInformation.AUTH_PROXY) {
      if (!(aChannel instanceof Ci.nsIProxiedChannel)) {
        throw new Error("proxy auth needs nsIProxiedChannel");
      }
      const info = aChannel.proxyInfo;
      if (!info) {
        throw new Error("proxy auth needs nsIProxyInfo");
      }
      const idnService = Cc["@mozilla.org/network/idn-service;1"].getService(
        Ci.nsIIDNService
      );
      const displayHost =
        "moz-proxy://" +
        idnService.domainToDisplay(info.host) +
        ":" +
        info.port;
      let realm = aAuthInfo.realm;
      if (!realm) {
        realm = displayHost;
      }
      return { displayHost, realm };
    }

    const displayHostOnly = aChannel.URI.displayHostPort;
    const displayHost = aChannel.URI.scheme + "://" + displayHostOnly;
    let realm = aAuthInfo.realm;
    if (!realm) {
      realm = displayHost;
    }
    return { displayHost, displayHostOnly, realm };
  },
};

export var EnableDelayHelper = function ({
  enableDialog,
  disableDialog,
  focusTarget,
}) {
  this.enableDialog = makeSafe(enableDialog);
  this.disableDialog = makeSafe(disableDialog);
  this.focusTarget = focusTarget;

  this.disableDialog();

  this.focusTarget.addEventListener("blur", this);
  this.focusTarget.addEventListener("focus", this);
  this.focusTarget.addEventListener("keyup", this, true);
  this.focusTarget.addEventListener("keydown", this, true);
  this.focusTarget.addEventListener("unload", this);

  let topWin = focusTarget.browsingContext.top.window;
  if (topWin != Services.focus.activeWindow) {
    return;
  }

  this.startOnFocusDelay();
};

EnableDelayHelper.prototype = {
  get delayTime() {
    return Services.prefs.getIntPref("security.dialog_enable_delay");
  },

  handleEvent(event) {
    switch (event.type) {
      case "keyup":
        this.focusTarget.removeEventListener("keyup", this, true);
        this.focusTarget.removeEventListener("keydown", this, true);
        break;

      case "keydown":
        if (this._focusTimer) {
          this._focusTimer.cancel();
          this._focusTimer = null;
          this.startOnFocusDelay();
          event.preventDefault();
        }
        break;

      case "blur":
        this.onBlur();
        break;

      case "focus":
        this.onFocus();
        break;

      case "unload":
        this.onUnload();
        break;
    }
  },

  onBlur() {
    this.disableDialog();
    if (this._focusTimer) {
      this._focusTimer.cancel();
      this._focusTimer = null;
    }
  },

  onFocus() {
    this.startOnFocusDelay();
  },

  onUnload() {
    this.focusTarget.removeEventListener("blur", this);
    this.focusTarget.removeEventListener("focus", this);
    this.focusTarget.removeEventListener("keyup", this, true);
    this.focusTarget.removeEventListener("keydown", this, true);
    this.focusTarget.removeEventListener("unload", this);

    if (this._focusTimer) {
      this._focusTimer.cancel();
      this._focusTimer = null;
    }

    this.focusTarget = this.enableDialog = this.disableDialog = null;
  },

  startOnFocusDelay() {
    if (this._focusTimer) {
      return;
    }

    this._focusTimer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
    this._focusTimer.initWithCallback(
      () => {
        this.onFocusTimeout();
      },
      this.delayTime,
      Ci.nsITimer.TYPE_ONE_SHOT
    );
  },

  onFocusTimeout() {
    this._focusTimer = null;
    this.enableDialog();
  },
};

function makeSafe(fn) {
  return function () {
    try {
      fn();
    } catch (e) {
      console.error(e);
    }
  };
}
