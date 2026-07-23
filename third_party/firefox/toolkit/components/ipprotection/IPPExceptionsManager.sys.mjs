/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  IPProtectionService:
    "moz-src:///toolkit/components/ipprotection/IPProtectionService.sys.mjs",
});

const PERM_NAME = "ipp-vpn";

const INCLUSION_PREF = "browser.ipProtection.inclusion.match_patterns";

const MATCH_PATTERN_OPTIONS = {
  ignorePath: true,
  restrictSchemes: false,
};

const DEFAULT_EXCLUDED_URL_PREFS = [
  "browser.ipProtection.guardian.endpoint",
  "captivedetect.canonicalURL",
];

function urlToHostPattern(url) {
  try {
    const uri = Services.io.newURI(url);
    return `${uri.scheme}://${uri.host}/*`;
  } catch (_) {
    return null;
  }
}

export const IPPPrincipalRules = Object.freeze({
  INCLUDED: "included",
  EXCLUDED: "excluded",
  DEFAULT: "default",
});

class ExceptionsManager extends EventTarget {
  #inited = false;
  #observer = null;
  #inclusionPrefObserver = null;
  #inclusionSet = new MatchPatternSet([], MATCH_PATTERN_OPTIONS);
  #excludedOrigins = new MatchPatternSet([], MATCH_PATTERN_OPTIONS);
  #excludedPrefObserver = null;
  #observedExcludedPrefs = [];

  init() {
    if (this.#inited) {
      return;
    }

    this.#observer = (subject, topic, data) => {
      this.observe(subject, topic, data);
    };
    Services.obs.addObserver(this.#observer, "perm-changed");

    this.#inclusionSet = ExceptionsManager.getInclusionList();
    this.#inclusionPrefObserver = () => {
      this.#inclusionSet = ExceptionsManager.getInclusionList();
    };
    Services.prefs.addObserver(INCLUSION_PREF, this.#inclusionPrefObserver);

    this.#observedExcludedPrefs = [
      ...DEFAULT_EXCLUDED_URL_PREFS,
      ...(lazy.IPProtectionService.authProvider?.excludedUrlPrefs ?? []),
    ];
    this.#excludedPrefObserver = () => this.#rebuildExcludedOrigins();
    for (const pref of this.#observedExcludedPrefs) {
      Services.prefs.addObserver(pref, this.#excludedPrefObserver);
    }
    this.#rebuildExcludedOrigins();

    this.#inited = true;
  }

  uninit() {
    if (!this.#inited) {
      return;
    }

    Services.obs.removeObserver(this.#observer, "perm-changed");
    this.#observer = null;

    if (this.#inclusionPrefObserver) {
      Services.prefs.removeObserver(
        INCLUSION_PREF,
        this.#inclusionPrefObserver
      );
      this.#inclusionPrefObserver = null;
    }

    if (this.#excludedPrefObserver) {
      for (const pref of this.#observedExcludedPrefs) {
        Services.prefs.removeObserver(pref, this.#excludedPrefObserver);
      }
      this.#excludedPrefObserver = null;
      this.#observedExcludedPrefs = [];
    }

    this.#inited = false;
  }

  #rebuildExcludedOrigins() {
    const patterns = [];
    for (const pref of this.#observedExcludedPrefs) {
      const pattern = urlToHostPattern(Services.prefs.getStringPref(pref, ""));
      if (pattern) {
        patterns.push(pattern);
      }
    }
    this.#excludedOrigins = new MatchPatternSet(
      patterns,
      MATCH_PATTERN_OPTIONS
    );
  }

  observe(subject, topic, data) {
    if (topic !== "perm-changed") {
      return;
    }

    let permission = subject.QueryInterface(Ci.nsIPermission);
    if (permission.type !== PERM_NAME) {
      return;
    }

    const isExclusion =
      permission.capability === Ci.nsIPermissionManager.DENY_ACTION;
    const added = data === "added" && isExclusion;
    const removed = data === "deleted" && isExclusion;

    if (added || removed) {
      if (added) {
      }

      this.dispatchEvent(
        new CustomEvent("IPPExceptionsManager:ExclusionChanged")
      );
    }
  }

  addExclusion(principal) {
    Services.perms.addFromPrincipal(
      principal,
      PERM_NAME,
      Ci.nsIPermissionManager.DENY_ACTION
    );
  }

  removeExclusion(principal) {
    Services.perms.removeFromPrincipal(principal, PERM_NAME);
  }

  hasExclusion(principal) {
    let permission = this.getExceptionPermissionObject(principal);
    return permission?.capability === Ci.nsIPermissionManager.DENY_ACTION;
  }

  getExceptionPermissionObject(principal) {
    let permissionObject = Services.perms.getPermissionObject(
      principal,
      PERM_NAME,
      true 
    );
    return permissionObject;
  }

  getExclusionCount() {
    let count = 0;
    for (let perm of Services.perms.getAllByTypes([PERM_NAME])) {
      if (perm.capability === Ci.nsIPermissionManager.DENY_ACTION) {
        count++;
      }
    }
    return count;
  }

  setExclusion(principal, shouldExclude) {
    if (!principal) {
      return;
    }

    const isExclusion = this.hasExclusion(principal);

    if ((shouldExclude && isExclusion) || (!shouldExclude && !isExclusion)) {
      return;
    }

    if (shouldExclude) {
      this.addExclusion(principal);
    } else {
      this.removeExclusion(principal);
    }
  }

  static getInclusionList() {
    let raw = Services.prefs.getStringPref(INCLUSION_PREF, "[]");
    let arr = JSON.parse(raw);
    if (!Array.isArray(arr)) {
      throw new TypeError(`${INCLUSION_PREF} does not contain a JSON array`);
    }
    let patterns = arr.filter(s => typeof s === "string" && s.length);
    return new MatchPatternSet(patterns, MATCH_PATTERN_OPTIONS);
  }

  static isLocal(principal) {
    return principal.isLoopbackHost || principal.isLocalIpAddress;
  }

  getPrincipalRule(principal) {
    if (!this.#inited) {
      this.init();
    }
    try {
      const uri = principal?.URI;
      if (uri && this.#inclusionSet.matches(uri)) {
        return IPPPrincipalRules.INCLUDED;
      }
      if (
        !principal?.isNullPrincipal &&
        !principal?.schemeIs("http") &&
        !principal?.schemeIs("https")
      ) {
        return IPPPrincipalRules.EXCLUDED;
      }
      if (ExceptionsManager.isLocal(principal)) {
        return IPPPrincipalRules.EXCLUDED;
      }
      if (this.hasExclusion(principal)) {
        return IPPPrincipalRules.EXCLUDED;
      }
      if (uri && this.#excludedOrigins.matches(uri)) {
        return IPPPrincipalRules.EXCLUDED;
      }
      return IPPPrincipalRules.DEFAULT;
    } catch (_) {
      return IPPPrincipalRules.EXCLUDED;
    }
  }

  canManage(principal) {
    if (!principal) {
      return false;
    }
    return (
      !principal.schemeIs("about") &&
      !principal.schemeIs("resource") &&
      !principal.isSystemPrincipal &&
      !ExceptionsManager.isLocal(principal)
    );
  }
}

const IPPExceptionsManager = new ExceptionsManager();
export { IPPExceptionsManager };
