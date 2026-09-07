/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  ThemeContentPropertyList: "resource:///modules/ThemeVariableMap.sys.mjs",
  ThemeVariableMap: "resource:///modules/ThemeVariableMap.sys.mjs",
  LightweightThemeManager:
    "resource://gre/modules/LightweightThemeManager.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "BROWSER_THEME_UNIFIED_COLOR_SCHEME",
  "browser.theme.unified-color-scheme",
  false
);

const DEFAULT_THEME_ID = "default-theme@mozilla.org";

const toolkitVariableMap = [
  [
    "--lwt-accent-color",
    {
      lwtProperty: "accentcolor",
      processColor(rgbaChannels) {
        if (!rgbaChannels || rgbaChannels.a == 0) {
          return "white";
        }
        const { r, g, b } = rgbaChannels;
        return `rgb(${r}, ${g}, ${b})`;
      },
    },
  ],
  [
    "--lwt-text-color",
    {
      lwtProperty: "textcolor",
      processColor(rgbaChannels) {
        if (!rgbaChannels) {
          rgbaChannels = { r: 0, g: 0, b: 0 };
        }
        const { r, g, b } = rgbaChannels;
        return `rgba(${r}, ${g}, ${b})`;
      },
    },
  ],
  [
    "--panel-background-color",
    {
      lwtProperty: "popup",
    },
  ],
  [
    "--panel-text-color",
    {
      lwtProperty: "popup_text",
    },
  ],
  [
    "--panel-border-color",
    {
      lwtProperty: "popup_border",
    },
  ],
  [
    "--toolbar-field-background-color",
    {
      lwtProperty: "toolbar_field",
      fallbackColor: "rgba(255, 255, 255, 0.8)",
    },
  ],
  [
    "--toolbar-background-color",
    {
      lwtProperty: "toolbarColor",
    },
  ],
  [
    "--toolbar-text-color",
    {
      lwtProperty: "toolbar_text",
    },
  ],
  [
    "--toolbar-field-text-color",
    {
      lwtProperty: "toolbar_field_text",
      fallbackColor: "black",
    },
  ],
  [
    "--toolbar-field-border-color",
    {
      lwtProperty: "toolbar_field_border",
      fallbackColor: "transparent",
    },
  ],
  [
    "--toolbar-field-background-color-focus",
    {
      lwtProperty: "toolbar_field_focus",
      fallbackProperty: "toolbar_field",
      fallbackColor: "white",
      processColor(rgbaChannels, element, propertyOverrides) {
        if (!rgbaChannels) {
          return null;
        }
        const min_opacity = 0.9;
        let { r, g, b, a } = rgbaChannels;
        if (a < min_opacity) {
          propertyOverrides.set(
            "toolbar_field_text_focus",
            _isColorDark(r, g, b) ? "white" : "black"
          );
          return `rgba(${r}, ${g}, ${b}, ${min_opacity})`;
        }
        return `rgba(${r}, ${g}, ${b}, ${a})`;
      },
    },
  ],
  [
    "--toolbar-field-text-color-focus",
    {
      lwtProperty: "toolbar_field_text_focus",
      fallbackProperty: "toolbar_field_text",
      fallbackColor: "black",
    },
  ],
  [
    "--toolbar-field-border-color-focus",
    {
      lwtProperty: "toolbar_field_border_focus",
    },
  ],
  [
    "--lwt-toolbar-field-highlight",
    {
      lwtProperty: "toolbar_field_highlight",
      processColor(rgbaChannels) {
        if (!rgbaChannels) {
          return null;
        }
        const { r, g, b, a } = rgbaChannels;
        return `rgba(${r}, ${g}, ${b}, ${a})`;
      },
    },
  ],
  [
    "--lwt-toolbar-field-highlight-text",
    {
      lwtProperty: "toolbar_field_highlight_text",
    },
  ],
  [
    "--toolbarbutton-icon-fill",
    {
      lwtProperty: "icon_color",
    },
  ],
  [
    "--toolbarbutton-icon-fill-attention",
    {
      lwtProperty: "icon_attention_color",
    },
  ],
  [
    "--newtab-background-color",
    {
      lwtProperty: "ntp_background",
      processColor(rgbaChannels) {
        if (!rgbaChannels) {
          return null;
        }
        const { r, g, b } = rgbaChannels;
        return `rgb(${r}, ${g}, ${b})`;
      },
    },
  ],
  [
    "--newtab-background-color-secondary",
    { lwtProperty: "ntp_card_background" },
  ],
  [
    "--newtab-text-primary-color",
    {
      lwtProperty: "ntp_text",
      processColor(rgbaChannels, element) {
        if (!rgbaChannels) {
          element.removeAttribute("lwt-newtab-brighttext");
          return null;
        }

        const { r, g, b } = rgbaChannels;
        element.toggleAttribute(
          "lwt-newtab-brighttext",
          0.2125 * r + 0.7154 * g + 0.0721 * b > 110
        );

        return _rgbaToString(rgbaChannels);
      },
    },
  ],
];

