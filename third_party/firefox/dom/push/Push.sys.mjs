/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "console", () => {
  return console.createInstance({
    maxLogLevelPref: "dom.push.loglevel",
    prefix: "Push",
  });
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "PushService",
  "@mozilla.org/push/Service;1",
  Ci.nsIPushService
);

export class Push {
  constructor() {
    lazy.console.debug("Push()");
  }

  get contractID() {
    return "@mozilla.org/push/PushManager;1";
  }

  get classID() {
    return Components.ID("{cde1d019-fad8-4044-b141-65fb4fb7a245}");
  }

  get QueryInterface() {
    return ChromeUtils.generateQI([
      "nsIDOMGlobalPropertyInitializer",
      "nsISupportsWeakReference",
      "nsIObserver",
    ]);
  }

  init(win) {
    lazy.console.debug("init()");

    this._window = win;

    this._principal = win.clientPrincipal;

    if (!this._principal) {
      throw new Error(" The client principal of the window is not available");
    }

    try {
      this._topLevelPrincipal = win.top.document.nodePrincipal;
    } catch (error) {
      this._topLevelPrincipal = undefined;
    }
  }

  __init(scope) {
    this._scope = scope;
  }

  askPermission() {
    lazy.console.debug("askPermission()");

    let hasValidTransientUserGestureActivation =
      this._window.document.hasValidTransientUserGestureActivation;

    return new this._window.Promise((resolve, reject) => {
      if (this.#testPermission() === Ci.nsIPermissionManager.ALLOW_ACTION) {
        resolve();
        return;
      }

      let permissionDenied = () => {
        reject(
          new this._window.DOMException(
            "User denied permission to use the Push API.",
            "NotAllowedError"
          )
        );
      };

      if (
        Services.prefs.getBoolPref("dom.push.testing.ignorePermission", false)
      ) {
        resolve();
        return;
      }

      this.#requestPermission(
        hasValidTransientUserGestureActivation,
        resolve,
        permissionDenied
      );
    });
  }

  subscribe(options) {
    lazy.console.debug("subscribe()", this._scope);

    return this.askPermission().then(
      () =>
        new this._window.Promise((resolve, reject) => {
          let callback = new PushSubscriptionCallback(this, resolve, reject);

          if (!options || options.applicationServerKey === null) {
            lazy.PushService.subscribe(this._scope, this._principal, callback);
            return;
          }

          let keyView = this.#normalizeAppServerKey(
            options.applicationServerKey
          );
          if (keyView.byteLength === 0) {
            callback.rejectWithError(Cr.NS_ERROR_DOM_PUSH_INVALID_KEY_ERR);
            return;
          }
          lazy.PushService.subscribeWithKey(
            this._scope,
            this._principal,
            keyView,
            callback
          );
        })
    );
  }

  #normalizeAppServerKey(appServerKey) {
    let key;
    if (typeof appServerKey == "string") {
      try {
        key = Cu.cloneInto(
          ChromeUtils.base64URLDecode(appServerKey, {
            padding: "reject",
          }),
          this._window
        );
      } catch (e) {
        throw new this._window.DOMException(
          "String contains an invalid character",
          "InvalidCharacterError"
        );
      }
    } else if (this._window.ArrayBuffer.isView(appServerKey)) {
      key = appServerKey.buffer;
    } else {
      key = appServerKey;
    }
    return new this._window.Uint8Array(key);
  }

  getSubscription() {
    lazy.console.debug("getSubscription()", this._scope);

    return new this._window.Promise((resolve, reject) => {
      let callback = new PushSubscriptionCallback(this, resolve, reject);
      lazy.PushService.getSubscription(this._scope, this._principal, callback);
    });
  }

  permissionState() {
    lazy.console.debug("permissionState()", this._scope);

    return new this._window.Promise((resolve, reject) => {
      let permission = Ci.nsIPermissionManager.UNKNOWN_ACTION;

      try {
        permission = this.#testPermission();
      } catch (e) {
        reject();
        return;
      }

      let pushPermissionStatus = "prompt";
      if (permission == Ci.nsIPermissionManager.ALLOW_ACTION) {
        pushPermissionStatus = "granted";
      } else if (permission == Ci.nsIPermissionManager.DENY_ACTION) {
        pushPermissionStatus = "denied";
      }
      resolve(pushPermissionStatus);
    });
  }

  #testPermission() {
    let permission = Services.perms.testExactPermissionFromPrincipal(
      this._principal,
      "desktop-notification"
    );
    if (permission == Ci.nsIPermissionManager.ALLOW_ACTION) {
      return permission;
    }
    try {
      if (Services.prefs.getBoolPref("dom.push.testing.ignorePermission")) {
        permission = Ci.nsIPermissionManager.ALLOW_ACTION;
      }
    } catch (e) {}
    return permission;
  }

  #requestPermission(
    hasValidTransientUserGestureActivation,
    allowCallback,
    cancelCallback
  ) {
    let type = {
      type: "desktop-notification",
      options: [],
      QueryInterface: ChromeUtils.generateQI(["nsIContentPermissionType"]),
    };
    let typeArray = Cc["@mozilla.org/array;1"].createInstance(
      Ci.nsIMutableArray
    );
    typeArray.appendElement(type);

    let request = {
      QueryInterface: ChromeUtils.generateQI(["nsIContentPermissionRequest"]),
      types: typeArray,
      principal: this._principal,
      hasValidTransientUserGestureActivation,
      topLevelPrincipal: this._topLevelPrincipal,
      allow: allowCallback,
      cancel: cancelCallback,
      window: this._window,
    };

    let windowUtils = this._window.windowUtils;
    windowUtils.askPermission(request);
  }
}

