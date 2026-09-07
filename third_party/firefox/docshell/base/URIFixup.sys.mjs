/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */



import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "externalProtocolService",
  "@mozilla.org/uriloader/external-protocol-service;1",
  Ci.nsIExternalProtocolService
);

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "defaultProtocolHandler",
  "@mozilla.org/network/protocol;1?name=default",
  Ci.nsIProtocolHandler
);

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "fileProtocolHandler",
  "@mozilla.org/network/protocol;1?name=file",
  Ci.nsIFileProtocolHandler
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "fixupSchemeTypos",
  "browser.fixup.typo.scheme",
  true
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "dnsFirstForSingleWords",
  "browser.fixup.dns_first_for_single_words",
  false
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "keywordEnabled",
  "keyword.enabled",
  true
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "alternateProtocol",
  "browser.fixup.alternate.protocol",
  "https"
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "dnsResolveFullyQualifiedNames",
  "browser.urlbar.dnsResolveFullyQualifiedNames",
  true
);

const {
  FIXUP_FLAG_NONE,
  FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP,
  FIXUP_FLAGS_MAKE_ALTERNATE_URI,
  FIXUP_FLAG_PRIVATE_CONTEXT,
  FIXUP_FLAG_FIX_SCHEME_TYPOS,
} = Ci.nsIURIFixup;

const COMMON_PROTOCOLS = ["http", "https", "file"];

const HTTPISH = new Set(["http", "https"]);

ChromeUtils.defineLazyGetter(
  lazy,
  "userPasswordRegex",
  () => /^([a-z+.-]+:\/{0,3})*([^\/@]+@).+/i
);

