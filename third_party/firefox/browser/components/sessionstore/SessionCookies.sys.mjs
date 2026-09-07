/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PrivacyLevel: "resource://gre/modules/sessionstore/PrivacyLevel.sys.mjs",
});

const MAX_EXPIRY = Number.MAX_SAFE_INTEGER;

export var SessionCookies = Object.freeze({
  collect() {
    return SessionCookiesInternal.collect();
  },

  restore(cookies) {
    SessionCookiesInternal.restore(cookies);
  },
});

var SessionCookiesInternal = {
  _initialized: false,

  collect() {
    this._ensureInitialized();
    return CookieStore.toArray();
  },

  restore(cookies) {
    for (let cookie of cookies) {
      let expiry = "expiry" in cookie ? cookie.expiry : MAX_EXPIRY;
      let exists = false;
      try {
        exists = Services.cookies.cookieExists(
          cookie.host,
          cookie.path || "",
          cookie.name || "",
          cookie.originAttributes || {}
        );
      } catch (ex) {
        console.error(
          `CookieService::CookieExists failed with error '${ex}' for '${JSON.stringify(
            cookie
          )}'.`
        );
      }
      if (!exists) {
        let isPartitioned =
          cookie.isPartitioned ||
          cookie.originAttributes?.partitionKey?.length > 0;

        try {
          const cv = Services.cookies.add(
            cookie.host,
            cookie.path || "",
            cookie.name || "",
            cookie.value,
            !!cookie.secure,
            !!cookie.httponly,
             true,
            expiry,
            cookie.originAttributes || {},
            cookie.sameSite === undefined
              ? Ci.nsICookie.SAMESITE_NONE
              : cookie.sameSite,
            cookie.schemeMap || Ci.nsICookie.SCHEME_HTTPS,
            isPartitioned
          );
          if (cv.result !== Ci.nsICookieValidation.eOK) {
            console.error(
              `CookieService::Add failed with error '${cv.result}' for cookie ${JSON.stringify(
                cookie
              )}.`
            );
          }
        } catch (ex) {
          console.error(
            `CookieService::Add failed with error '${ex}' for cookie ${JSON.stringify(
              cookie
            )}.`
          );
        }
      }
    }
  },

  observe(subject) {
    let notification = subject.QueryInterface(Ci.nsICookieNotification);

    let {
      COOKIE_DELETED,
      COOKIE_ADDED,
      COOKIE_CHANGED,
      ALL_COOKIES_CLEARED,
      COOKIES_BATCH_DELETED,
    } = Ci.nsICookieNotification;

    switch (notification.action) {
      case COOKIE_ADDED:
        this._addCookie(notification.cookie);
        break;
      case COOKIE_CHANGED:
        this._updateCookie(notification.cookie);
        break;
      case COOKIE_DELETED:
        this._removeCookie(notification.cookie);
        break;
      case ALL_COOKIES_CLEARED:
        CookieStore.clear();
        break;
      case COOKIES_BATCH_DELETED:
        this._removeCookies(notification.batchDeletedCookies);
        break;
      default:
        throw new Error("Unhandled session-cookie-changed notification.");
    }
  },

  _ensureInitialized() {
    if (this._initialized) {
      return;
    }
    this._reloadCookies();
    this._initialized = true;
    Services.obs.addObserver(this, "session-cookie-changed");

    Services.prefs.addObserver("browser.sessionstore.privacy_level", () => {
      this._reloadCookies();
    });
  },

  _addCookie(cookie) {
    cookie.QueryInterface(Ci.nsICookie);

    if (cookie.isSession && lazy.PrivacyLevel.canSave(cookie.isSecure)) {
      CookieStore.add(cookie);
    }
  },

  _updateCookie(cookie) {
    cookie.QueryInterface(Ci.nsICookie);

    if (cookie.isSession && lazy.PrivacyLevel.canSave(cookie.isSecure)) {
      CookieStore.add(cookie);
    } else {
      CookieStore.delete(cookie);
    }
  },

  _removeCookie(cookie) {
    cookie.QueryInterface(Ci.nsICookie);

    if (cookie.isSession) {
      CookieStore.delete(cookie);
    }
  },

  _removeCookies(cookies) {
    for (let i = 0; i < cookies.length; i++) {
      this._removeCookie(cookies.queryElementAt(i, Ci.nsICookie));
    }
  },

  _reloadCookies() {
    CookieStore.clear();

    if (!lazy.PrivacyLevel.canSave(false)) {
      return;
    }

    for (let cookie of Services.cookies.sessionCookies) {
      this._addCookie(cookie);
    }
  },
};

var CookieStore = {
  _entries: new Map(),

  add(cookie) {
    let jscookie = { host: cookie.host, value: cookie.value };

    if (cookie.path) {
      jscookie.path = cookie.path;
    }

    if (cookie.name) {
      jscookie.name = cookie.name;
    }

    if (cookie.isSecure) {
      jscookie.secure = true;
    }

    if (cookie.isHttpOnly) {
      jscookie.httponly = true;
    }

    if (cookie.expiry < MAX_EXPIRY) {
      jscookie.expiry = cookie.expiry;
    }

    if (cookie.originAttributes) {
      jscookie.originAttributes = cookie.originAttributes;
    }

    jscookie.sameSite = cookie.sameSite;

    if (cookie.schemeMap) {
      jscookie.schemeMap = cookie.schemeMap;
    }

    if (cookie.isPartitioned) {
      jscookie.isPartitioned = true;
    }

    this._entries.set(this._getKeyForCookie(cookie), jscookie);
  },

  delete(cookie) {
    this._entries.delete(this._getKeyForCookie(cookie));
  },

  clear() {
    this._entries.clear();
  },

  toArray() {
    return [...this._entries.values()];
  },

  _getKeyForCookie(cookie) {
    return JSON.stringify({
      host: cookie.host,
      name: cookie.name,
      path: cookie.path,
      attr: ChromeUtils.originAttributesToSuffix(cookie.originAttributes),
    });
  },
};