LightweightThemeConsumer.init = function (window) {
  new LightweightThemeConsumer(window.document);
};

export function LightweightThemeConsumer(aDocument) {
  this._doc = aDocument;
  this._win = aDocument.defaultView;
  this._winId = this._win.docShell.outerWindowID;
  this._isPrivateWindow = lazy.PrivateBrowsingUtils.isWindowPrivate(this._win);

  XPCOMUtils.defineLazyPreferenceGetter(
    this,
    "FORCED_COLORS_OVERRIDE_ENABLED",
    "browser.theme.forced-colors-override.enabled",
    true,
    () => this._update(this._lastData)
  );

  XPCOMUtils.defineLazyPreferenceGetter(
    this,
    "BROWSER_NOVA_ENABLED",
    "browser.nova.enabled",
    false,
    () => this._update(this._lastData)
  );

  XPCOMUtils.defineLazyPreferenceGetter(
    this,
    "BROWSER_THEME_DARK_PRIVATE_WINDOWS",
    "browser.theme.dark-private-windows",
    true,
    () => this._update(this._lastData)
  );

  Services.obs.addObserver(this, "lightweight-theme-styling-update");

  this.darkThemeMediaQuery = this._win.matchMedia("(-moz-system-dark-theme)");
  this.darkThemeMediaQuery.addListener(this);

  this.forcedColorsMediaQuery = this._win.matchMedia("(forced-colors)");
  this.forcedColorsMediaQuery.addListener(this);

  this._update(lazy.LightweightThemeManager.themeData);

  this._win.addEventListener("unload", this, { once: true });
}

