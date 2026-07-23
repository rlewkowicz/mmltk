/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */



export let searchParams = new URLSearchParams(
  document.documentURI.split("?")[1]
);
export const MDN_DOCS_HEADERS =
  "https://developer.mozilla.org/docs/Web/HTTP/Reference/Headers/";
export const COOP_MDN_DOCS = MDN_DOCS_HEADERS + "Cross-Origin-Opener-Policy";
export const COEP_MDN_DOCS = MDN_DOCS_HEADERS + "Cross-Origin-Embedder-Policy";
export const HTTPS_UPGRADES_MDN_DOCS =
  "https://support.mozilla.org/kb/https-upgrades";
export let gErrorCode = searchParams.get("e");
export let gIsCertError = gErrorCode == "nssBadCert";
export let gHasSts = gIsCertError && getCSSClass() === "badStsCert";
export let gNoConnectivity =
  gErrorCode == "dnsNotFound" && !RPMHasConnectivity();
export let gOffline = gErrorCode === "netOffline" || gNoConnectivity;
export const VPN_ACTIVE = RPMGetBoolPref(
  "browser.ipProtection.userEnabled",
  false
);

export function isCaptive() {
  return searchParams.get("captive") == "true";
}

export function getCSSClass() {
  return searchParams.get("s");
}

export function getHostName() {
  return RPMGetHostForDisplay(document);
}

export function getFilePath() {
  try {
    const url = new URL(document.location.href);
    if (url.protocol === "file:") {
      let path = decodeURIComponent(url.pathname);
      if (/^\/[A-Za-z]:/.test(path)) {
        path = path.substring(1);
      }
      return path;
    }
    return document.location.href;
  } catch (e) {}
  return null;
}

export function retryThis(buttonEl) {
  RPMSendAsyncMessage("Browser:EnableOnlineMode");
  buttonEl.disabled = true;
}

export async function getFailedCertificatesAsPEMString() {
  let locationUrl = document.location.href;
  let failedCertInfo = document.getFailedCertSecurityInfo();
  let errorMessage = failedCertInfo.errorMessage;
  let hasHSTS = failedCertInfo.hasHSTS.toString();
  let hasHPKP = failedCertInfo.hasHPKP.toString();
  let [hstsLabel, hpkpLabel, failedChainLabel] =
    await document.l10n.formatValues([
      { id: "cert-error-details-hsts-label", args: { hasHSTS } },
      { id: "cert-error-details-key-pinning-label", args: { hasHPKP } },
      { id: "cert-error-details-cert-chain-label" },
    ]);

  let certStrings = failedCertInfo.certChainStrings;
  let failedChainCertificates = "";
  for (let der64 of certStrings) {
    let wrapped = der64.replace(/(\S{64}(?!$))/g, "$1\r\n");
    failedChainCertificates +=
      "-----BEGIN CERTIFICATE-----\r\n" +
      wrapped +
      "\r\n-----END CERTIFICATE-----\r\n";
  }

  let details =
    locationUrl +
    "\r\n\r\n" +
    errorMessage +
    "\r\n\r\n" +
    hstsLabel +
    "\r\n" +
    hpkpLabel +
    "\r\n\r\n" +
    failedChainLabel +
    "\r\n\r\n" +
    failedChainCertificates;
  return details;
}

export async function getSubjectAltNames(failedCertInfo) {
  return [];
}

export async function recordSecurityUITelemetry(category, name, errorInfo) {
  let errorCode = errorInfo.errorCodeString.substring(0, 40);
  let extraKeys = {
    value: errorCode,
    is_frame: window.parent != window,
  };
  if (category == "securityUiCerterror") {
    extraKeys.has_sts = gHasSts;
  }
  if (name.startsWith("load")) {
    extraKeys.channel_status = errorInfo.channelStatus;
  }
  if (category == "securityUiCerterror" && name.startsWith("load")) {
    extraKeys.issued_by_cca = false;
    extraKeys.hyphen_compat = false;

    const asciiHostname = RPMGetInnermostAsciiHost();

    let label = asciiHostname.substring(0, asciiHostname.indexOf("."));
    if (
      errorCode == "SSL_ERROR_BAD_CERT_DOMAIN" &&
      (label.startsWith("-") || label.endsWith("-"))
    ) {
      try {
        let subjectAltNames = await getSubjectAltNames(errorInfo);
        for (let subjectAltName of subjectAltNames) {
          if (
            subjectAltName.startsWith("*.") &&
            subjectAltName.substring(1) == asciiHostname.substring(label.length)
          ) {
            extraKeys.hyphen_compat = true;
            break;
          }
        }
      } catch (e) {
        console.error("error parsing certificate:", e);
      }
    }
  }
  if (category == "securityUiNeterror" && name == "loadAboutneterror") {
    extraKeys.no_connectivity = gNoConnectivity;
    extraKeys.trr_only = gErrorCode == "dnsNotFound" && RPMIsTRROnlyFailure();
    extraKeys.captive_portal_state =
      searchParams.get("captivePortalState") ?? "";
  }
  RPMRecordGleanEvent(category, name, extraKeys);
}

