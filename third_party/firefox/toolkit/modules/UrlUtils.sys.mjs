/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


export const UrlUtils = {
  REGEXP_SPACES: /\s+/,
  REGEXP_SPACES_START: /^\s+/,

  REGEXP_LIKE_PROTOCOL: /^[A-Z+.-]+:\/*(?!\/)/i,
  REGEXP_USERINFO_INVALID_CHARS: /[^\w.~%!$&'()*+,;=:-]/,
  REGEXP_HOSTPORT_INVALID_CHARS: /[^\[\]A-Z0-9.:-]/i,
  REGEXP_HOSTPORT_INVALID_TLD_NUM: /\.\w*\d\w*(:\d+)?$/,
  REGEXP_SINGLE_WORD_HOST: /^[^.:]+$/i,
  REGEXP_HOSTPORT_IP_LIKE: /^(?=(.*[.:].*){2})[a-f0-9\.\[\]:]+$/i,
  REGEXP_HOSTPORT_INVALID_IP:
    /\.{2,}|\d{5,}|\d{4,}(?![:\]])|^\.|^(\d+\.){4,}\d+$|^\d{4,}$/,
  REGEXP_HOSTPORT_IPV4: /^(\d{1,3}\.){3,}\d{1,3}(:\d+)?$/,
  REGEXP_HOSTPORT_IPV6: /^\[([0-9a-f]{0,4}:){0,7}[0-9a-f]{0,4}\]?$/i,
  REGEXP_COMMON_EMAIL: /^[\w!#$%&'*+/=?^`{|}~.-]+@[\[\]A-Z0-9.-]+$/i,
  REGEXP_HAS_PORT: /:\d+$/,
  REGEXP_PERCENT_ENCODED_START: /^(%[0-9a-f]{2}){2,}/i,
  REGEXP_PREFIX: /^[a-z-]+:(?:\/){0,2}/i,

  looksLikeUrl(
    token,
    { requirePath = false, validateOrigin = false } = {},
    logger
  ) {
    if (token.length < 2) {
      return false;
    }
    if (token.startsWith("data:")) {
      return token.length > 5;
    }
    if (this.REGEXP_SPACES.test(token)) {
      return false;
    }
    if (this.REGEXP_LIKE_PROTOCOL.test(token)) {
      return true;
    }
    let slashIndex = token.indexOf("/");
    let prePath = slashIndex != -1 ? token.slice(0, slashIndex) : token;
    if (!this.looksLikeOrigin(prePath, { ignoreKnownDomains: true })) {
      return false;
    }

    if (validateOrigin) {
      const result = this.looksLikeOrigin(prePath, {
        ignoreKnownDomains: false,
      });
      if (result !== this.LOOKS_LIKE_ORIGIN.NONE) {
        return true;
      }
      return false;
    }

    let path = slashIndex != -1 ? token.slice(slashIndex) : "";
    logger?.debug("path", path);
    if (requirePath && !path) {
      return false;
    }
    let atIndex = prePath.indexOf("@");
    let userinfo = atIndex != -1 ? prePath.slice(0, atIndex) : "";
    if (path.length && userinfo.length) {
      return true;
    }

    if (/^\/[a-z]/i.test(path)) {
      return true;
    }
    if (["%", "?", "#"].some(c => path.includes(c))) {
      return true;
    }

    let hostPort = atIndex != -1 ? prePath.slice(atIndex + 1) : prePath;
    if (this.REGEXP_HOSTPORT_IPV4.test(hostPort)) {
      return true;
    }
    if (
      this.REGEXP_HOSTPORT_IPV6.test(hostPort) &&
      ["[", "]", ":"].some(c => hostPort.includes(c))
    ) {
      return true;
    }
    if (Services.uriFixup.isDomainKnown(hostPort)) {
      return true;
    }
    return false;
  },

  looksLikeOrigin(
    token,
    {
      ignoreKnownDomains = false,
      noIp = false,
      noPort = false,
      allowPartialNumericalTLDs = false,
    } = {},
    logger
  ) {
    if (!token.length) {
      return this.LOOKS_LIKE_ORIGIN.NONE;
    }
    let atIndex = token.indexOf("@");
    if (atIndex != -1 && this.REGEXP_COMMON_EMAIL.test(token)) {
      return this.LOOKS_LIKE_ORIGIN.NONE;
    }

    let userinfo = atIndex != -1 ? token.slice(0, atIndex) : "";
    let hostPort = atIndex != -1 ? token.slice(atIndex + 1) : token;
    let hasPort = this.REGEXP_HAS_PORT.test(hostPort);
    logger?.debug("userinfo", userinfo);
    logger?.debug("hostPort", hostPort);
    if (noPort && hasPort) {
      return this.LOOKS_LIKE_ORIGIN.NONE;
    }

    if (
      this.REGEXP_HOSTPORT_IPV4.test(hostPort) ||
      this.REGEXP_HOSTPORT_IPV6.test(hostPort)
    ) {
      return noIp ? this.LOOKS_LIKE_ORIGIN.NONE : this.LOOKS_LIKE_ORIGIN.IP;
    }

    if (
      this.REGEXP_LIKE_PROTOCOL.test(hostPort) ||
      this.REGEXP_USERINFO_INVALID_CHARS.test(userinfo) ||
      this.REGEXP_HOSTPORT_INVALID_CHARS.test(hostPort) ||
      (!allowPartialNumericalTLDs &&
        this.REGEXP_HOSTPORT_INVALID_TLD_NUM.test(hostPort)) ||
      (!this.REGEXP_SINGLE_WORD_HOST.test(hostPort) &&
        this.REGEXP_HOSTPORT_IP_LIKE.test(hostPort) &&
        this.REGEXP_HOSTPORT_INVALID_IP.test(hostPort))
    ) {
      return this.LOOKS_LIKE_ORIGIN.NONE;
    }

    if (
      !ignoreKnownDomains &&
      !userinfo &&
      !hasPort &&
      this.REGEXP_SINGLE_WORD_HOST.test(hostPort)
    ) {
      return Services.uriFixup.isDomainKnown(hostPort)
        ? this.LOOKS_LIKE_ORIGIN.KNOWN_DOMAIN
        : this.LOOKS_LIKE_ORIGIN.NONE;
    }

    if (atIndex != -1 || hasPort) {
      return this.LOOKS_LIKE_ORIGIN.USERINFO_OR_PORT;
    }

    return this.LOOKS_LIKE_ORIGIN.OTHER;
  },

  LOOKS_LIKE_ORIGIN: Object.freeze({
    NONE: 0,
    OTHER: 1,
    IP: 2,
    KNOWN_DOMAIN: 3,
    USERINFO_OR_PORT: 4,
  }),
};
