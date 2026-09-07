/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";


const lazy = XPCOMUtils.declareLazy({
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  logConsole: () =>
    console.createInstance({
      prefix: "SearchUtils",
      maxLogLevel: SearchUtils.loggingEnabled ? "Debug" : "Warn",
    }),
});

const BinaryInputStream = Components.Constructor(
  "@mozilla.org/binaryinputstream;1",
  "nsIBinaryInputStream",
  "setInputStream"
);

const BROWSER_SEARCH_PREF = "browser.search.";

class LoadListener {
  _bytes = [];
  _callback = null;
  _channel = null;
  _countRead = 0;
  _expectedContentType = null;
  _stream = null;
  QueryInterface = ChromeUtils.generateQI([
    Ci.nsIRequestObserver,
    Ci.nsIStreamListener,
    Ci.nsIChannelEventSink,
    Ci.nsIInterfaceRequestor,
    Ci.nsIProgressEventSink,
  ]);

  constructor(channel, expectedContentType, callback) {
    this._channel = channel;
    this._callback = callback;
    this._expectedContentType = expectedContentType;
  }

  onStartRequest(request) {
    lazy.logConsole.debug("loadListener: Starting request:", request.name);
    this._stream = Cc["@mozilla.org/binaryinputstream;1"].createInstance(
      Ci.nsIBinaryInputStream
    );
  }

  onStopRequest(request, statusCode) {
    lazy.logConsole.debug("loadListener: Stopping request:", request.name);

    var requestFailed = !Components.isSuccessCode(statusCode);
    if (!requestFailed && request instanceof Ci.nsIHttpChannel) {
      requestFailed = !request.requestSucceeded;
    }

    if (requestFailed || this._countRead == 0) {
      lazy.logConsole.debug("loadListener: request failed!");
      this._bytes = null;
    } else if (!this._expectedContentType.test(this._channel.contentType)) {
      lazy.logConsole.warn(
        "loadListener: Content type does not match expected",
        this._channel.contentType
      );
      this._bytes = null;
    }
    this._callback(this._bytes, this._bytes ? this._channel.contentType : "");
    this._channel = null;
  }

  onDataAvailable(request, inputStream, offset, count) {
    this._stream.setInputStream(inputStream);

    this._bytes = this._bytes.concat(this._stream.readByteArray(count));
    this._countRead += count;
  }

  asyncOnChannelRedirect(oldChannel, newChannel, flags, callback) {
    this._channel = newChannel;
    callback.onRedirectVerifyCallback(Cr.NS_OK);
  }

  getInterface(iid) {
    return this.QueryInterface(iid);
  }

  onProgress() {}
  onStatus() {}
}

export class SearchEngineInstallError extends Error {
  constructor(type, ...params) {
    super(...params);
    this.type = type;
  }
}

