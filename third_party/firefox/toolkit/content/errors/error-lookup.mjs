/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import {
  getErrorConfig,
  isErrorSupported,
} from "chrome://global/content/errors/error-registry.mjs";

export function resolveErrorID({
  errorCodeString,
  gErrorCode,
  noConnectivity,
  vpnActive,
}) {
  if (noConnectivity) {
    return "NS_ERROR_OFFLINE";
  }
  if (gErrorCode === "proxyConnectFailure" && vpnActive) {
    return "vpnFailure";
  }

  if (errorCodeString && isErrorSupported(errorCodeString)) {
    return errorCodeString;
  }

  if (errorCodeString) {
    if (gErrorCode !== "nssFailure2" || !isErrorSupported("nssFailure2")) {
      return null;
    }
  }

  if (gErrorCode && isErrorSupported(gErrorCode)) {
    return gErrorCode;
  }

  return null;
}

export function errorHasNoUserFix(id) {
  const config = getErrorConfig(id);
  return config ? config.hasNoUserFix === true : false;
}

export function isFeltPrivacySupported(id) {
  const config = getErrorConfig(id);
  return !!config;
}

export function resolveL10nArgs(l10nConfig, l10nArgValues) {
  if (!l10nConfig?.dataL10nArgs) {
    return l10nConfig;
  }

  const values = {
    hostname: l10nArgValues.hostname,
    path: l10nArgValues.filePath,
    date: Date.now(),
    now: l10nArgValues.now ?? Date.now(),
    errorMessage: l10nArgValues.errorInfo?.errorMessage ?? "",
    errorCodeString: l10nArgValues.errorInfo?.errorCodeString ?? "",
    validHosts: l10nArgValues.domainMismatchNames ?? "",
    mitm: l10nArgValues.mitmName ?? "",
    responsestatus: l10nArgValues.errorInfo?.responseStatus ?? 0,
    responsestatustext: l10nArgValues.errorInfo?.responseStatusText ?? "",
  };

  if (typeof l10nConfig.dataL10nId === "function") {
    l10nConfig.dataL10nId = l10nConfig.dataL10nId(l10nArgValues);
  }

  for (const [key, value] of Object.entries(l10nConfig.dataL10nArgs)) {
    if (value === null || value === "") {
      l10nConfig.dataL10nArgs[key] = values[key];
    } else if (typeof value === "function") {
      l10nConfig.dataL10nArgs[key] = value(l10nArgValues);
    }
  }
  return l10nConfig;
}

export function resolveManyL10nArgs(l10nConfigs, l10nArgValues) {
  if (!l10nConfigs) {
    return null;
  }
  for (let i = 0; i < l10nConfigs.length; i++) {
    l10nConfigs[i] = resolveL10nArgs(l10nConfigs[i], l10nArgValues);
  }
  return l10nConfigs;
}

export function resolveDescriptionParts(descriptionParts, l10nArgValues) {
  if (!descriptionParts) {
    return [];
  }

  if (typeof descriptionParts === "function") {
    return descriptionParts(l10nArgValues);
  }

  return descriptionParts.map(part => resolveL10nArgs(part, l10nArgValues));
}

export function resolveAdvancedConfig(advancedConfig, l10nArgValues) {
  if (!advancedConfig) {
    return null;
  }

  const resolved = { ...advancedConfig };
  ["whyDangerous", "whatCanYouDo", "learnMore"].forEach(key => {
    if (resolved[key]) {
      resolved[key] = resolveL10nArgs(advancedConfig[key], l10nArgValues);
    }
  });
  return resolved;
}

function resolveCustomNetError(customNetError, l10nArgValues) {
  if (!customNetError) {
    return customNetError;
  }
  const resolved = { ...customNetError };
  if (typeof resolved.whatCanYouDoItems === "function") {
    resolved.whatCanYouDoItems = resolved.whatCanYouDoItems(l10nArgValues);
  }
  if (customNetError.whatCanYouDoL10nArgs) {
    const argsClone = {
      dataL10nArgs: { ...customNetError.whatCanYouDoL10nArgs },
    };
    resolveL10nArgs(argsClone, l10nArgValues);
    resolved.whatCanYouDoL10nArgs = argsClone.dataL10nArgs;
  }
  return resolved;
}

export function getResolvedErrorConfig(id, l10nArgValues) {
  const baseConfig = getErrorConfig(id);
  if (!baseConfig) {
    return {};
  }

  const introContentHandler = Array.isArray(baseConfig.introContent)
    ? resolveManyL10nArgs
    : resolveL10nArgs;
  return {
    ...baseConfig,
    introContent: introContentHandler(baseConfig.introContent, l10nArgValues),
    shortDescription: resolveL10nArgs(
      baseConfig.shortDescription,
      l10nArgValues
    ),
    descriptionParts: resolveDescriptionParts(
      baseConfig.descriptionParts,
      l10nArgValues
    ),
    advanced: resolveAdvancedConfig(baseConfig.advanced, l10nArgValues),
    customNetError: resolveCustomNetError(
      baseConfig.customNetError,
      l10nArgValues
    ),
  };
}