LightweightThemeConsumer.prototype = {
  _lastData: null,

  observe(aSubject, aTopic) {
    if (aTopic != "lightweight-theme-styling-update") {
      return;
    }

    let data = aSubject.wrappedJSObject;
    if (data.window && data.window !== this._winId) {
      return;
    }

    this._update(data);
  },

  handleEvent(aEvent) {
    if (
      aEvent.target == this.darkThemeMediaQuery ||
      aEvent.target == this.forcedColorsMediaQuery
    ) {
      this._update(this._lastData);
      return;
    }

    switch (aEvent.type) {
      case "unload":
        Services.obs.removeObserver(this, "lightweight-theme-styling-update");
        Services.ppmm.sharedData.delete(`theme/${this._winId}`);
        this._win = this._doc = null;
        this.darkThemeMediaQuery?.removeListener(this);
        this.darkThemeMediaQuery = null;
        this.forcedColorsMediaQuery?.removeListener(this);
        this.forcedColorsMediaQuery = null;
        break;
    }
  },

  _update(themeData) {
    const manager = lazy.LightweightThemeManager;

    this._lastData = themeData;
    let isPrivateThemeActive = false;

    const shouldMakePrivateWindowDark =
      this._isPrivateWindow &&
      !lazy.PrivateBrowsingUtils.permanentPrivateBrowsing &&
      this.BROWSER_THEME_DARK_PRIVATE_WINDOWS;
    const themeId = themeData.theme?.id ?? DEFAULT_THEME_ID;
    let isDefaultOrInApp = DEFAULT_THEME_ID === themeId;

    if (
      shouldMakePrivateWindowDark &&
      this.BROWSER_NOVA_ENABLED &&
      isDefaultOrInApp
    ) {
      if (manager.privateThemeData) {
        themeData = manager.privateThemeData;
        isPrivateThemeActive = true;
      } else {
        manager.promisePrivateThemeData().then(() => {
          if (this._win && !this._win.closed) {
            this._update(this._lastData);
          }
        });
        return;
      }
    }

    let updateGlobalThemeData = true;
    const useDarkTheme = (() => {
      let supportsDarkTheme =
        !!themeData.darkTheme ||
        !themeData.theme ||
        themeData.theme.id == DEFAULT_THEME_ID;

      if (!supportsDarkTheme) {
        return false;
      }

      if (this.darkThemeMediaQuery?.matches) {
        return true;
      }

      if (!shouldMakePrivateWindowDark) {
        return false;
      }
      updateGlobalThemeData = false;
      return true;
    })();

    if (isPrivateThemeActive) {
      updateGlobalThemeData = false;
    }

    let theme = useDarkTheme ? themeData.darkTheme : themeData.theme;
    let forcedColorsThemeOverride =
      this.FORCED_COLORS_OVERRIDE_ENABLED &&
      this.forcedColorsMediaQuery?.matches;
    if (!theme || forcedColorsThemeOverride) {
      theme = { id: DEFAULT_THEME_ID };
    }
    let hasTheme = theme.id != DEFAULT_THEME_ID;
    this._doc.forceNonNativeTheme = false;

    let root = this._doc.documentElement;
    root.toggleAttribute("lwtheme-image", !!(hasTheme && theme.headerImage));
    root.toggleAttribute("theme-in-app", isDefaultOrInApp);
    root.setAttribute("theme-effective-id", theme.id);
    root.toggleAttribute(
      "theme-image-in-toolbox",
      (() => {
        if (hasTheme) {
          return !!theme.backgroundsAlignment?.split(",").some(alignment => {
            if (alignment == "center" || alignment == "bottom") {
              return true;
            }
            let [, y] = alignment.split(" ");
            return y == "center" || y == "bottom";
          });
        }
        return this.BROWSER_NOVA_ENABLED;
      })()
    );
    this._setExperiment(hasTheme, themeData.experiment, theme.experimental);
    _setImage(
      this._win,
      root,
      hasTheme,
      "--lwt-header-image",
      theme.headerImage
    );
    _setImage(
      this._win,
      root,
      hasTheme,
      "--lwt-additional-images",
      theme.additionalBackgrounds
    );

    let processedColors = _setProperties(root, hasTheme, theme);
    _setDarkModeAttributes(this._doc, root, theme, processedColors, hasTheme);

    let toolbarColorScheme = (() => {
      if (useDarkTheme || themeData.darkTheme) {
        return useDarkTheme ? "dark" : "light";
      }
      switch (theme.color_scheme) {
        case "light":
        case "dark":
          return theme.color_scheme;
        case "system":
          return null;
        default:
          break;
      }
      if (!hasTheme) {
        return null;
      }
      return _isToolbarDark(this._doc, theme, processedColors, true)
        ? "dark"
        : "light";
    })();

    let contentColorScheme = (() => {
      if (lazy.BROWSER_THEME_UNIFIED_COLOR_SCHEME) {
        return toolbarColorScheme;
      }
      let themeOverride = theme.content_color_scheme || theme.color_scheme;
      switch (themeOverride) {
        case "light":
        case "dark":
          return themeOverride;
        case "system":
          return null;
        default:
          break;
      }
      return null;
    })();

    if (!this._win.browsingContext.parent) {
      this._win.browsingContext.prefersColorSchemeOverride =
        toolbarColorScheme || "none";
    }
    if (updateGlobalThemeData) {
      function colorSchemeToThemePref(scheme) {
        switch (scheme) {
          case "dark":
            return 0;
          case "light":
            return 1;
          default:
            return 2; 
        }
      }
      Services.prefs.setIntPref(
        "browser.theme.toolbar-theme",
        colorSchemeToThemePref(toolbarColorScheme)
      );
      Services.prefs.setIntPref(
        "browser.theme.content-theme",
        colorSchemeToThemePref(contentColorScheme)
      );
    }
    root.toggleAttribute("lwtheme", hasTheme);

    let contentThemeData = _getContentProperties(this._doc, hasTheme, theme);
    Services.ppmm.sharedData.set(`theme/${this._winId}`, contentThemeData);
    Services.ppmm.sharedData.flush();

    this._win.dispatchEvent(new CustomEvent("windowlwthemeupdate"));
  },

  _setExperiment(hasTheme, experiment, properties) {
    const root = this._doc.documentElement;
    if (this._lastExperimentData) {
      const { stylesheet, usedVariables } = this._lastExperimentData;
      if (stylesheet) {
        stylesheet.remove();
      }
      if (usedVariables) {
        for (const [variable] of usedVariables) {
          _setProperty(root, false, variable);
        }
      }
    }

    this._lastExperimentData = {};

    if (!hasTheme || !experiment) {
      return;
    }

    let usedVariables = [];
    if (properties.colors) {
      for (const property in properties.colors) {
        const cssVariable = experiment.colors[property];
        const value = _rgbaToString(
          _cssColorToRGBA(root.ownerDocument, properties.colors[property])
        );
        usedVariables.push([cssVariable, value]);
      }
    }

    if (properties.images) {
      for (const property in properties.images) {
        const cssVariable = experiment.images[property];
        usedVariables.push([
          cssVariable,
          `url(${properties.images[property]})`,
        ]);
      }
    }
    if (properties.properties) {
      for (const property in properties.properties) {
        const cssVariable = experiment.properties[property];
        usedVariables.push([cssVariable, properties.properties[property]]);
      }
    }
    for (const [variable, value] of usedVariables) {
      _setProperty(root, true, variable, value);
    }
    this._lastExperimentData.usedVariables = usedVariables;

    if (experiment.stylesheet) {
      let stylesheet = this._doc.createElement("link");
      stylesheet.rel = "stylesheet";
      stylesheet.href = experiment.stylesheet;
      this._doc.head.appendChild(stylesheet);
      this._lastExperimentData.stylesheet = stylesheet;
    }
  },
};

