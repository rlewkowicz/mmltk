/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const displayModes = new Set([
  "fullscreen",
  "standalone",
  "minimal-ui",
  "browser",
]);
const orientationTypes = new Set([
  "any",
  "natural",
  "landscape",
  "portrait",
  "portrait-primary",
  "portrait-secondary",
  "landscape-primary",
  "landscape-secondary",
]);
const textDirections = new Set(["ltr", "rtl", "auto"]);

import { ValueExtractor } from "moz-src:///dom/manifest/ValueExtractor.sys.mjs";

import { ImageObjectProcessor } from "moz-src:///dom/manifest/ImageObjectProcessor.sys.mjs";

const domBundle = Services.strings.createBundle(
  "chrome://global/locale/dom/dom.properties"
);

export var ManifestProcessor = {
  get defaultDisplayMode() {
    return "browser";
  },
  get displayModes() {
    return displayModes;
  },
  get orientationTypes() {
    return orientationTypes;
  },
  get textDirections() {
    return textDirections;
  },
  process(aOptions) {
    const {
      jsonText,
      manifestURL: aManifestURL,
      docURL: aDocURL,
      checkConformance,
    } = aOptions;

    const errors = [];

    let rawManifest = {};
    try {
      rawManifest = JSON.parse(jsonText);
    } catch (e) {
      errors.push({ type: "json", error: e.message });
    }
    if (rawManifest === null) {
      return null;
    }
    if (typeof rawManifest !== "object") {
      const warn = domBundle.GetStringFromName("ManifestShouldBeObject");
      errors.push({ warn });
      rawManifest = {};
    }
    const manifestURL = new URL(aManifestURL);
    const docURL = new URL(aDocURL);
    const extractor = new ValueExtractor(errors, domBundle);
    const imgObjProcessor = new ImageObjectProcessor(
      errors,
      extractor,
      domBundle
    );
    const processedManifest = {
      dir: processDirMember.call(this),
      lang: processLangMember(),
      start_url: processStartURLMember(),
      display: processDisplayMember.call(this),
      orientation: processOrientationMember.call(this),
      name: processNameMember(),
      icons: imgObjProcessor.process(rawManifest, manifestURL, "icons"),
      short_name: processShortNameMember(),
      theme_color: processThemeColorMember(),
      background_color: processBackgroundColorMember(),
    };
    processedManifest.scope = processScopeMember();
    processedManifest.id = processIdMember();
    if (checkConformance) {
      processedManifest.moz_validation = errors;
      processedManifest.moz_manifest_url = manifestURL.href;
    }
    return processedManifest;

    function processDirMember() {
      const spec = {
        objectName: "manifest",
        object: rawManifest,
        property: "dir",
        expectedType: "string",
        trim: true,
      };
      const value = extractor.extractValue(spec);
      if (
        value &&
        typeof value === "string" &&
        this.textDirections.has(value.toLowerCase())
      ) {
        return value.toLowerCase();
      }
      return "auto";
    }

    function processNameMember() {
      const spec = {
        objectName: "manifest",
        object: rawManifest,
        property: "name",
        expectedType: "string",
        trim: true,
      };
      return extractor.extractValue(spec);
    }

    function processShortNameMember() {
      const spec = {
        objectName: "manifest",
        object: rawManifest,
        property: "short_name",
        expectedType: "string",
        trim: true,
      };
      return extractor.extractValue(spec);
    }

    function processOrientationMember() {
      const spec = {
        objectName: "manifest",
        object: rawManifest,
        property: "orientation",
        expectedType: "string",
        trim: true,
      };
      const value = extractor.extractValue(spec);
      if (
        value &&
        typeof value === "string" &&
        this.orientationTypes.has(value.toLowerCase())
      ) {
        return value.toLowerCase();
      }
      return undefined;
    }

    function processDisplayMember() {
      const spec = {
        objectName: "manifest",
        object: rawManifest,
        property: "display",
        expectedType: "string",
        trim: true,
      };
      const value = extractor.extractValue(spec);
      if (
        value &&
        typeof value === "string" &&
        displayModes.has(value.toLowerCase())
      ) {
        return value.toLowerCase();
      }
      return this.defaultDisplayMode;
    }

    function processScopeMember() {
      const spec = {
        objectName: "manifest",
        object: rawManifest,
        property: "scope",
        expectedType: "string",
        trim: false,
      };
      const startURL = new URL(processedManifest.start_url);
      const defaultScope = new URL(".", startURL).href;
      const value = extractor.extractValue(spec);
      if (value === undefined || value === "") {
        return defaultScope;
      }
      let scopeURL = URL.parse(value, manifestURL);
      if (!scopeURL) {
        const warn = domBundle.GetStringFromName("ManifestScopeURLInvalid");
        errors.push({ warn });
        return defaultScope;
      }
      if (scopeURL.origin !== docURL.origin) {
        const warn = domBundle.GetStringFromName("ManifestScopeNotSameOrigin");
        errors.push({ warn });
        return defaultScope;
      }
      if (
        startURL.origin !== scopeURL.origin ||
        startURL.pathname.startsWith(scopeURL.pathname) === false
      ) {
        const warn = domBundle.GetStringFromName(
          "ManifestStartURLOutsideScope"
        );
        errors.push({ warn });
        return defaultScope;
      }
      scopeURL.hash = "";
      scopeURL.search = "";
      return scopeURL.href;
    }

    function processStartURLMember() {
      const spec = {
        objectName: "manifest",
        object: rawManifest,
        property: "start_url",
        expectedType: "string",
        trim: false,
      };
      const defaultStartURL = new URL(docURL).href;
      const value = extractor.extractValue(spec);
      if (value === undefined || value === "") {
        return defaultStartURL;
      }
      let potentialResult = URL.parse(value, manifestURL);
      if (!potentialResult) {
        const warn = domBundle.GetStringFromName("ManifestStartURLInvalid");
        errors.push({ warn });
        return defaultStartURL;
      }
      if (potentialResult.origin !== docURL.origin) {
        const warn = domBundle.GetStringFromName(
          "ManifestStartURLShouldBeSameOrigin"
        );
        errors.push({ warn });
        return defaultStartURL;
      }
      return potentialResult.href;
    }

    function processThemeColorMember() {
      const spec = {
        objectName: "manifest",
        object: rawManifest,
        property: "theme_color",
        expectedType: "string",
        trim: true,
      };
      return extractor.extractColorValue(spec);
    }

    function processBackgroundColorMember() {
      const spec = {
        objectName: "manifest",
        object: rawManifest,
        property: "background_color",
        expectedType: "string",
        trim: true,
      };
      return extractor.extractColorValue(spec);
    }

    function processLangMember() {
      const spec = {
        objectName: "manifest",
        object: rawManifest,
        property: "lang",
        expectedType: "string",
        trim: true,
      };
      return extractor.extractLanguageValue(spec);
    }

    function processIdMember() {
      const startURL = new URL(processedManifest.start_url);

      const spec = {
        objectName: "manifest",
        object: rawManifest,
        property: "id",
        expectedType: "string",
        trim: false,
      };
      const extractedValue = extractor.extractValue(spec);

      if (typeof extractedValue !== "string" || extractedValue === "") {
        return startURL.href;
      }

      let appId = URL.parse(extractedValue, startURL.origin);
      if (!appId) {
        const warn = domBundle.GetStringFromName("ManifestIdIsInvalid");
        errors.push({ warn });
        return startURL.href;
      }

      if (appId.origin !== startURL.origin) {
        const warn = domBundle.GetStringFromName("ManifestIdNotSameOrigin");
        errors.push({ warn });
        return startURL.href;
      }

      return appId.href;
    }
  },
};