export function errorHasNoUserFix(errorCodeString) {
  switch (errorCodeString) {
    case "MOZILLA_PKIX_ERROR_INSUFFICIENT_CERTIFICATE_TRANSPARENCY":
    case "MOZILLA_PKIX_ERROR_INVALID_INTEGER_ENCODING":
    case "MOZILLA_PKIX_ERROR_ISSUER_NO_LONGER_TRUSTED":
    case "MOZILLA_PKIX_ERROR_KEY_PINNING_FAILURE":
    case "MOZILLA_PKIX_ERROR_SIGNATURE_ALGORITHM_MISMATCH":
    case "SEC_ERROR_BAD_DER":
    case "SEC_ERROR_BAD_SIGNATURE":
    case "SEC_ERROR_CERT_NOT_IN_NAME_SPACE":
    case "SEC_ERROR_EXTENSION_VALUE_INVALID":
    case "SEC_ERROR_INADEQUATE_CERT_TYPE":
    case "SEC_ERROR_INADEQUATE_KEY_USAGE":
    case "SEC_ERROR_INVALID_KEY":
    case "SEC_ERROR_PATH_LEN_CONSTRAINT_INVALID":
    case "SEC_ERROR_REVOKED_CERTIFICATE":
    case "SEC_ERROR_UNKNOWN_CRITICAL_EXTENSION":
    case "SEC_ERROR_UNSUPPORTED_EC_POINT_FORM":
    case "SEC_ERROR_UNSUPPORTED_ELLIPTIC_CURVE":
    case "SEC_ERROR_UNSUPPORTED_KEYALG":
    case "SEC_ERROR_UNTRUSTED_CERT":
    case "SEC_ERROR_UNTRUSTED_ISSUER":
      return true;
    default:
      return false;
  }
}

export function detectClockSkew(failedCertInfo, now = Date.now()) {
  const ONE_DAY_SECONDS = 60 * 60 * 24;
  const FIVE_DAYS_MS = 5 * ONE_DAY_SECONDS * 1000;

  const certNotBefore = failedCertInfo.certValidityRangeNotBefore;
  const certNotAfter = failedCertInfo.certValidityRangeNotAfter;
  if (certNotBefore == null || certNotAfter == null) {
    return false;
  }

  const difference = RPMGetIntPref("services.settings.clock_skew_seconds", 0);
  const lastFetched =
    RPMGetIntPref("services.settings.last_update_seconds", 0) * 1000;

  const approximateDate = now - difference * 1000;

  if (
    Math.abs(difference) > ONE_DAY_SECONDS &&
    now - lastFetched <= FIVE_DAYS_MS &&
    certNotBefore < approximateDate &&
    certNotAfter > approximateDate
  ) {
    return true;
  }

  const appBuildID = RPMGetAppBuildID();
  const year = parseInt(appBuildID.substring(0, 4), 10);
  const month = parseInt(appBuildID.substring(4, 6), 10) - 1;
  const day = parseInt(appBuildID.substring(6, 8), 10);
  const buildDate = new Date(year, month, day).getTime();

  return buildDate > now && certNotAfter > buildDate;
}

export function handleNSSFailure(callback) {
  const netErrorInfo = document.getNetErrorInfo();
  void recordSecurityUITelemetry(
    "securityUiTlserror",
    "loadAbouttlserror",
    netErrorInfo
  );
  const errorCode = netErrorInfo.errorCodeString;
  const result = {};
  switch (errorCode) {
    case "SSL_ERROR_UNSUPPORTED_VERSION":
    case "SSL_ERROR_PROTOCOL_VERSION_ALERT": {
      result.versionError = true;
    }
    // fallthrough

    case "SSL_ERROR_NO_CIPHERS_SUPPORTED":
    case "SSL_ERROR_NO_CYPHER_OVERLAP":
    case "SSL_ERROR_SSL_DISABLED":
      RPMAddMessageListener("HasChangedCertPrefs", msg => {
        if (msg.data.hasChangedCertPrefs) {
          callback?.();
        }
      });
      RPMSendAsyncMessage("GetChangedCertPrefs");
  }
  return result;
}