function _getContentProperties(doc, hasTheme, data) {
  let properties = { hasTheme };
  if (!hasTheme) {
    return properties;
  }
  for (let property in data) {
    if (lazy.ThemeContentPropertyList.includes(property)) {
      properties[property] = _cssColorToRGBA(doc, data[property]);
    }
  }
  if (data.experimental) {
    for (const property in data.experimental.colors) {
      if (lazy.ThemeContentPropertyList.includes(property)) {
        properties[property] = _cssColorToRGBA(
          doc,
          data.experimental.colors[property]
        );
      }
    }
    for (const property in data.experimental.images) {
      if (lazy.ThemeContentPropertyList.includes(property)) {
        properties[property] = `url(${data.experimental.images[property]})`;
      }
    }
    for (const property in data.experimental.properties) {
      if (lazy.ThemeContentPropertyList.includes(property)) {
        properties[property] = data.experimental.properties[property];
      }
    }
  }
  return properties;
}

function _imageToCss(aWin, aImage) {
  if (typeof aImage == "object") {
    const [gradient, args] = Object.entries(aImage)[0];
    return `${gradient}(${args})`;
  }
  return `url(${aWin.CSS.escape(aImage)})`;
}

function _setImage(aWin, aRoot, aActive, aVariableName, aImages) {
  if (aImages && !Array.isArray(aImages)) {
    aImages = [aImages];
  }
  _setProperty(
    aRoot,
    aActive,
    aVariableName,
    aImages && aImages.map(v => _imageToCss(aWin, v)).join(", ")
  );
}

function _setProperty(elem, hasTheme, variableName, value) {
  if (hasTheme && value) {
    elem.style.setProperty(variableName, value);
  } else {
    elem.style.removeProperty(variableName);
  }
}

function _isToolbarDark(doc, theme, colors, hasTheme) {
  if (colors.toolbarColor) {
    let color = _cssColorToRGBA(doc, colors.toolbarColor);
    if (color.a == 1) {
      return _isColorDark(color.r, color.g, color.b);
    }
  }
  if (colors.toolbar_text) {
    let color = _cssColorToRGBA(doc, colors.toolbar_text);
    return !_isColorDark(color.r, color.g, color.b);
  }
  return _hasDarkFrame(doc, theme, colors, hasTheme);
}

