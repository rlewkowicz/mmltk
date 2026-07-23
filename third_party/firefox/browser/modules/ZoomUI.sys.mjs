/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const gLoadContext = Cu.createLoadContext();
const gContentPrefs = Cc["@mozilla.org/content-pref/service;1"].getService(
  Ci.nsIContentPrefService2
);
const gZoomPropertyName = "browser.content.full-zoom";

export var ZoomUI = {
  init(aWindow) {
    aWindow.addEventListener("EndSwapDocShells", onEndSwapDocShells, true);
    aWindow.addEventListener("FullZoomChange", onZoomChange);
    aWindow.addEventListener("TextZoomChange", onZoomChange);
    aWindow.addEventListener(
      "unload",
      () => {
        aWindow.removeEventListener(
          "EndSwapDocShells",
          onEndSwapDocShells,
          true
        );
        aWindow.removeEventListener("FullZoomChange", onZoomChange);
        aWindow.removeEventListener("TextZoomChange", onZoomChange);
      },
      { once: true }
    );
  },

  getGlobalValue() {
    return new Promise(resolve => {
      let cachedVal = gContentPrefs.getCachedGlobal(
        gZoomPropertyName,
        gLoadContext
      );
      if (cachedVal) {
        resolve(parseFloat(cachedVal.value) || 1.0);
        return;
      }
      let value = 1.0;
      gContentPrefs.getGlobal(gZoomPropertyName, gLoadContext, {
        handleResult(pref) {
          if (pref.value) {
            value = parseFloat(pref.value);
          }
        },
        handleCompletion() {
          resolve(value);
        },
        handleError(error) {
          console.error(error);
        },
      });
    });
  },
};

function fullZoomLocationChangeObserver(aSubject) {
  if (!aSubject.documentGlobal) {
    return;
  }
  updateZoomUI(aSubject, false);
}
Services.obs.addObserver(
  fullZoomLocationChangeObserver,
  "browser-fullZoom:location-change"
);

function onEndSwapDocShells(event) {
  updateZoomUI(event.originalTarget);
}

function onZoomChange(event) {
  let browser;
  if (event.target.nodeType == event.target.DOCUMENT_NODE) {
    let topDoc = event.target.defaultView.top.document;
    if (!topDoc.documentElement) {
      return;
    }
    browser = topDoc.documentGlobal.docShell.chromeEventHandler;
  } else {
    browser = event.originalTarget;
  }
  updateZoomUI(browser, true);
}

export async function updateZoomUI(aBrowser, aAnimate = false) {
  let win = aBrowser.documentGlobal;
  if (
    !win.gBrowser ||
    win.gBrowser.selectedBrowser != aBrowser ||
    aBrowser.browsingContext?.topChromeWindow != win
  ) {
    return;
  }

  let appMenuZoomReset = win.document.getElementById(
    "appMenu-zoomReset-button2"
  );
  let customizableZoomControls = win.document.getElementById("zoom-controls");
  let customizableZoomReset = win.document.getElementById("zoom-reset-button");
  let urlbarZoomButton = win.document.getElementById("urlbar-zoom-button");
  let zoomFactor = Math.round(win.ZoomManager.zoom * 100);

  let defaultZoom = Math.round((await ZoomUI.getGlobalValue()) * 100);

  if (!win.gBrowser || win.gBrowser.selectedBrowser != aBrowser) {
    return;
  }


  urlbarZoomButton.hidden =
    defaultZoom == zoomFactor ||
    (aBrowser.currentURI.spec == "about:blank" &&
      (!aBrowser.contentPrincipal ||
        aBrowser.contentPrincipal.isNullPrincipal)) ||
    (customizableZoomControls &&
      customizableZoomControls.getAttribute("cui-areatype") == "toolbar");

  let label = win.gNavigatorBundle.getFormattedString("zoom-button.label", [
    zoomFactor,
  ]);
  let accessibilityLabel = win.gNavigatorBundle.getFormattedString(
    "zoom-button.aria-label",
    [zoomFactor]
  );

  if (appMenuZoomReset) {
    appMenuZoomReset.setAttribute("label", label);
  }
  if (customizableZoomReset) {
    customizableZoomReset.setAttribute("label", label);
  }
  if (!urlbarZoomButton.hidden) {
    if (aAnimate && !win.gReduceMotion) {
      urlbarZoomButton.setAttribute("animate", "true");
    } else {
      urlbarZoomButton.removeAttribute("animate");
    }
    urlbarZoomButton.setAttribute("label", label);
    urlbarZoomButton.setAttribute("aria-label", accessibilityLabel);
  }

  win.FullZoom.updateCommands();
}

import { CustomizableUI } from "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs";

let customizationListener = {};
customizationListener.onWidgetAdded =
  customizationListener.onWidgetRemoved =
  customizationListener.onWidgetMoved =
    function (aWidgetId) {
      if (aWidgetId == "zoom-controls") {
        for (let window of CustomizableUI.windows) {
          updateZoomUI(window.gBrowser.selectedBrowser);
        }
      }
    };
customizationListener.onWidgetReset = customizationListener.onWidgetUndoMove =
  function (aWidgetNode) {
    if (aWidgetNode.id == "zoom-controls") {
      updateZoomUI(aWidgetNode.documentGlobal.gBrowser.selectedBrowser);
    }
  };
CustomizableUI.addListener(customizationListener);
