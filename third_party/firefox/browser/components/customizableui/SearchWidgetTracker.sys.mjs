/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { CustomizableUI } from "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs";

const WIDGET_ID = "search-container";

export const SearchWidgetTracker = {
  init() {
    CustomizableUI.addListener(this);
    this._removeWidgetIfUnused();
  },

  onWidgetAfterDOMChange(node, _nextNode, _container, wasRemoval) {
    if (node.id == WIDGET_ID && wasRemoval) {
      this._removePersistedWidths();
    }
  },

  onCustomizeStart() {
    this._widgetWasInNavBar = this._widgetIsInNavBar;
  },

  onCustomizeEnd() {
    if (!this._widgetWasInNavBar && this._widgetIsInNavBar) {
      Services.prefs.setStringPref(
        "browser.search.widget.lastUsed",
        new Date().toISOString()
      );
    }
    delete this._widgetWasInNavBar;
  },

  _removeWidgetIfUnused() {
    if (!this._widgetIsInNavBar) {
      return;
    }
    let searchBarLastUsed = Services.prefs.getStringPref(
      "browser.search.widget.lastUsed",
      ""
    );
    if (searchBarLastUsed) {
      const removeAfterDaysUnused = Services.prefs.getIntPref(
        "browser.search.widget.removeAfterDaysUnused"
      );
      let saerchBarUnusedThreshold =
        removeAfterDaysUnused * 24 * 60 * 60 * 1000;
      if (new Date() - new Date(searchBarLastUsed) > saerchBarUnusedThreshold) {
        CustomizableUI.removeWidgetFromArea(WIDGET_ID);
      }
    }
  },

  _removePersistedWidths() {
    Services.xulStore.removeValue(
      AppConstants.BROWSER_CHROME_URL,
      WIDGET_ID,
      "width"
    );
    for (let win of CustomizableUI.windows) {
      let searchbar =
        win.document.getElementById(WIDGET_ID) ||
        win.gNavToolbox.palette.querySelector("#" + WIDGET_ID);
      searchbar.removeAttribute("width");
      searchbar.style.removeProperty("width");
    }
  },

  get _widgetIsInNavBar() {
    let placement = CustomizableUI.getPlacementOfWidget(WIDGET_ID);
    return placement?.area == CustomizableUI.AREA_NAVBAR;
  },
};