ChromeUtils.defineLazyGetter(lazy, "portRegex", () => /^:\d{1,5}([?#/]|$)/);

ChromeUtils.defineLazyGetter(lazy, "numberRegex", () => /^[0-9]+(\.[0-9]+)?$/);

ChromeUtils.defineLazyGetter(lazy, "maxOneTabRegex", () => /^[^\t]*\t?[^\t]*$/);

ChromeUtils.defineLazyGetter(
  lazy,
  "possiblyHostPortRegex",
  () => /^[a-z0-9-]+(\.[a-z0-9-]+)*:[0-9]{1,5}([/?#]|$)/i
);

ChromeUtils.defineLazyGetter(lazy, "newLinesRegex", () => /[\r\n]/g);

ChromeUtils.defineLazyGetter(
  lazy,
  "possibleProtocolRegex",
  () => /^([a-z][a-z0-9.+\t-]*)(:|;)?(\/\/)?/i
);

ChromeUtils.defineLazyGetter(
  lazy,
  "IPv4LikeRegex",
  () => /^(?:[a-z+.-]+:\/*(?!\/))?(?:\d{1,3}\.){2,3}\d{1,3}(?::\d+|\/)?/i
);
ChromeUtils.defineLazyGetter(
  lazy,
  "IPv6LikeRegex",
  () =>
    /^(?:[a-z+.-]+:\/*(?!\/))?\[(?:[0-9a-f]{0,4}:){0,7}[0-9a-f]{0,4}\]?(?::\d+|\/)?/i
);

ChromeUtils.defineLazyGetter(lazy, "knownDomains", () => {
  const branch = "browser.fixup.domainwhitelist.";
  let domains = new Set(
    Services.prefs
      .getChildList(branch)
      .filter(p => Services.prefs.getBoolPref(p, false))
      .map(p => p.substring(branch.length))
  );
  domains._observer = {
    observe(subject, topic, data) {
      let domain = data.substring(branch.length);
      if (Services.prefs.getBoolPref(data, false)) {
        domains.add(domain);
      } else {
        domains.delete(domain);
      }
    },
    QueryInterface: ChromeUtils.generateQI([
      "nsIObserver",
      "nsISupportsWeakReference",
    ]),
  };
  Services.prefs.addObserver(branch, domains._observer, true);
  return domains;
});

ChromeUtils.defineLazyGetter(lazy, "knownSuffixes", () => {
  const branch = "browser.fixup.domainsuffixwhitelist.";
  let suffixes = new Map();
  let prefs = Services.prefs
    .getChildList(branch)
    .filter(p => Services.prefs.getBoolPref(p, false));
  for (let pref of prefs) {
    let suffix = pref.substring(branch.length);
    let lastPart = suffix.substr(suffix.lastIndexOf(".") + 1);
    if (lastPart) {
      let entries = suffixes.get(lastPart);
      if (!entries) {
        entries = new Set();
        suffixes.set(lastPart, entries);
      }
      entries.add(suffix);
    }
  }
  suffixes._observer = {
    observe(subject, topic, data) {
      let suffix = data.substring(branch.length);
      let lastPart = suffix.substr(suffix.lastIndexOf(".") + 1);
      let entries = suffixes.get(lastPart);
      if (Services.prefs.getBoolPref(data, false)) {
        if (!entries) {
          entries = new Set();
          suffixes.set(lastPart, entries);
        }
        entries.add(suffix);
      } else if (entries) {
        entries.delete(suffix);
        if (!entries.size) {
          suffixes.delete(lastPart);
        }
      }
    },
    QueryInterface: ChromeUtils.generateQI([
      "nsIObserver",
      "nsISupportsWeakReference",
    ]),
  };
  Services.prefs.addObserver(branch, suffixes._observer, true);
  return suffixes;
});

export function URIFixup() {}

URIFixup.prototype = {
  get FIXUP_FLAG_NONE() {
    return FIXUP_FLAG_NONE;
  },
  get FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP() {
    return FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP;
  },
  get FIXUP_FLAGS_MAKE_ALTERNATE_URI() {
    return FIXUP_FLAGS_MAKE_ALTERNATE_URI;
  },
  get FIXUP_FLAG_PRIVATE_CONTEXT() {
    return FIXUP_FLAG_PRIVATE_CONTEXT;
  },
  get FIXUP_FLAG_FIX_SCHEME_TYPOS() {
    return FIXUP_FLAG_FIX_SCHEME_TYPOS;
  },

  getFixupURIInfo(uriString, fixupFlags = FIXUP_FLAG_NONE) {
    let isPrivateContext = fixupFlags & FIXUP_FLAG_PRIVATE_CONTEXT;
    let untrimmedURIString = uriString;

    uriString = uriString.trim().replace(lazy.newLinesRegex, "");

    if (!uriString) {
      throw new Components.Exception(
        "Should pass a non-null uri",
        Cr.NS_ERROR_FAILURE
      );
    }

    let info = new URIFixupInfo(uriString);

    const { scheme, fixedSchemeUriString, fixupChangedProtocol } =
      extractScheme(uriString, fixupFlags);
    uriString = fixedSchemeUriString;
    info.fixupChangedProtocol = fixupChangedProtocol;

    if (scheme == "view-source") {
      let { preferredURI, postData } = fixupViewSource(uriString, fixupFlags);
      info.preferredURI = info.fixedURI = preferredURI;
      info.postData = postData;
      return info;
    }

    if (scheme.length < 2) {
      let fileURI = fileURIFixup(uriString);
      if (fileURI) {
        info.preferredURI = info.fixedURI = fileURI;
        info.fixupChangedProtocol = true;
        return info;
      }
    }

    const isCommonProtocol = COMMON_PROTOCOLS.includes(scheme);

    let canHandleProtocol =
      scheme &&
      (isCommonProtocol ||
        Services.io.getProtocolHandler(scheme) != lazy.defaultProtocolHandler ||
        this._isKnownExternalProtocol(scheme));

    if (
      canHandleProtocol ||
      (!lazy.possiblyHostPortRegex.test(uriString) &&
        !lazy.userPasswordRegex.test(uriString))
    ) {
      try {
        info.fixedURI = makeURIWithFixedLocalHosts(uriString, fixupFlags);
      } catch (ex) {
        if (ex.result != Cr.NS_ERROR_MALFORMED_URI) {
          throw ex;
        }
      }
    }

    if (
      info.fixedURI &&
      lazy.keywordEnabled &&
      fixupFlags & FIXUP_FLAG_FIX_SCHEME_TYPOS &&
      scheme &&
      !canHandleProtocol
    ) {
      tryKeywordFixupForURIInfo(uriString, info, isPrivateContext);
    }

    if (info.fixedURI) {
      if (!info.preferredURI) {
        maybeSetAlternateFixedURI(info, fixupFlags);
        info.preferredURI = info.fixedURI;
      }
      fixupConsecutiveDotsHost(info);
      return info;
    }

    let inputHadDuffProtocol =
      uriString.startsWith("://") || uriString.startsWith("//");
    if (inputHadDuffProtocol) {
      uriString = uriString.replace(/^:?\/\//, "");
    }

    let detectSpaceInCredentials = val => {
      let firstChars = val.slice(0, 512);
      if (!firstChars.includes("@")) {
        return false;
      }
      let credentials = firstChars.split("@")[0];
      return !credentials.includes("/") && /\s/.test(credentials);
    };

    if (
      !isCommonProtocol &&
      lazy.maxOneTabRegex.test(uriString) &&
      !detectSpaceInCredentials(untrimmedURIString)
    ) {
      let uriWithProtocol = fixupURIProtocol(uriString, fixupFlags);
      if (uriWithProtocol) {
        info.fixedURI = uriWithProtocol;
        info.fixupChangedProtocol = true;
        info.schemelessInput = Ci.nsILoadInfo.SchemelessInputTypeSchemeless;
        maybeSetAlternateFixedURI(info, fixupFlags);
        info.preferredURI = info.fixedURI;
        if (uriString.endsWith("/")) {
          fixupConsecutiveDotsHost(info);
          return info;
        }
      }
    }

    const asciiHost = info.fixedURI?.asciiHost;
    if (
      asciiHost?.length > 4 &&
      asciiHost?.startsWith("www.") &&
      asciiHost?.lastIndexOf(".") == 3
    ) {
      return info;
    }

    let suffixInfo;
    function checkSuffix(i) {
      if (!suffixInfo) {
        suffixInfo = checkAndFixPublicSuffix(i);
      }
      return suffixInfo;
    }

    if (
      lazy.keywordEnabled &&
      fixupFlags & FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP &&
      !inputHadDuffProtocol &&
      !checkSuffix(info).suffix &&
      keywordURIFixup(uriString, info, isPrivateContext)
    ) {
      fixupConsecutiveDotsHost(info);
      return info;
    }

    if (
      info.fixedURI &&
      (!info.fixupChangedProtocol || !checkSuffix(info).hasUnknownSuffix)
    ) {
      fixupConsecutiveDotsHost(info);
      return info;
    }

    if (lazy.keywordEnabled && fixupFlags & FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP) {
      tryKeywordFixupForURIInfo(info.originalInput, info, isPrivateContext);
    }

    if (!info.preferredURI) {
      throw new Components.Exception(
        "Couldn't build a valid uri",
        Cr.NS_ERROR_MALFORMED_URI
      );
    }

    fixupConsecutiveDotsHost(info);
    return info;
  },

  webNavigationFlagsToFixupFlags(href, navigationFlags) {
    try {
      Services.io.newURI(href);
      navigationFlags &=
        ~Ci.nsIWebNavigation.LOAD_FLAGS_ALLOW_THIRD_PARTY_FIXUP;
    } catch (ex) {}

    let fixupFlags = FIXUP_FLAG_NONE;
    if (
      navigationFlags & Ci.nsIWebNavigation.LOAD_FLAGS_ALLOW_THIRD_PARTY_FIXUP
    ) {
      fixupFlags |= FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP;
    }
    if (navigationFlags & Ci.nsIWebNavigation.LOAD_FLAGS_FIXUP_SCHEME_TYPOS) {
      fixupFlags |= FIXUP_FLAG_FIX_SCHEME_TYPOS;
    }
    return fixupFlags;
  },

  keywordToURI(keyword, isPrivateContext) {
    if (Services.appinfo.processType == Ci.nsIXULRuntime.PROCESS_TYPE_CONTENT) {
      throw new Components.Exception(
        "Can't invoke URIFixup in the content process",
        Cr.NS_ERROR_NOT_AVAILABLE
      );
    }
    let info = new URIFixupInfo(keyword);

    if (keyword.startsWith("?")) {
      keyword = keyword.substring(1);
    }
    keyword = keyword.trim();

    if (!lazy.SearchService.hasSuccessfullyInitialized) {
      return info;
    }

    let engine = isPrivateContext
      ? lazy.SearchService.defaultPrivateEngine
      : lazy.SearchService.defaultEngine;

    let responseType = null;
    if (engine.supportsResponseType("application/x-moz-keywordsearch")) {
      responseType = "application/x-moz-keywordsearch";
    }
    let submission = engine.getSubmission(keyword, responseType);
    if (
      !submission ||
      !HTTPISH.has(submission.uri.scheme)
    ) {
      throw new Components.Exception(
        "Invalid search submission uri",
        Cr.NS_ERROR_NOT_AVAILABLE
      );
    }
    let submissionPostDataStream = submission.postData;
    if (submissionPostDataStream) {
      info.postData = submissionPostDataStream;
    }

    info.keywordProviderId = engine.id;
    info.keywordAsSent = keyword;
    info.preferredURI = submission.uri;
    return info;
  },

  forceHttpFixup(uriString) {
    if (!uriString) {
      throw new Components.Exception(
        "Should pass a non-null uri",
        Cr.NS_ERROR_FAILURE
      );
    }

    let info = new URIFixupInfo(uriString);
    let { scheme, fixedSchemeUriString, fixupChangedProtocol } = extractScheme(
      uriString,
      FIXUP_FLAG_FIX_SCHEME_TYPOS
    );

    if (!HTTPISH.has(scheme)) {
      throw new Components.Exception(
        "Scheme should be either http or https",
        Cr.NS_ERROR_FAILURE
      );
    }

    info.fixupChangedProtocol = fixupChangedProtocol;
    info.fixedURI = Services.io.newURI(fixedSchemeUriString);

    let host = info.fixedURI.host;
    if (!HTTPISH.has(host) && host != "localhost") {
      let modifiedHostname = maybeAddPrefixAndSuffix(host);
      updateHostAndScheme(info, modifiedHostname);
      info.preferredURI = info.fixedURI;
    }

    return info;
  },

  checkHost(uri, listener, originAttributes) {
    let { displayHost, asciiHost } = uri;
    if (!displayHost) {
      throw new Components.Exception(
        "URI must have displayHost",
        Cr.NS_ERROR_FAILURE
      );
    }
    if (!asciiHost) {
      throw new Components.Exception(
        "URI must have asciiHost",
        Cr.NS_ERROR_FAILURE
      );
    }

    let isIPv4Address = host => {
      let parts = host.split(".");
      if (parts.length != 4) {
        return false;
      }
      return parts.every(part => {
        let n = parseInt(part, 10);
        return n >= 0 && n <= 255;
      });
    };

    if (isIPv4Address(asciiHost) || /^(?:\d+|0x[a-f0-9]+)$/i.test(asciiHost)) {
      return;
    }

    let lookupName = displayHost;
    if (lazy.dnsResolveFullyQualifiedNames && !lookupName.includes(".")) {
      lookupName += ".";
    }

    Services.obs.notifyObservers(null, "uri-fixup-check-dns");
    Services.dns.asyncResolve(
      lookupName,
      Ci.nsIDNSService.RESOLVE_TYPE_DEFAULT,
      0,
      null,
      listener,
      Services.tm.mainThread,
      originAttributes
    );
  },

  isDomainKnown,

  _isKnownExternalProtocol(scheme) {
    if (AppConstants.platform == "android") {
      return false;
    }
    return lazy.externalProtocolService.externalProtocolHandlerExists(scheme);
  },

  classID: Components.ID("{c6cf88b7-452e-47eb-bdc9-86e3561648ef}"),
  QueryInterface: ChromeUtils.generateQI(["nsIURIFixup"]),
};

export function URIFixupInfo(originalInput = "") {
  this._originalInput = originalInput;
}

URIFixupInfo.prototype = {
  set consumer(consumer) {
    this._consumer = consumer || null;
  },
  get consumer() {
    return this._consumer || null;
  },

  set preferredURI(uri) {
    this._preferredURI = uri;
  },
  get preferredURI() {
    return this._preferredURI || null;
  },

  set fixedURI(uri) {
    this._fixedURI = uri;
  },
  get fixedURI() {
    return this._fixedURI || null;
  },

  set keywordProviderId(id) {
    this._keywordProviderId = id;
  },
  get keywordProviderId() {
    return this._keywordProviderId || "";
  },

  set keywordAsSent(keyword) {
    this._keywordAsSent = keyword;
  },
  get keywordAsSent() {
    return this._keywordAsSent || "";
  },

  set schemelessInput(changed) {
    this._schemelessInput = changed;
  },
  get schemelessInput() {
    return this._schemelessInput ?? Ci.nsILoadInfo.SchemelessInputTypeUnset;
  },

  set fixupChangedProtocol(changed) {
    this._fixupChangedProtocol = changed;
  },
  get fixupChangedProtocol() {
    return !!this._fixupChangedProtocol;
  },

  set fixupCreatedAlternateURI(changed) {
    this._fixupCreatedAlternateURI = changed;
  },
  get fixupCreatedAlternateURI() {
    return !!this._fixupCreatedAlternateURI;
  },

  set originalInput(input) {
    this._originalInput = input;
  },
  get originalInput() {
    return this._originalInput || "";
  },

  set postData(postData) {
    this._postData = postData;
  },
  get postData() {
    return this._postData || null;
  },

  classID: Components.ID("{33d75835-722f-42c0-89cc-44f328e56a86}"),
  QueryInterface: ChromeUtils.generateQI(["nsIURIFixupInfo"]),
};


function isDomainKnown(asciiHost) {
  if (lazy.dnsFirstForSingleWords) {
    return true;
  }
  let lastDotIndex = asciiHost.lastIndexOf(".");
  if (lastDotIndex == asciiHost.length - 1) {
    asciiHost = asciiHost.substring(0, asciiHost.length - 1);
    lastDotIndex = asciiHost.lastIndexOf(".");
  }
  if (lazy.knownDomains.has(asciiHost.toLowerCase())) {
    return true;
  }
  if (lastDotIndex <= 0) {
    return false;
  }
  let lastPart = asciiHost.substr(lastDotIndex + 1);
  let suffixes = lazy.knownSuffixes.get(lastPart);
  if (suffixes) {
    return Array.from(suffixes).some(s => asciiHost.endsWith(s));
  }
  return false;
}

function checkAndFixPublicSuffix(info) {
  let uri = info.fixedURI;
  let asciiHost = uri?.asciiHost;

  if (
    !asciiHost ||
    !asciiHost.includes(".") ||
    (asciiHost.endsWith(".") && !info.originalInput.endsWith("。")) ||
    isDomainKnown(asciiHost)
  ) {
    return { suffix: "", hasUnknownSuffix: false };
  }

  if (
    /^\w/.test(asciiHost) &&
    (asciiHost.endsWith(".com") ||
      asciiHost.endsWith(".net") ||
      asciiHost.endsWith(".org") ||
      asciiHost.endsWith(".ru") ||
      asciiHost.endsWith(".de"))
  ) {
    return {
      suffix: asciiHost.substring(asciiHost.lastIndexOf(".") + 1),
      hasUnknownSuffix: false,
    };
  }
  try {
    let suffix = Services.eTLD.getKnownPublicSuffix(uri);
    if (suffix) {
      return { suffix, hasUnknownSuffix: false };
    }
  } catch (ex) {
    return { suffix: "", hasUnknownSuffix: false };
  }
  let suffix = Services.eTLD.getPublicSuffix(uri);
  if (!suffix || lazy.numberRegex.test(suffix)) {
    return { suffix: "", hasUnknownSuffix: false };
  }
  for (let [typo, fixed] of [
    ["ocm", "com"],
    ["con", "com"],
    ["cmo", "com"],
    ["xom", "com"],
    ["vom", "com"],
    ["cpm", "com"],
    ["com'", "com"],
    ["ent", "net"],
    ["ner", "net"],
    ["nte", "net"],
    ["met", "net"],
    ["rog", "org"],
    ["ogr", "org"],
    ["prg", "org"],
    ["orh", "org"],
  ]) {
    if (suffix == typo) {
      let host = uri.host.substring(0, uri.host.length - typo.length) + fixed;
      let updatePreferredURI = info.preferredURI == info.fixedURI;
      info.fixedURI = uri.mutate().setHost(host).finalize();
      if (updatePreferredURI) {
        info.preferredURI = info.fixedURI;
      }
      return { suffix: fixed, hasUnknownSuffix: false };
    }
  }
  return { suffix: "", hasUnknownSuffix: true };
}

function tryKeywordFixupForURIInfo(uriString, fixupInfo, isPrivateContext) {
  try {
    let keywordInfo = Services.uriFixup.keywordToURI(
      uriString,
      isPrivateContext
    );
    fixupInfo.keywordProviderId = keywordInfo.keywordProviderId;
    fixupInfo.keywordAsSent = keywordInfo.keywordAsSent;
    fixupInfo.preferredURI = keywordInfo.preferredURI;
    return true;
  } catch (ex) {}
  return false;
}

function maybeSetAlternateFixedURI(info, fixupFlags) {
  let uri = info.fixedURI;
  if (
    !(fixupFlags & FIXUP_FLAGS_MAKE_ALTERNATE_URI) ||
    !uri.schemeIs("http") ||
    uri.userPass ||
    uri.port != -1
  ) {
    return false;
  }

  let oldHost = uri.host;
  if (oldHost == "localhost" || HTTPISH.has(oldHost)) {
    return false;
  }

  let newHost = maybeAddPrefixAndSuffix(oldHost);

  if (newHost == oldHost) {
    return false;
  }

  return updateHostAndScheme(info, newHost);
}

function fileURIFixup(uriString) {
  let attemptFixup = false;
  let path = uriString;
  if (AppConstants.platform == "win") {
    attemptFixup =
      uriString.includes("\\") ||
      (uriString[1] == ":" && (uriString.length == 2 || uriString[2] == "/"));
    if (uriString[1] == ":" && uriString[2] == "/") {
      path = uriString.replace(/\//g, "\\");
    }
  } else {
    attemptFixup = /^[~/]/.test(uriString);
  }
  if (attemptFixup) {
    try {
      let file = Cc["@mozilla.org/file/local;1"].createInstance(Ci.nsIFile);
      file.initWithPath(path);
      return Services.io.newURI(
        lazy.fileProtocolHandler.getURLSpecFromActualFile(file)
      );
    } catch (ex) {
    }
  }
  return null;
}

function fixupURIProtocol(uriString, fixupFlags) {
  let schemeChars = uriString.slice(0, 39);

  let schemePos = schemeChars.indexOf("://");
  if (schemePos == -1 || schemePos > schemeChars.search(/[:\/]/)) {
    uriString = "http://" + uriString;
  }
  try {
    return makeURIWithFixedLocalHosts(uriString, fixupFlags);
  } catch (ex) {
  }
  return null;
}

function makeURIWithFixedLocalHosts(uriString, fixupFlags) {
  let uri = Services.io.newURI(uriString);

  if (fixupFlags & FIXUP_FLAG_FIX_SCHEME_TYPOS && HTTPISH.has(uri.scheme)) {
    if (uri.host == "0.0.0.0") {
      uri = uri.mutate().setHost("127.0.0.1").finalize();
    } else if (uri.host == "::") {
      uri = uri.mutate().setHost("[::1]").finalize();
    }
  }
  return uri;
}

function keywordURIFixup(uriString, fixupInfo, isPrivateContext) {


  if (uriString.startsWith("?")) {
    return tryKeywordFixupForURIInfo(
      fixupInfo.originalInput,
      fixupInfo,
      isPrivateContext
    );
  }

  const userPassword = lazy.userPasswordRegex.exec(uriString);
  const ipString = userPassword
    ? uriString.replace(userPassword[2], "")
    : uriString;
  if (lazy.IPv4LikeRegex.test(ipString) || lazy.IPv6LikeRegex.test(ipString)) {
    return false;
  }

  let asciiHost = fixupInfo.fixedURI?.asciiHost;
  if (
    asciiHost &&
    (isDomainKnown(asciiHost) ||
      (asciiHost.endsWith(".") &&
        asciiHost.indexOf(".") != asciiHost.length - 1))
  ) {
    return false;
  }

  if (fixupInfo.fixedURI?.password) {
    return false;
  }

  if (
    !isURILike(uriString, fixupInfo.fixedURI?.displayHost) ||
    (fixupInfo.fixedURI?.userPass && fixupInfo.fixedURI?.pathQueryRef === "/")
  ) {
    return tryKeywordFixupForURIInfo(
      fixupInfo.originalInput,
      fixupInfo,
      isPrivateContext
    );
  }

  return false;
}

function extractScheme(uriString, fixupFlags = FIXUP_FLAG_NONE) {
  const matches = uriString.match(lazy.possibleProtocolRegex);
  const hasColon = matches?.[2] === ":";
  const hasSlash2 = matches?.[3] === "//";

  const isFixupSchemeTypos =
    lazy.fixupSchemeTypos && fixupFlags & FIXUP_FLAG_FIX_SCHEME_TYPOS;

  if (
    !matches ||
    (!hasColon && !hasSlash2) ||
    (!hasColon && !isFixupSchemeTypos)
  ) {
    return {
      scheme: "",
      fixedSchemeUriString: uriString,
      fixupChangedProtocol: false,
    };
  }

  let scheme = matches[1].replace("\t", "").toLowerCase();
  let fixedSchemeUriString = uriString;

  if (isFixupSchemeTypos && hasSlash2) {
    const afterProtocol = uriString.substring(matches[0].length);
    fixedSchemeUriString = `${scheme}://${afterProtocol}`;
  }

  let fixupChangedProtocol = false;

  if (isFixupSchemeTypos) {
    fixupChangedProtocol = [
      ["ttp", "http"],
      ["htp", "http"],
      ["ttps", "https"],
      ["tps", "https"],
      ["ps", "https"],
      ["htps", "https"],
      ["ile", "file"],
      ["le", "file"],
    ].some(([typo, fixed]) => {
      if (scheme === typo) {
        scheme = fixed;
        fixedSchemeUriString =
          scheme + fixedSchemeUriString.substring(typo.length);
        return true;
      }
      return false;
    });
  }

  return {
    scheme,
    fixedSchemeUriString,
    fixupChangedProtocol,
  };
}

function fixupViewSource(uriString, fixupFlags) {
  let newFixupFlags = fixupFlags & ~FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP;
  let innerURIString = uriString.substring(12).trim();

  const { scheme: innerScheme } = extractScheme(innerURIString);
  if (innerScheme == "view-source") {
    throw new Components.Exception(
      "Prevent view-source recursion",
      Cr.NS_ERROR_FAILURE
    );
  }

  let info = Services.uriFixup.getFixupURIInfo(innerURIString, newFixupFlags);
  if (!info.preferredURI) {
    throw new Components.Exception(
      "Couldn't build a valid uri",
      Cr.NS_ERROR_MALFORMED_URI
    );
  }
  return {
    preferredURI: Services.io.newURI("view-source:" + info.preferredURI.spec),
    postData: info.postData,
  };
}

function fixupConsecutiveDotsHost(fixupInfo) {
  const uri = fixupInfo.fixedURI;

  try {
    if (!uri?.host.includes("..")) {
      return;
    }
  } catch (e) {
    return;
  }

  try {
    const isPreferredEqualsToFixed = fixupInfo.preferredURI?.equals(uri);

    fixupInfo.fixedURI = uri
      .mutate()
      .setHost(uri.host.replace(/\.+/g, "."))
      .finalize();

    if (isPreferredEqualsToFixed) {
      fixupInfo.preferredURI = fixupInfo.fixedURI;
    }
  } catch (e) {
    if (e.result !== Cr.NS_ERROR_MALFORMED_URI) {
      throw e;
    }
  }
}

function isURILike(uriString, host) {
  const indexOfSlash = uriString.indexOf("/");
  if (
    indexOfSlash >= 0 &&
    (indexOfSlash < uriString.indexOf("?", indexOfSlash) ||
      indexOfSlash < uriString.indexOf("#", indexOfSlash))
  ) {
    return true;
  }

  if (uriString.startsWith(host)) {
    uriString = uriString.substring(host.length);
  }

  return lazy.portRegex.test(uriString);
}

function maybeAddPrefixAndSuffix(oldHost) {
  let prefix = Services.prefs.getCharPref(
    "browser.fixup.alternate.prefix",
    "www."
  );
  let suffix = Services.locale.urlFixupSuffix;
  let newHost = "";
  let numDots = (oldHost.match(/\./g) || []).length;
  if (numDots == 0) {
    newHost = prefix + oldHost + suffix;
  } else if (numDots == 1 && !oldHost.startsWith(prefix)) {
    newHost = prefix + oldHost;
  }
  return newHost ? newHost : oldHost;
}

function updateHostAndScheme(info, newHost) {
  let oldHost = info.fixedURI.host;
  let oldScheme = info.fixedURI.scheme;
  try {
    info.fixedURI = info.fixedURI
      .mutate()
      .setScheme(lazy.alternateProtocol)
      .setHost(newHost)
      .finalize();
  } catch (ex) {
    if (ex.result != Cr.NS_ERROR_MALFORMED_URI) {
      throw ex;
    }
    return false;
  }
  if (oldScheme != info.fixedURI.scheme) {
    info.fixupChangedProtocol = true;
  }
  if (oldHost != info.fixedURI.host) {
    info.fixupCreatedAlternateURI = true;
  }
  return true;
}
