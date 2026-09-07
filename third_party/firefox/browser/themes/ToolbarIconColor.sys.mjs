/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const _windowStateMap = new WeakMap();

export const ToolbarIconColor = {
  init(window) {
    if (_windowStateMap.has(window)) {
      return;
    }

    const state = {
      active: false,
      fullscreen: false,
      customtitlebar: false,
      toolbarLuminanceCache: new Map(),
    };

    _windowStateMap.set(window, state);

    window.addEventListener("nativethemechange", this);
    window.addEventListener("activate", this);
    window.addEventListener("deactivate", this);
    window.addEventListener("toolbarvisibilitychange", this);
    window.addEventListener("windowlwthemeupdate", this);

    if (Services.focus.activeWindow == window) {
      this.inferFromText("activate", window);
    }
  },

  uninit(window) {
    const state = _windowStateMap.get(window);
    if (!state) {
      return;
    }

    window.removeEventListener("nativethemechange", this);
    window.removeEventListener("activate", this);
    window.removeEventListener("deactivate", this);
    window.removeEventListener("toolbarvisibilitychange", this);
    window.removeEventListener("windowlwthemeupdate", this);

    _windowStateMap.delete(window);
  },

  handleEvent(event) {
    const window = event.target;
    switch (event.type) {
      case "activate":
      case "deactivate":
      case "nativethemechange":
      case "windowlwthemeupdate":
        this.inferFromText(event.type, window);
        break;
      case "toolbarvisibilitychange":
        this.inferFromText(event.type, window, event.visible);
        break;
    }
  },

  inferFromText(reason, window, reasonValue) {
    const state = _windowStateMap.get(window);

    if (!state) {
      return;
    }

    switch (reason) {
      case "activate": 
      case "deactivate":
        state.active = reason === "activate";
        break;
      case "fullscreen":
        state.fullscreen = reasonValue;
        break;
      case "nativethemechange":
      case "windowlwthemeupdate":
        state.toolbarLuminanceCache.clear();
        break;
      case "toolbarvisibilitychange":
        break;
      case "customtitlebar":
        state.customtitlebar = reasonValue;
        break;
    }

    let toolbarSelector = ".browser-toolbar:not([collapsed])";
    if (Services.appinfo.nativeMenubar) {
      toolbarSelector += ":not([type=menubar])";
    }

    let cachedLuminances = state.toolbarLuminanceCache;
    let luminances = new Map();
    for (let toolbar of window.document.querySelectorAll(toolbarSelector)) {
      let cacheKey = toolbar.id && toolbar.id + JSON.stringify(state);
      let luminance = cacheKey && cachedLuminances.get(cacheKey);
      if (isNaN(luminance)) {
        let { r, g, b } = InspectorUtils.colorToRGBA(
          window.getComputedStyle(toolbar).color
        );
        luminance = 0.2125 * r + 0.7154 * g + 0.0721 * b;
        if (cacheKey) {
          cachedLuminances.set(cacheKey, luminance);
        }
      }
      luminances.set(toolbar, luminance);
    }

    const luminanceThreshold = 127; 
    for (let [toolbar, luminance] of luminances) {
      toolbar.toggleAttribute("brighttext", luminance > luminanceThreshold);
    }
  },
};
