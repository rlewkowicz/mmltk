/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

if (Services.appinfo.processType !== Services.appinfo.PROCESS_TYPE_DEFAULT) {
  throw new Error("This code is assumed to run in the parent process.");
}

const mockable = {
  getAvailableLocalesIncludingFallback() {
    return Services.locale.availableLocales;
  },
  getAppLocaleAsBCP47() {
    return Services.locale.appLocaleAsBCP47;
  },
  getSystemLocale() {
    const osPrefs = Cc["@mozilla.org/intl/ospreferences;1"].getService(
      Ci.mozIOSPreferences
    );
    return osPrefs.systemLocale;
  },
  setRequestedAppLocales(locales) {
    Services.locale.requestedLocales = locales;
  },
};

function setRequestedAppLocales(locales) {
  mockable.setRequestedAppLocales(locales);
}


function getStructuredLocaleOrNull(localeString) {
  try {
    const locale = new Services.intl.Locale(localeString);
    return {
      baseName: locale.baseName,
      language: locale.language,
      region: locale.region,
    };
  } catch (_err) {
    return null;
  }
}

function getAppAndSystemLocaleInfo() {
  const systemLocaleRaw = mockable.getSystemLocale();
  const appLocaleRaw = mockable.getAppLocaleAsBCP47();

  const systemLocale = getStructuredLocaleOrNull(systemLocaleRaw);
  const appLocale = getStructuredLocaleOrNull(appLocaleRaw);

  let matchType = "unknown";
  if (systemLocale && appLocale) {
    if (systemLocale.language !== appLocale.language) {
      matchType = "language-mismatch";
    } else if (systemLocale.region !== appLocale.region) {
      matchType = "region-mismatch";
    } else {
      matchType = "match";
    }
  }

  let canLiveReload = null;
  if (systemLocale && appLocale) {
    const systemDirection = Services.intl.getScriptDirection(
      systemLocale.language
    );
    const appDirection = Services.intl.getScriptDirection(appLocale.language);
    const supportsBidiSwitching = Services.prefs.getBoolPref(
      "intl.multilingual.liveReloadBidirectional",
      false
    );
    canLiveReload = systemDirection === appDirection || supportsBidiSwitching;
  }
  return {
    systemLocaleRaw,
    systemLocale,
    appLocaleRaw,
    appLocale,
    matchType,
    canLiveReload,

    displayNames: {
      systemLanguage: systemLocale
        ? Services.intl.getLocaleDisplayNames(
            undefined,
            [systemLocale.baseName],
            { preferNative: true }
          )[0]
        : null,
      appLanguage: appLocale
        ? Services.intl.getLocaleDisplayNames(undefined, [appLocale.baseName], {
            preferNative: true,
          })[0]
        : null,
    },
  };
}

async function getAvailableLocales() {
  return mockable.getAvailableLocalesIncludingFallback();
}

export var LangPackMatcher = {
  getAppAndSystemLocaleInfo,
  setRequestedAppLocales,
  getAvailableLocales,
  mockable,
};