export var SearchUtils = {
  BROWSER_SEARCH_PREF,

  SETTINGS_IGNORELIST_KEY: "hijack-blocklists",

  SETTINGS_ALLOWLIST_KEY: "search-default-override-allowlist",

  SETTINGS_KEY: "search-config-v2",

  SETTINGS_OVERRIDES_KEY: "search-config-overrides-v2",

  TOPIC_SEARCH_SERVICE: "browser-search-service",

  TOPIC_ENGINE_MODIFIED: "browser-search-engine-modified",

  MODIFIED_TYPE: Object.freeze({
    CHANGED: "engine-changed",
    ICON_CHANGED: "engine-icon-changed",
    REMOVED: "engine-removed",
    ADDED: "engine-added",
    DEFAULT: "engine-default",
    DEFAULT_PRIVATE: "engine-default-private",
  }),

  URL_TYPE: Object.freeze({
    SUGGEST_JSON: "application/x-suggestions+json",
    SEARCH: "text/html",
    OPENSEARCH: "application/opensearchdescription+xml",
    TRENDING_JSON: "application/x-trending+json",
    SEARCH_FORM: "searchform",
    VISUAL_SEARCH: "application/x-visual-search+html",
  }),

  MAX_ICON_SIZE: 20000,

  get DEFAULT_QUERY_CHARSET() {
    return "UTF-8";
  },

  LoadListener,

  notifyAction(engine, verb) {
    if (lazy.SearchService.isInitialized) {
      lazy.logConsole.debug("NOTIFY: Engine:", engine.name, "Verb:", verb);
      // @ts-ignore XPConnect will automatically wrap the object, so it does
      Services.obs.notifyObservers(engine, this.TOPIC_ENGINE_MODIFIED, verb);
    }
  },

  makeURI(urlSpec) {
    try {
      return Services.io.newURI(urlSpec);
    } catch (ex) {}

    return null;
  },

  makeChannel(url, contentPolicyType, originAttributes = null) {
    if (!contentPolicyType) {
      throw new Error("makeChannel called with invalid content policy type");
    }
    try {
      let uri = typeof url == "string" ? Services.io.newURI(url) : url;
      if (!originAttributes) {
        originAttributes = {};
        try {
          originAttributes.firstPartyDomain =
            Services.eTLD.getSchemelessSite(uri);
        } catch {}
      }
      const principal =
        Services.scriptSecurityManager.createNullPrincipal(originAttributes);

      return Services.io.newChannelFromURI(
        uri,
        null ,
        principal,
        null ,
        Ci.nsILoadInfo.SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL,
        contentPolicyType
      );
    } catch (ex) {}

    return null;
  },

  isPartnerBuild() {
    return SearchUtils.distroID && !SearchUtils.distroID.startsWith("mozilla");
  },

  get SETTINGS_VERSION() {
    return 14;
  },

  get MODIFIED_APP_CHANNEL() {
    return AppConstants.IS_ESR ? "esr" : AppConstants.MOZ_UPDATE_CHANNEL;
  },

  sanitizeName(name) {
    const maxLength = 60;
    const minLength = 1;
    var result = name.toLowerCase();
    result = result.replace(/\s+/g, "-");
    result = result.replace(/[^-a-z0-9]/g, "");

    if (result.length < minLength) {
      result = Math.random().toString(36).replace(/^.*\./, "");
    }

    return result.substring(0, maxLength);
  },

  getVerificationHash(
    name,
    profileDirName = PathUtils.filename(PathUtils.profileDir)
  ) {
    let disclaimer =
      "By modifying this file, I agree that I am doing so " +
      "only within $appName itself, using official, user-driven search " +
      "engine selection processes, and in a way which does not circumvent " +
      "user consent. I acknowledge that any attempt to change this file " +
      "from outside of $appName is a malicious act, and will be responded " +
      "to accordingly.";

    let salt =
      profileDirName +
      name +
      disclaimer.replace(/\$appName/g, Services.appinfo.name);

    let data = new TextEncoder().encode(salt);
    let hasher = Cc["@mozilla.org/security/hash;1"].createInstance(
      Ci.nsICryptoHash
    );
    hasher.init(hasher.SHA256);
    hasher.update(data, data.length);

    return hasher.finish(true);
  },

  isSecureURIForOpenSearch(uri) {
    const loopbackAddresses = ["127.0.0.1", "[::1]", "localhost"];

    return (
      uri.schemeIs("https") ||
      loopbackAddresses.includes(uri.host) ||
      uri.host.toLowerCase().endsWith(".onion")
    );
  },

  sortEnginesByDefaults({
    engines,
    appDefaultEngine,
    appPrivateDefaultEngine,
    locale = Services.locale.appLocaleAsBCP47,
  }) {
    const sortedEngines = [];
    const addedEngines = new Set();

    function maybeAddEngineToSort(engine) {
      if (!engine || addedEngines.has(engine.name)) {
        return;
      }

      sortedEngines.push(engine);
      addedEngines.add(engine.name);
    }

    const appDefault = appDefaultEngine;
    maybeAddEngineToSort(appDefault);

    const appPrivateDefault = appPrivateDefaultEngine;
    if (appPrivateDefault && appPrivateDefault != appDefault) {
      maybeAddEngineToSort(appPrivateDefault);
    }

    const collator = new Intl.Collator(locale);

    let remainingEngines = engines.filter(e => !addedEngines.has(e.name));

    remainingEngines.sort((a, b) => {
      if (a.orderHint && b.orderHint) {
        if (a.orderHint == b.orderHint) {
          return collator.compare(a.name, b.name);
        }
        return b.orderHint - a.orderHint;
      }
      if (a.orderHint) {
        return -1;
      }
      if (b.orderHint) {
        return 1;
      }
      return collator.compare(a.name, b.name);
    });

    return [...sortedEngines, ...remainingEngines];
  },

  chooseIconSize(preferredSize, availableSizes) {
    availableSizes = availableSizes.toSorted((a, b) => b - a);
    let bestSize = availableSizes.shift();
    for (let currentSize of availableSizes) {
      if (currentSize >= preferredSize) {
        bestSize = currentSize;
      } else {
        if (
          bestSize > preferredSize &&
          preferredSize - currentSize < (bestSize - preferredSize) / 4
        ) {
          bestSize = currentSize;
        }
        break;
      }
    }

    return bestSize;
  },

  async fetchIcon(uri, originAttributes = null) {
    return new Promise((resolve, reject) => {
      let chan = SearchUtils.makeChannel(
        uri,
        Ci.nsIContentPolicy.TYPE_IMAGE,
        originAttributes
      );
      let listener = new SearchUtils.LoadListener(
        chan,
        /^image\//,
        (byteArray, contentType) => {
          if (!byteArray) {
            reject(new Error("Unable to fetch icon."));
            return;
          }
          resolve([Uint8Array.from(byteArray), contentType]);
        }
      );
      chan.notificationCallbacks = listener;
      chan.asyncOpen(listener);
    });
  },

  decodeSize(byteArray, contentType, fallbackSize = null) {
    if (contentType == "image/svg+xml") {
      let svgString;
      try {
        svgString = new TextDecoder("UTF-8", { fatal: true }).decode(byteArray);
      } catch {
        return fallbackSize;
      }
      let parser = new DOMParser();
      let doc = parser.parseFromString(svgString, contentType);
      if (doc.querySelector("parsererror")) {
        return fallbackSize;
      }
      if (SVGSVGElement.isInstance(doc.documentElement)) {
        let width = doc.documentElement.width.baseVal.value;
        let height = doc.documentElement.height.baseVal.value;
        if (width != height) {
          return fallbackSize;
        }
        return width;
      }
      return fallbackSize;
    }

    let imageTools = Cc["@mozilla.org/image/tools;1"].getService(Ci.imgITools);
    let imgDecoded;
    try {
      imgDecoded = imageTools.decodeImageFromArrayBuffer(
        byteArray.buffer,
        contentType
      );
    } catch {
      return fallbackSize;
    }
    if (imgDecoded.width != imgDecoded.height) {
      return fallbackSize;
    }

    return imgDecoded.width;
  },

  rescaleIcon(byteArray, contentType, size = 32) {
    if (contentType == "image/svg+xml") {
      throw new Error("Cannot rescale SVG image");
    }

    let imgTools = Cc["@mozilla.org/image/tools;1"].getService(Ci.imgITools);
    let container = imgTools.decodeImageFromArrayBuffer(
      byteArray.buffer,
      contentType
    );
    let stream = imgTools.encodeScaledImage(container, "image/png", size, size);
    let streamSize = stream.available();
    if (streamSize > SearchUtils.MAX_ICON_SIZE) {
      throw new Error("Rescaled icon still is too big");
    }

    let bis = new BinaryInputStream(stream);
    let newByteArray = new Uint8Array(streamSize);
    bis.readArrayBuffer(streamSize, newByteArray.buffer);
    return [newByteArray, "image/png"];
  },
};

XPCOMUtils.defineLazyPreferenceGetter(
  SearchUtils,
  "loggingEnabled",
  BROWSER_SEARCH_PREF + "log",
  false
);

ChromeUtils.defineLazyGetter(SearchUtils, "distroID", () => {
  return Services.prefs.getDefaultBranch("distribution.").getCharPref("id", "");
});
