/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import { Preferences } from "chrome://global/content/preferences/Preferences.mjs";
import { SettingGroupManager } from "chrome://browser/content/preferences/config/SettingGroupManager.mjs";

const {
  LangPackMatcher, 
  TAB_SESSION_ID, 
} = window;

Preferences.addAll([
  { id: "intl.multilingual.enabled", type: "bool" },
  { id: "intl.multilingual.downloadEnabled", type: "bool" },
  { id: "intl.regional_prefs.use_os_locales", type: "bool" },
  { id: "intl.accept_languages", type: "string" },
  { id: "privacy.spoof_english", type: "int" },
]);


export const Multilingual = {
  TransitionType: Object.freeze({
    LocalesMatch: "LocalesMatch",
    RestartRequired: "RestartRequired",
    LiveReload: "LiveReload",
  }),

  async installedLocales() {
    return this.localizeArray(
       (await LangPackMatcher.getAvailableLocales()),
      code => code,
      (code, label) => ({ code, label })
    );
  },

  localizeArray(list, getCode, transform) {
    const localeNames = this.getLocaleDisplayNames(list.map(getCode));
    return list
      .map(
        (data, i) =>
           ([localeNames[i], transform(data, localeNames[i])])
      )
      .sort((a, b) => a[0].localeCompare(b[0]))
      .map(d => d[1]);
  },

  getLocaleDisplayNames(codes) {
    return Services.intl.getLocaleDisplayNames(undefined, codes, {
      preferNative: true,
    });
  },

  getTransitionType(newLocales) {
    const { appLocalesAsBCP47 } = Services.locale;
    if (appLocalesAsBCP47.join(",") === newLocales.join(",")) {
      return Multilingual.TransitionType.LocalesMatch;
    }

    if (Services.prefs.getBoolPref("intl.multilingual.liveReload")) {
      if (
        Services.intl.getScriptDirection(newLocales[0]) !==
          Services.intl.getScriptDirection(appLocalesAsBCP47[0]) &&
        !Services.prefs.getBoolPref("intl.multilingual.liveReloadBidirectional")
      ) {
        return Multilingual.TransitionType.RestartRequired;
      }

      return Multilingual.TransitionType.LiveReload;
    }

    return Multilingual.TransitionType.RestartRequired;
  },

  recordTelemetry(method, value = null) {
    if (method == "apply") {
    } else if (method == "reorder") {
    } else if (method == "add") {
    } else if (method == "search") {
    } else if (method == "manage") {
    }
  },

  applyAndRestart(locales) {
    Services.locale.requestedLocales = locales;

    this.recordTelemetry("apply");

    let cancelQuit = Cc["@mozilla.org/supports-PRBool;1"].createInstance(
      Ci.nsISupportsPRBool
    );
    Services.obs.notifyObservers(
      cancelQuit,
      "quit-application-requested",
      "restart"
    );
    if (!cancelQuit.data) {
      Services.startup.quit(
        Services.startup.eAttemptQuit | Services.startup.eRestart
      );
    }
  },

};

Preferences.addSetting({
  id: "multilingualEnabled",
  pref: "intl.multilingual.enabled",
});
Preferences.addSetting({
  id: "multilingualDownloadEnabled",
  pref: "intl.multilingual.downloadEnabled",
});

function makeBrowserLanguageOption({ code, label }) {
  return {
    value: code,
    controlAttrs: {
      label,
    },
  };
}
class BrowserLanguagesSetting extends Preferences.AsyncSetting {
  static id = "browserLanguages";

  multilingualEnabled = Preferences.getSetting("multilingualEnabled");

  installedLocales = Promise.resolve([]);

  #pendingLocales = null;
  #installing = false;
  #installError = false;

  get currentLocale() {
    return Services.locale.appLocaleAsBCP47;
  }

  get pendingLocale() {
    return this.#pendingLocales?.[0] ?? null;
  }

