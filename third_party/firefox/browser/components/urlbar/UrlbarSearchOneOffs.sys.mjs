/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { SearchOneOffs } from "moz-src:///browser/components/search/SearchOneOffs.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
});


export class UrlbarSearchOneOffs extends SearchOneOffs {
  constructor(view) {
    super(view.input.querySelector(".search-one-offs"));
    this.view = view;
    this.input = view.input;
    lazy.UrlbarPrefs.addObserver(this);
    this.disableOneOffsHorizontalKeyNavigation = true;
  }

  get localButtons() {
    return this.getSelectableButtons(false).filter(b => b.source);
  }

  updateWebEngines() {
    this.invalidateCache();
    if (this.view.isOpen) {
      this._rebuild();
    }
  }

  enable(enable) {
    enable = false;
    if (enable) {
      this.telemetryOrigin = "urlbar";
      this.style.display = "";
      this.textbox = this.view.input.inputField;
      if (this.view.isOpen) {
        this._rebuild();
      }
      this.view.controller.addListener(this);
    } else {
      this.telemetryOrigin = null;
      this.style.display = "none";
      this.textbox = null;
      this.view.controller.removeListener(this);
    }
  }

  onViewOpen() {
    this._on_popupshowing();
  }

  onViewClose() {
    this._on_popuphidden();
  }

  get hasView() {
    return this.style.display != "none" && !this.container.hidden;
  }

  get isViewOpen() {
    return this.view.isOpen;
  }

  set selectedButton(button) {
    if (this.selectedButton == button) {
      return;
    }

    super.selectedButton = button;

    let expectedSearchMode;
    if (button && button != this.view.oneOffSearchButtons.settingsButton) {
      expectedSearchMode = {
        engineName: button.engine?.name,
        source: button.source,
        entry: "oneoff",
      };
      this.input.searchMode = expectedSearchMode;
    } else if (this.input.searchMode) {
      this.input.restoreSearchModeState();
    }
  }

  get selectedButton() {
    return super.selectedButton;
  }

  get selectedViewIndex() {
    return this.view.selectedRowIndex;
  }
  set selectedViewIndex(val) {
    this.view.selectedRowIndex = val;
  }

  closeView() {
    if (this.view) {
      this.view.close();
    }
  }

  handleSearchCommand(event, searchMode) {
    if (
      this.selectedButton == this.view.oneOffSearchButtons.settingsButton ||
      this.selectedButton.classList.contains(
        "searchbar-engine-one-off-add-engine"
      )
    ) {
      this.selectedButton.doCommand();
      this.selectedButton = null;
      return;
    }

    let startQueryParams = {
      allowAutofill:
        !searchMode.engineName &&
        searchMode.source != lazy.UrlbarShared.RESULT_SOURCE.SEARCH,
      event,
    };

    let userTypedSearchString =
      this.input.value && this.input.getAttribute("pageproxystate") != "valid";
    let engine = lazy.SearchService.getEngineByName(searchMode.engineName);

    let { where, params } = this._whereToOpen(event);

    if (
      userTypedSearchString &&
      engine &&
      (event.shiftKey || where != "current")
    ) {
      this.input.handleNavigation({
        event,
        oneOffParams: {
          openWhere: where,
          openParams: params,
          engine: this.selectedButton.engine,
        },
      });
      this.selectedButton = null;
      return;
    }

    switch (where) {
      case "current": {
        this.input.searchMode = searchMode;
        this.input.startQuery(startQueryParams);
        break;
      }
      case "tab": {
        searchMode.isPreview = false;

        let newTab = this.input.window.gBrowser.addTrustedTab("about:newtab");
        this.input.setSearchMode(searchMode, newTab.linkedBrowser);
        if (userTypedSearchString) {
          newTab.linkedBrowser.userTypedValue = this.input.value;
        }
        if (!params?.inBackground) {
          this.input.window.gBrowser.selectedTab = newTab;
          newTab.documentGlobal.gURLBar.startQuery(startQueryParams);
        }
        break;
      }
      default: {
        this.input.searchMode = searchMode;
        this.input.startQuery(startQueryParams);
        this.input.select();
        break;
      }
    }

    this.selectedButton = null;
  }

  setTooltipForEngineButton(button) {
    let aliases = button.engine.aliases;
    if (!aliases.length) {
      super.setTooltipForEngineButton(button);
      return;
    }
    this.document.l10n.setAttributes(
      button,
      "search-one-offs-engine-with-alias",
      {
        engineName: button.engine.name,
        alias: aliases[0],
      }
    );
  }

  async willHide() {
    let superWillHide = await super.willHide();
    if (
      lazy.UrlbarUtils.LOCAL_SEARCH_MODES.some(m =>
        lazy.UrlbarPrefs.get(m.pref)
      )
    ) {
      return false;
    }
    return superWillHide;
  }

  onPrefChanged(changedPref) {
    if (
      [...lazy.UrlbarUtils.LOCAL_SEARCH_MODES.map(m => m.pref)].includes(
        changedPref
      )
    ) {
      this.invalidateCache();
    }
  }

  async _rebuildEngineList(engines, addEngines) {
    await super._rebuildEngineList(engines, addEngines);

    const messageIDs = {
      actions: "search-one-offs-actions",
      bookmarks: "search-one-offs-bookmarks",
      history: "search-one-offs-history",
      tabs: "search-one-offs-tabs",
    };
    for (let { source, pref, restrict } of lazy.UrlbarUtils
      .LOCAL_SEARCH_MODES) {
      if (!lazy.UrlbarPrefs.get(pref)) {
        continue;
      }
      let name = lazy.UrlbarUtils.getResultSourceName(source);
      let button = this.document.createXULElement("button");
      button.id = `urlbar-engine-one-off-item-${name}`;
      button.setAttribute("class", "searchbar-engine-one-off-item");
      button.setAttribute("tabindex", "-1");
      this.document.l10n.setAttributes(button, messageIDs[name], {
        restrict,
      });
      button.source = source;
      this.buttons.appendChild(button);
    }
  }

  _on_click(event) {
    if (event.button == 2) {
      return;
    }

    let button =  (event.originalTarget);

    if (!button.engine && !button.source) {
      return;
    }

    this.selectedButton = button;
    this.handleSearchCommand(event, {
      engineName: button.engine?.name,
      source: button.source,
      entry: "oneoff",
    });
  }

  _on_contextmenu(event) {
    event.preventDefault();
  }
}