function _hasDarkFrame(doc, theme, colors, hasTheme) {
  if (!hasTheme) {
    return false;
  }
  if (!theme.headerImage && colors.accentcolor) {
    let color = _cssColorToRGBA(doc, colors.accentcolor);
    if (color.a == 1) {
      return _isColorDark(color.r, color.g, color.b);
    }
  }
  let textColor = _cssColorToRGBA(doc, colors.textcolor || "black");
  return !_isColorDark(textColor.r, textColor.g, textColor.b);
}

function _setDarkModeAttributes(doc, root, theme, colors, hasTheme) {
  root.toggleAttribute(
    "lwtheme-brighttext",
    _hasDarkFrame(doc, theme, colors, hasTheme)
  );

  if (hasTheme) {
    root.setAttribute(
      "lwt-toolbar",
      _isToolbarDark(doc, theme, colors, hasTheme) ? "dark" : "light"
    );
  } else {
    root.removeAttribute("lwt-toolbar");
  }

  const setAttribute = function (
    attribute,
    textPropertyName,
    backgroundPropertyName
  ) {
    let dark = _determineIfColorPairIsDark(
      doc,
      colors,
      textPropertyName,
      backgroundPropertyName
    );
    if (dark === null) {
      root.removeAttribute(attribute);
    } else {
      root.setAttribute(attribute, dark ? "dark" : "light");
    }
  };

  setAttribute("lwt-tab-selected", "tab_text", "tab_selected");
  setAttribute("lwt-toolbar-field", "toolbar_field_text", "toolbar_field");
  setAttribute(
    "lwt-toolbar-field-focus",
    "toolbar_field_text_focus",
    "toolbar_field_focus"
  );
  setAttribute("lwt-popup", "popup_text", "popup");
  setAttribute("lwt-sidebar", "sidebar_text", "sidebar");
  setAttribute(
    "lwt-icon-fill-attention",
     null,
    "icon_attention_color"
  );
}

function _determineIfColorPairIsDark(
  doc,
  colors,
  textPropertyName,
  backgroundPropertyName
) {
  let backgroundColor =
    backgroundPropertyName && colors[backgroundPropertyName];
  let textColor = textPropertyName && colors[textPropertyName];
  if (!backgroundColor && !textColor) {
    return null;
  }

  let color = _cssColorToRGBA(doc, backgroundColor);
  if (color && color.a == 1) {
    return _isColorDark(color.r, color.g, color.b);
  }

  color = _cssColorToRGBA(doc, textColor);
  if (!color) {
    return null;
  }

  return !_isColorDark(color.r, color.g, color.b);
}

function _setProperties(root, hasTheme, themeData) {
  let propertyOverrides = new Map();
  let doc = root.ownerDocument;

  let processedColors = { ...themeData };
  for (let map of [toolkitVariableMap, lazy.ThemeVariableMap]) {
    for (let [cssVarName, definition] of map) {
      const {
        lwtProperty,
        fallbackProperty,
        fallbackColor,
        processColor,
        isColor = true,
      } = definition;
      let val = propertyOverrides.get(lwtProperty) || themeData[lwtProperty];
      if (isColor) {
        val = _cssColorToRGBA(doc, val);
        if (!val && fallbackProperty) {
          val = _cssColorToRGBA(doc, themeData[fallbackProperty]);
        }
        if (!val && hasTheme && fallbackColor) {
          val = _cssColorToRGBA(doc, fallbackColor);
        }
        if (processColor) {
          val = processColor(val, root, propertyOverrides);
        } else {
          val = _rgbaToString(val);
        }
      }

      processedColors[lwtProperty] = val;

      _setProperty(root, hasTheme, cssVarName, val);
    }
  }
  return processedColors;
}

const kInvalidColor = { r: 0, g: 0, b: 0, a: 1 };

function _cssColorToRGBA(doc, cssColor) {
  if (!cssColor) {
    return null;
  }
  return doc.defaultView.InspectorUtils.colorToRGBA(cssColor) || kInvalidColor;
}

function _rgbaToString(parsedColor) {
  if (!parsedColor) {
    return null;
  }
  let { r, g, b, a } = parsedColor;
  if (a == 1) {
    return `rgb(${r}, ${g}, ${b})`;
  }
  return `rgba(${r}, ${g}, ${b}, ${a})`;
}

function _isColorDark(r, g, b) {
  return 0.2125 * r + 0.7154 * g + 0.0721 * b <= 127;
}