  get restartRequired() {
    return Boolean(this.#pendingLocales?.length);
  }

  get installing() {
    return this.#installing;
  }

  get installError() {
    return this.#installError;
  }

  #updateLocales(newLocales) {
    switch (Multilingual.getTransitionType(newLocales)) {
      case Multilingual.TransitionType.RestartRequired:
        this.#pendingLocales = newLocales;
        break;
      case Multilingual.TransitionType.LiveReload:
        this.#pendingLocales = null;
        Services.locale.requestedLocales = newLocales;
        break;
      case Multilingual.TransitionType.LocalesMatch:
        this.#pendingLocales = null;
        break;
      default:
        throw new Error("Unhandled transition type.");
    }
    this.emitChange();
  }

  async #ensureLocaleInstalled(code) {
    return (await this.installedLocales).find(
      locale => locale.code == code
    );
  }

  setup() {
    Services.obs.addObserver(this.emitChange, "intl:app-locales-changed");
    this.multilingualEnabled.on("change", this.emitChange);
    return () => {
      Services.obs.removeObserver(this.emitChange, "intl:app-locales-changed");
      this.multilingualEnabled.off("change", this.emitChange);
    };
  }

  beforeRefresh() {
    this.installedLocales = Multilingual.installedLocales();
  }

  // @ts-expect-error Arrays aren't typed currently.
  async get() {
    return this.#pendingLocales
      ? [...this.#pendingLocales]
      : [...Services.locale.appLocalesAsBCP47];
  }

  async visible() {
    return Boolean(this.multilingualEnabled.value);
  }

  async getPreferred() {
    return this.pendingLocale ?? this.currentLocale;
  }

  async setPreferred(code) {
    this.#installError = false;
    if (code == this.currentLocale) {
      this.#pendingLocales = null;
      this.emitChange();
      return;
    }
    let locale = await this.#ensureLocaleInstalled(code);
    if (!locale) {
      this.#installError = true;
      this.emitChange();
      return;
    }
    Multilingual.recordTelemetry("reorder");
    let newLocales = Array.from(
      new Set([code, ...Services.locale.requestedLocales]).values()
    );
    this.#updateLocales(newLocales);
  }

  async getFallback() {
    let locales = await this.get();
    if (locales[1]) {
      return locales[1];
    }
    return null;
  }

  async setFallback(code) {
    let locales = await this.get();
    Multilingual.recordTelemetry("reorder");
    this.#updateLocales([locales[0], code]);
  }

  applyAndRestart() {
    if (this.restartRequired) {
      Multilingual.applyAndRestart(this.#pendingLocales);
    }
  }
}
Preferences.addSetting(BrowserLanguagesSetting);

class BrowserLanguageRemoteLocalesSetting extends Preferences.AsyncSetting {
  static id = "browserLanguageRemoteLocales";

  #multilingualDownloadEnabled =  (
    Preferences.getSetting("multilingualDownloadEnabled")
  );

  #cache = null;

  #languagesLoaded = false;

  setup() {
    this.#multilingualDownloadEnabled.on("change", this.emitChange);
    const onPaneshown = e => {
      if (e.detail.category == "paneLanguages") {
        this.#languagesLoaded = true;
        this.emitChange();
        window.removeEventListener("paneshown", onPaneshown);
      }
    };
    window.addEventListener("paneshown", onPaneshown);
    return () =>
      this.#multilingualDownloadEnabled.off("change", this.emitChange);
  }

  // @ts-expect-error Arrays aren't typed currently.
  defaultValue = [];

  // @ts-expect-error Arrays aren't typed currently.
  async get() {
    if (!this.#multilingualDownloadEnabled.value || !this.#languagesLoaded) {
      return [];
    }
    if (!this.#cache) {
      this.#cache = this.#fetch();
    }
    return this.#cache;
  }

  async #fetch() {
    return [];
  }
}
Preferences.addSetting(BrowserLanguageRemoteLocalesSetting);