class PushSubscriptionCallback {
  constructor(pushManager, resolve, reject) {
    this.pushManager = pushManager;
    this.resolve = resolve;
    this.reject = reject;
  }

  get QueryInterface() {
    return ChromeUtils.generateQI(["nsIPushSubscriptionCallback"]);
  }

  onPushSubscription(ok, subscription) {
    let { pushManager } = this;
    if (!Components.isSuccessCode(ok)) {
      this.rejectWithError(ok);
      return;
    }

    if (!subscription) {
      this.resolve(null);
      return;
    }

    let p256dhKey = this.#getKey(subscription, "p256dh");
    let authSecret = this.#getKey(subscription, "auth");
    let options = {
      endpoint: subscription.endpoint,
      scope: pushManager._scope,
      p256dhKey,
      authSecret,
    };
    let appServerKey = this.#getKey(subscription, "appServer");
    if (appServerKey) {
      options.appServerKey = appServerKey;
    }
    let sub = new pushManager._window.PushSubscription(options);
    this.resolve(sub);
  }

  #getKey(subscription, name) {
    let rawKey = Cu.cloneInto(
      subscription.getKey(name),
      this.pushManager._window
    );
    if (!rawKey.length) {
      return null;
    }

    let key = new this.pushManager._window.ArrayBuffer(rawKey.length);
    let keyView = new this.pushManager._window.Uint8Array(key);
    keyView.set(rawKey);
    return key;
  }

  rejectWithError(result) {
    let error;
    switch (result) {
      case Cr.NS_ERROR_DOM_PUSH_INVALID_KEY_ERR:
        error = new this.pushManager._window.DOMException(
          "Invalid raw ECDSA P-256 public key.",
          "InvalidAccessError"
        );
        break;

      case Cr.NS_ERROR_DOM_PUSH_MISMATCHED_KEY_ERR:
        error = new this.pushManager._window.DOMException(
          "A subscription with a different application server key already exists.",
          "InvalidStateError"
        );
        break;

      default:
        error = new this.pushManager._window.DOMException(
          "Error retrieving push subscription.",
          "AbortError"
        );
    }
    this.reject(error);
  }
}