class BrowserLanguagePreferredSetting extends Preferences.AsyncSetting {
  static id = "browserLanguagePreferred";

  #browserLanguagesSetting =  (
    Preferences.getSetting("browserLanguages")
  );
  #remoteLocalesSetting =  (
    Preferences.getSetting("browserLanguageRemoteLocales")
  );

  get #browserLanguages() {
    return  (
       (
         (
          this.#browserLanguagesSetting.config
        ).asyncSetting
      )
    );
  }

  setup() {
    this.#browserLanguagesSetting.on("change", this.emitChange);
    this.#remoteLocalesSetting.on("change", this.emitChange);
    return () => {
      this.#browserLanguagesSetting.off("change", this.emitChange);
      this.#remoteLocalesSetting.off("change", this.emitChange);
    };
  }

  get #remoteLocales() {
    return (
       (
         (this.#remoteLocalesSetting.value)
      ) || []
    );
  }

  async get() {
    return this.#browserLanguages.getPreferred();
  }

  async set(code) {
    return this.#browserLanguages.setPreferred(code, this.#remoteLocales);
  }

  async disabled() {
    return this.#browserLanguages.installing;
  }

  async visible() {
    return this.#browserLanguagesSetting.visible;
  }

  async getControlConfig() {
    let installed = await this.#browserLanguages.installedLocales;
    let remote = this.#remoteLocales.filter(
      r => !installed.some(i => i.code == r.code)
    );
    return {
      options: [
        ...installed.map(makeBrowserLanguageOption),
        ...(remote.length ? [{ control: "hr" }] : []),
        ...remote.map(makeBrowserLanguageOption),
      ],
    };
  }
}
Preferences.addSetting(BrowserLanguagePreferredSetting);

class BrowserLanguageFallbackSetting extends Preferences.AsyncSetting {
  static id = "browserLanguageFallback";

  #browserLanguages = Preferences.getSetting("browserLanguages");

  get #languages() {
    return  (
       (
         (this.#browserLanguages.config)
          .asyncSetting
      )
    );
  }

  setup() {
    this.#browserLanguages.on("change", this.emitChange);
    return () => this.#browserLanguages.off("change", this.emitChange);
  }

  async get() {
    return this.#languages.getFallback();
  }

  async set(code) {
    return this.#languages.setFallback(code);
  }

  async disabled() {
    return this.#languages.installing;
  }

  async visible() {
    if (!this.#browserLanguages.visible) {
      return false;
    }
    if (
      (await this.#languages.getPreferred()) == Services.locale.defaultLocale
    ) {
      return false;
    }
    let installed = await this.#languages.installedLocales;
    return installed.length >= 2;
  }

  async getControlConfig() {
    let installed = await this.#languages.installedLocales;
    let locales = await this.#languages.get();
    let options = installed.map(locale => ({
      ...makeBrowserLanguageOption(locale),
      hidden: locale.code === locales[0],
    }));
    return { options };
  }
}
Preferences.addSetting(BrowserLanguageFallbackSetting);

Preferences.addSetting({
  id: "browserLanguageMessage",
  deps: ["browserLanguages"],
  visible(deps) {
    let handler =  (
      deps.browserLanguages.config
    );
    let setting =  (
       (handler.asyncSetting)
    );
    return setting.restartRequired || setting.installError;
  },
});

Preferences.addSetting({
  id: "useSystemLocale",
  pref: "intl.regional_prefs.use_os_locales",
  visible() {
    let appLocale = Services.locale.appLocaleAsBCP47;
    let regionalPrefsLocales = Services.locale.regionalPrefsLocales;
    if (!regionalPrefsLocales.length) {
      return false;
    }
    let systemLocale = regionalPrefsLocales[0];
    return appLocale.split("-u-")[0] != systemLocale.split("-u-")[0];
  },
});

Preferences.addSetting({
  id: "acceptLanguages",
  pref: "intl.accept_languages",
  get(prefVal, _, setting) {
    return setting.pref.defaultValue != prefVal
      ? prefVal.toLowerCase()
      : Services.locale.acceptLanguages.toLowerCase();
  },
});
Preferences.addSetting({
  id: "availableLanguages",
  deps: ["acceptLanguages"],
  get(_, { acceptLanguages }) {
    let re = /\s*(?:,|$)\s*/;
    let _acceptLanguages = acceptLanguages.value.split(re);
    let availableLanguages = [];
    let localeCodes = [];
    let localeValues = [];
    let bundle = Services.strings.createBundle(
      "resource://gre/res/language.properties"
    );

    for (let currString of bundle.getSimpleEnumeration()) {
      let property = currString.key.split(".");
      if (property[1] == "accept") {
        localeCodes.push(property[0]);
        localeValues.push(currString.value);
      }
    }

    let localeNames = Services.intl.getLocaleDisplayNames(
      undefined,
      localeCodes
    );

    for (let i in localeCodes) {
      let isVisible =
        localeValues[i] == "true" &&
        (!_acceptLanguages.includes(localeCodes[i]) ||
          !_acceptLanguages[localeCodes[i]]);
      let locale = {
        code: localeCodes[i],
        displayName: localeNames[i],
        isVisible,
      };
      availableLanguages.push(locale);
    }

    return availableLanguages;
  },
});

Preferences.addSetting({
  id: "websiteLanguageWrapper",
  deps: ["acceptLanguages"],
  onUserReorder(event, deps) {
    let re = /\s*(?:,|$)\s*/;
    let languages =  (deps.acceptLanguages.value)
      .split(re)
      .filter(lang => lang);
    languages =  (event.target).reorderArrayFromEvent(
      languages,
      event
    );
    deps.acceptLanguages.value = languages.join(",");
  },
  getControlConfig(config, deps) {
    let languagePref = deps.acceptLanguages.value;
    let localeCodes = languagePref
      .toLowerCase()
      .split(/\s*,\s*/)
      .filter(code => code.length);
    let localeDisplayNames = Services.intl.getLocaleDisplayNames(
      undefined,
      localeCodes
    );
    let availableLanguages = [];
    for (let i = 0; i < localeCodes.length; i++) {
      let displayName = localeDisplayNames[i];
      let localeCode = localeCodes[i];
      availableLanguages.push({
        l10nId: "languages-code-format",
        l10nArgs: {
          locale: displayName,
          code: localeCode,
        },
        control: "moz-box-item",
        key: localeCode,
        options: [
          {
            control: "moz-button",
            slot: "actions-start",
            iconSrc: "chrome://global/skin/icons/delete.svg",
            l10nId: "website-remove-language-button",
            l10nArgs: {
              locale: displayName,
              code: localeCode,
            },
            controlAttrs: {
              locale: localeCode,
              action: "remove",
            },
          },
        ],
      });
    }
    config.options = [config.options[0], ...availableLanguages];
    return config;
  },
  onUserClick(e, deps) {
    let code = e.target.getAttribute("locale");
    let action = e.target.getAttribute("action");
    if (code && action) {
      if (action === "remove") {
        let re = /\s*(?:,|$)\s*/;
        let acceptedLanguages = deps.acceptLanguages.value.split(re);
        let filteredLanguages = acceptedLanguages.filter(
          acceptedCode => acceptedCode !== code
        );
        deps.acceptLanguages.value = filteredLanguages.join(",");
        let closestBoxItem = e.target.closest("moz-box-item");
        closestBoxItem.nextElementSibling
          ? closestBoxItem.nextElementSibling.focus()
          : closestBoxItem.previousElementSibling.focus();
      }
    }
  },
});

Preferences.addSetting({
  id: "websiteLanguageAddLanguage",
  deps: ["websiteLanguagePicker", "acceptLanguages"],
  onUserClick(e, deps) {
    let selectedLanguage = deps.websiteLanguagePicker.value;
    if (selectedLanguage == "-1") {
      return;
    }

    let re = /\s*(?:,|$)\s*/;
    let currentLanguages = deps.acceptLanguages.value.split(re);
    let isAlreadyAccepted = currentLanguages.includes(selectedLanguage);

    if (isAlreadyAccepted) {
      return;
    }

    currentLanguages.unshift(selectedLanguage);
    deps.acceptLanguages.value = currentLanguages.join(",");
  },
});

Preferences.addSetting(
   ({
    id: "websiteLanguagePicker",
    deps: ["availableLanguages", "acceptLanguages"],
    inputValue: "-1",
    getControlConfig(config, deps) {
      let re = /\s*(?:,|$)\s*/;
      let availableLanguages =
        deps.availableLanguages.value;

      let acceptLanguages = new Set(
         (deps.acceptLanguages.value).split(re)
      );

      let sortedOptions = availableLanguages.map(locale => ({
        l10nId: "languages-code-format",
        l10nArgs: {
          locale: locale.displayName,
          code: locale.code,
        },
        hidden: locale.isVisible && acceptLanguages.has(locale.code),
        value: locale.code,
      }));
      let comp = new Services.intl.Collator(undefined, {
        usage: "sort",
      });

      sortedOptions.sort((a, b) => {
        return comp.compare(a.l10nArgs.locale, b.l10nArgs.locale);
      });

      config.options = [config.options[0], ...sortedOptions];
      return config;
    },
    get(_, deps) {
      if (
        !this.inputValue ||
        deps.acceptLanguages.value.split(",").includes(this.inputValue)
      ) {
        this.inputValue = "-1";
      }
      return this.inputValue;
    },
    set(inputVal) {
      this.inputValue = String(inputVal);
    },
  })
);

SettingGroupManager.registerGroups({
  browserLanguage: {
    inProgress: true,
    l10nId: "browser-language-heading",
    headingLevel: 2,
    iconSrc: "chrome://browser/skin/sidebar/firefox.svg",
    items: [
      {
        id: "browserLanguagePreferred",
        l10nId: "browser-language-preferred-label",
        control: "moz-select",
      },
      {
        id: "browserLanguageFallback",
        l10nId: "browser-language-fallback-label",
        control: "moz-select",
      },
      {
        id: "useSystemLocale",
        l10nId: "use-system-locale",
        get l10nArgs() {
          let regionalPrefsLocales = Services.locale.regionalPrefsLocales;
          if (!regionalPrefsLocales.length) {
            return { localeName: "und" };
          }
          let [systemLocale] = regionalPrefsLocales;
          let [displayName] = Services.intl.getLocaleDisplayNames(
            undefined,
            [systemLocale],
            { preferNative: true }
          );
          return { localeName: displayName || systemLocale };
        },
      },
      {
        id: "browserLanguageMessage",
        control: "browser-language-restart-message",
      },
    ],
  },
  websiteLanguage: {
    inProgress: true,
    l10nId: "website-language-heading",
    headingLevel: 2,
    iconSrc: "chrome://global/skin/icons/defaultFavicon.svg",
    items: [
      {
        id: "websiteLanguageWrapper",
        control: "moz-box-group",
        controlAttrs: {
          type: "reorderable-list",
        },
        options: [
          {
            id: "websiteLanguagePickerWrapper",
            l10nId: "website-preferred-language",
            key: "addlanguage",
            control: "moz-box-item",
            slot: "header",
            items: [
              {
                id: "websiteLanguagePicker",
                slot: "actions",
                control: "moz-select",
                options: [
                  {
                    control: "moz-option",
                    l10nId: "website-add-language",
                    controlAttrs: {
                      value: "-1",
                    },
                  },
                ],
              },
              {
                id: "websiteLanguageAddLanguage",
                slot: "actions",
                control: "moz-button",
                iconSrc: "chrome://global/skin/icons/plus.svg",
                l10nId: "website-add-language-button",
              },
            ],
          },
        ],
      },
    ],
  },
});
