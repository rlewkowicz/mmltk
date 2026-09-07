/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import { html } from "chrome://global/content/vendor/lit.all.mjs";
import { MozLitElement } from "chrome://global/content/lit-utils.mjs";

class ContentSearchHandoffUIController {
  #ui = null;
  #shadowRoot = null;

  constructor(ui) {
    this._isPrivateEngine = false;
    this._engineIcon = null;
    this.#ui = ui;
    this.#shadowRoot = ui.shadowRoot;

    window.addEventListener("ContentSearchService", this);
    this._sendMsg("GetEngine");
    this._sendMsg("GetHandoffSearchModePrefs");
  }

  handleEvent(event) {
    let methodName = "_onMsg" + event.detail.type;
    if (methodName in this) {
      this[methodName](event.detail.data);
    }
  }

  get defaultEngine() {
    return this._defaultEngine;
  }

  doSearchHandoff(text) {
    this._sendMsg("SearchHandoff", { text });
  }

  static privateBrowsingRegex = /^about:privatebrowsing([#?]|$)/i;
  get _isAboutPrivateBrowsing() {
    return ContentSearchHandoffUIController.privateBrowsingRegex.test(
      document.location.href
    );
  }

  _onMsgEngine({ isPrivateEngine, engine }) {
    this._isPrivateEngine = isPrivateEngine;
    this._updateEngine(engine);
  }

  _onMsgCurrentEngine(engine) {
    if (!this._isPrivateEngine) {
      this._updateEngine(engine);
    }
  }

  _onMsgCurrentPrivateEngine(engine) {
    if (this._isPrivateEngine) {
      this._updateEngine(engine);
    }
  }

  _onMsgHandoffSearchModePrefs(pref) {
    this._shouldHandOffToSearchMode = pref;
    this._updatel10nIds();
  }

  _onMsgDisableSearch() {
    this.#ui.disabled = true;
  }

  _onMsgShowSearch() {
    this.#ui.disabled = false;
    this.#ui.fakeFocus = false;
  }

  _updateEngine(engine) {
    this._defaultEngine = engine;
    if (this._engineIcon) {
      URL.revokeObjectURL(this._engineIcon);
    }

    if (!engine.isConfigEngine) {
      this._engineIcon = "chrome://global/skin/icons/search-glass.svg";
    } else if (engine.iconData) {
      this._engineIcon = this._getFaviconURIFromIconData(engine.iconData);
    } else {
      this._engineIcon = "chrome://global/skin/icons/defaultFavicon.svg";
    }

    document.body.style.setProperty(
      "--newtab-search-icon",
      "url(" + this._engineIcon + ")"
    );
    this._updatel10nIds();
  }

  _updatel10nIds() {
    let engine = this._defaultEngine;
    let fakeButton = this.#shadowRoot.querySelector(".search-handoff-button");
    let fakeInput = this.#shadowRoot.querySelector(".fake-textbox");
    if (!fakeButton || !fakeInput) {
      return;
    }
    if (!engine || this._shouldHandOffToSearchMode) {
      document.l10n.setAttributes(
        fakeButton,
        this._isAboutPrivateBrowsing
          ? "about-private-browsing-search-btn"
          : "newtab-search-box-input"
      );
      document.l10n.setAttributes(
        fakeInput,
        this._isAboutPrivateBrowsing
          ? "about-private-browsing-search-placeholder"
          : "newtab-search-box-text"
      );
    } else if (!engine.isConfigEngine) {
      document.l10n.setAttributes(
        fakeButton,
        this._isAboutPrivateBrowsing
          ? "about-private-browsing-handoff-no-engine"
          : "newtab-search-box-handoff-input-no-engine"
      );
      document.l10n.setAttributes(
        fakeInput,
        this._isAboutPrivateBrowsing
          ? "about-private-browsing-handoff-text-no-engine"
          : "newtab-search-box-handoff-text-no-engine"
      );
    } else {
      document.l10n.setAttributes(
        fakeButton,
        this._isAboutPrivateBrowsing
          ? "about-private-browsing-handoff"
          : "newtab-search-box-handoff-input",
        {
          engine: engine.name,
        }
      );
      document.l10n.setAttributes(
        fakeInput,
        this._isAboutPrivateBrowsing
          ? "about-private-browsing-handoff-text"
          : "newtab-search-box-handoff-text",
        {
          engine: engine.name,
        }
      );
    }
  }

  _getFaviconURIFromIconData(data) {
    if (typeof data == "string") {
      return data;
    }

    let blob = new Blob([data.icon], { type: data.mimeType });
    return URL.createObjectURL(blob);
  }

  _sendMsg(type, data = null) {
    dispatchEvent(
      new CustomEvent("ContentSearchClient", {
        detail: {
          type,
          data,
        },
      })
    );
  }
}

window.ContentSearchHandoffUIController = ContentSearchHandoffUIController;

window.ContentSearchUIController = (function () {
  const MAX_DISPLAYED_SUGGESTIONS = 6;
  const SUGGESTION_ID_PREFIX = "searchSuggestion";
  const ONE_OFF_ID_PREFIX = "oneOff";
  const HTML_NS = "http://www.w3.org/1999/xhtml";

  function ContentSearchUIController(
    inputElement,
    tableParent,
    healthReportKey,
    idPrefix = ""
  ) {
    this.input = inputElement;
    this._idPrefix = idPrefix;
    this._healthReportKey = healthReportKey;
    this._isPrivateEngine = false;

    let tableID = idPrefix + "searchSuggestionTable";
    this.input.autocomplete = "off";
    this.input.setAttribute("aria-autocomplete", "true");
    this.input.setAttribute("aria-controls", tableID);
    tableParent.appendChild(this._makeTable(tableID));

    this._tableParent = tableParent;

    this.input.addEventListener("keydown", this);
    this.input.addEventListener("input", this);
    this.input.addEventListener("focus", this);
    this.input.addEventListener("blur", this);
    window.addEventListener("ContentSearchService", this);

    this._stickyInputValue = "";
    this._hideSuggestions();

    this._getSearchEngines();
    this._getStrings();
  }

  ContentSearchUIController.prototype = {
    _oneOffButtons: [],
    _pendingOneOffRefresh: undefined,

    get defaultEngine() {
      return this._defaultEngine;
    },

    set defaultEngine(engine) {
      if (this._defaultEngine && this._defaultEngine.icon) {
        URL.revokeObjectURL(this._defaultEngine.icon);
      }
      let icon;
      if (engine.iconData) {
        icon = this._getFaviconURIFromIconData(engine.iconData);
      } else {
        icon = "chrome://global/skin/icons/defaultFavicon.svg";
      }
      this._defaultEngine = {
        name: engine.name,
        icon,
        isConfigEngine: engine.isConfigEngine,
      };
      this._updateDefaultEngineHeader();
      this._updateDefaultEngineIcon();

      if (engine && document.activeElement == this.input) {
        this._speculativeConnect();
      }
    },

    get engines() {
      return this._engines;
    },

    set engines(val) {
      this._engines = val;
      this._pendingOneOffRefresh = true;
    },

    get selectedIndex() {
      let allElts = [
        ...this._suggestionsList.children,
        ...this._oneOffButtons,
        this._tableParent.querySelector("#contentSearchSettingsButton"),
      ];
      for (let i = 0; i < allElts.length; ++i) {
        let elt = allElts[i];
        if (elt.classList.contains("selected")) {
          return i;
        }
      }
      return -1;
    },

    set selectedIndex(idx) {
      this._table.removeAttribute("aria-activedescendant");
      this.input.removeAttribute("aria-activedescendant");

      let allElts = [
        ...this._suggestionsList.children,
        ...this._oneOffButtons,
        this._tableParent.querySelector("#contentSearchSettingsButton"),
      ];
      let excludeIndex =
        idx < this.numSuggestions && this.selectedButtonIndex > -1
          ? this.numSuggestions + this.selectedButtonIndex
          : -1;
      for (let i = 0; i < allElts.length; ++i) {
        let elt = allElts[i];
        let ariaSelectedElt = i < this.numSuggestions ? elt.firstChild : elt;
        if (i == idx) {
          elt.classList.add("selected");
          ariaSelectedElt.setAttribute("aria-selected", "true");
          this.input.setAttribute("aria-activedescendant", ariaSelectedElt.id);
        } else if (i != excludeIndex) {
          elt.classList.remove("selected");
          ariaSelectedElt.setAttribute("aria-selected", "false");
        }
      }
    },

    get selectedButtonIndex() {
      let elts = [
        ...this._oneOffButtons,
        this._tableParent.querySelector("#contentSearchSettingsButton"),
      ];
      for (let i = 0; i < elts.length; ++i) {
        if (elts[i].classList.contains("selected")) {
          return i;
        }
      }
      return -1;
    },

    set selectedButtonIndex(idx) {
      let elts = [
        ...this._oneOffButtons,
        this._tableParent.querySelector("#contentSearchSettingsButton"),
      ];
      for (let i = 0; i < elts.length; ++i) {
        let elt = elts[i];
        if (i == idx) {
          elt.classList.add("selected");
          elt.setAttribute("aria-selected", "true");
        } else {
          elt.classList.remove("selected");
          elt.setAttribute("aria-selected", "false");
        }
      }
    },

    get selectedEngineName() {
      let selectedElt = this._oneOffsTable.querySelector(".selected");
      if (selectedElt) {
        return selectedElt.engineName;
      }
      return this.defaultEngine.name;
    },

    get numSuggestions() {
      return this._suggestionsList.children.length;
    },

    selectAndUpdateInput(idx) {
      this.selectedIndex = idx;
      let newValue = this.suggestionAtIndex(idx) || this._stickyInputValue;
      if (this.input.value != newValue) {
        this.input.value = newValue;
      }
      this._updateSearchWithHeader();
    },

    suggestionAtIndex(idx) {
      let row = this._suggestionsList.children[idx];
      return row ? row.textContent : null;
    },

    deleteSuggestionAtIndex(idx) {
      if (this.isFormHistorySuggestionAtIndex(idx)) {
        let suggestionStr = this.suggestionAtIndex(idx);
        this._sendMsg("RemoveFormHistoryEntry", suggestionStr);
        this._suggestionsList.children[idx].remove();
        this.selectAndUpdateInput(-1);
      }
    },

    isFormHistorySuggestionAtIndex(idx) {
      let row = this._suggestionsList.children[idx];
      return row && row.classList.contains("formHistory");
    },

    addInputValueToFormHistory() {
      let entry = {
        value: this.input.value,
        engineName: this.selectedEngineName,
      };
      this._sendMsg("AddFormHistoryEntry", entry);
      return entry;
    },

    handleEvent(event) {
      if (!this.input.isConnected) {
        return;
      }
      this["_on" + event.type[0].toUpperCase() + event.type.substr(1)](event);
    },

    _onCommand(aEvent) {
      if (this.selectedButtonIndex == this._oneOffButtons.length) {
        this._sendMsg("ManageEngines");
        return;
      }

      this.search(aEvent);

      if (aEvent) {
        aEvent.preventDefault();
      }
    },

    search(aEvent) {
      if (!this.defaultEngine) {
        return; 
      }

      let searchText = this.input;
      let searchTerms;
      if (
        this._table.hidden ||
        (aEvent.originalTarget &&
          aEvent.originalTarget.id == "contentSearchDefaultEngineHeader") ||
        aEvent instanceof KeyboardEvent
      ) {
        searchTerms = searchText.value;
      } else {
        searchTerms =
          this.suggestionAtIndex(this.selectedIndex) || searchText.value;
      }
      let eventData = {
        engineName: this.selectedEngineName,
        searchString: searchTerms,
        healthReportKey: this._healthReportKey,
        originalEvent: {
          shiftKey: aEvent.shiftKey,
          ctrlKey: aEvent.ctrlKey,
          metaKey: aEvent.metaKey,
          altKey: aEvent.altKey,
        },
      };
      if ("button" in aEvent) {
        eventData.originalEvent.button = aEvent.button;
      }

      if (this.suggestionAtIndex(this.selectedIndex)) {
        eventData.selection = {
          index: this.selectedIndex,
          kind: undefined,
        };
        if (aEvent instanceof MouseEvent) {
          eventData.selection.kind = "mouse";
        } else if (aEvent instanceof KeyboardEvent) {
          eventData.selection.kind = "key";
        }
      }

      this._sendMsg("Search", eventData);
      this.addInputValueToFormHistory();
    },

    _onInput() {
      if (!this.input.value) {
        this._stickyInputValue = "";
        this._hideSuggestions();
      } else if (this.input.value != this._stickyInputValue) {
        this._getSuggestions();
        this.selectAndUpdateInput(-1);
      }
      this._updateSearchWithHeader();
    },

    _onKeydown(event) {
      let selectedIndexDelta = 0;
      let selectedSuggestionDelta = 0;
      let selectedOneOffDelta = 0;

      switch (event.keyCode) {
        case event.DOM_VK_UP:
          if (this._table.hidden) {
            return;
          }
          if (event.getModifierState("Accel")) {
            if (event.shiftKey) {
              selectedSuggestionDelta = -1;
              break;
            }
            this._cycleCurrentEngine(true);
            break;
          }
          if (event.altKey) {
            selectedOneOffDelta = -1;
            break;
          }
          selectedIndexDelta = -1;
          break;
        case event.DOM_VK_DOWN:
          if (this._table.hidden) {
            this._getSuggestions();
            return;
          }
          if (event.getModifierState("Accel")) {
            if (event.shiftKey) {
              selectedSuggestionDelta = 1;
              break;
            }
            this._cycleCurrentEngine(false);
            break;
          }
          if (event.altKey) {
            selectedOneOffDelta = 1;
            break;
          }
          selectedIndexDelta = 1;
          break;
        case event.DOM_VK_TAB:
          if (this._table.hidden) {
            return;
          }
          if (
            (this.selectedButtonIndex <= 0 && event.shiftKey) ||
            (this.selectedButtonIndex == this._oneOffButtons.length &&
              !event.shiftKey)
          ) {
            return;
          }
          selectedOneOffDelta = event.shiftKey ? -1 : 1;
          break;
        case event.DOM_VK_RIGHT:
          if (
            this.input.selectionStart != this.input.selectionEnd ||
            this.input.selectionEnd != this.input.value.length
          ) {
            return;
          }
          if (
            this.numSuggestions &&
            this.selectedIndex >= 0 &&
            this.selectedIndex < this.numSuggestions
          ) {
            this.input.value = this.suggestionAtIndex(this.selectedIndex);
            this.input.setAttribute("selection-index", this.selectedIndex);
            this.input.setAttribute("selection-kind", "key");
          } else {
            this.input.removeAttribute("selection-index");
            this.input.removeAttribute("selection-kind");
          }
          this._stickyInputValue = this.input.value;
          this._hideSuggestions();
          return;
        case event.DOM_VK_RETURN:
          this._onCommand(event);
          return;
        case event.DOM_VK_DELETE:
          if (this.selectedIndex >= 0) {
            this.deleteSuggestionAtIndex(this.selectedIndex);
          }
          return;
        case event.DOM_VK_ESCAPE:
          if (!this._table.hidden) {
            this._hideSuggestions();
          }
          return;
        default:
          return;
      }

      let currentIndex = this.selectedIndex;
      if (selectedIndexDelta) {
        let newSelectedIndex = currentIndex + selectedIndexDelta;
        if (newSelectedIndex < -1) {
          newSelectedIndex = this.numSuggestions + this._oneOffButtons.length;
        }
        if (currentIndex == this.numSuggestions && selectedIndexDelta == -1) {
          this.selectedButtonIndex = -1;
        }
        this.selectAndUpdateInput(newSelectedIndex);
      } else if (selectedSuggestionDelta) {
        let newSelectedIndex;
        if (currentIndex >= this.numSuggestions || currentIndex == -1) {
          newSelectedIndex =
            selectedSuggestionDelta == 1 ? 0 : this.numSuggestions - 1;
        } else {
          newSelectedIndex = currentIndex + selectedSuggestionDelta;
        }
        if (newSelectedIndex >= this.numSuggestions) {
          newSelectedIndex = -1;
        }
        this.selectAndUpdateInput(newSelectedIndex);
      } else if (selectedOneOffDelta) {
        let newSelectedIndex;
        let currentButton = this.selectedButtonIndex;
        if (
          currentButton == -1 ||
          currentButton == this._oneOffButtons.length
        ) {
          newSelectedIndex =
            selectedOneOffDelta == 1 ? 0 : this._oneOffButtons.length - 1;
        } else {
          newSelectedIndex = currentButton + selectedOneOffDelta;
        }
        if (
          newSelectedIndex == this._oneOffButtons.length &&
          event.keyCode != event.DOM_VK_TAB
        ) {
          newSelectedIndex = -1;
        }
        this.selectedButtonIndex = newSelectedIndex;
      }

      event.preventDefault();
    },

    _currentEngineIndex: -1,
    _cycleCurrentEngine(aReverse) {
      if (
        (this._currentEngineIndex == this._engines.length - 1 && !aReverse) ||
        (this._currentEngineIndex == 0 && aReverse)
      ) {
        return;
      }
      this._currentEngineIndex += aReverse ? -1 : 1;
      let engineName = this._engines[this._currentEngineIndex].name;
      this._sendMsg("SetCurrentEngine", engineName);
    },

    _onFocus() {
      if (this._mousedown) {
        return;
      }
      this.input.setAttribute("keepfocus", "true");
      this._speculativeConnect();
    },

    _onBlur() {
      if (this._mousedown) {
        setTimeout(() => this.input.focus(), 0);
        return;
      }
      this.input.removeAttribute("keepfocus");
      this._hideSuggestions();
    },

    _onMousemove(event) {
      let idx = this._indexOfTableItem(event.target);
      if (idx >= this.numSuggestions) {
        this.selectedIndex = -1;
        this.selectedButtonIndex = idx - this.numSuggestions;
        return;
      }
      this.selectedIndex = idx;
    },

    _onMouseup(event) {
      if (event.button == 2) {
        return;
      }
      this._onCommand(event);
    },

    _onMouseout(event) {
      let idx = this._indexOfTableItem(event.originalTarget);
      if (idx >= this.numSuggestions) {
        this.selectedButtonIndex = -1;
      }
    },

    _onClick(event) {
      this._onMouseup(event);
    },

    _onContentSearchService(event) {
      let methodName = "_onMsg" + event.detail.type;
      if (methodName in this) {
        this[methodName](event.detail.data);
      }
    },

    _onMsgFocusInput() {
      this.input.focus();
    },

    _onMsgBlur() {
      this.input.blur();
      this._hideSuggestions();
    },

    _onMsgSuggestions(suggestions) {
      if (
        this._stickyInputValue != suggestions.searchString ||
        this.defaultEngine.name != suggestions.engineName
      ) {
        return;
      }

      this._clearSuggestionRows();

      let { left } = this.input.getBoundingClientRect();
      this._table.style.top = this.input.offsetHeight + "px";
      this._table.style.minWidth = this.input.offsetWidth + "px";
      this._table.style.maxWidth = window.innerWidth - left - 40 + "px";

      let searchWords = new Set(
        suggestions.searchString.trim().toLowerCase().split(/\s+/)
      );
      for (let i = 0; i < MAX_DISPLAYED_SUGGESTIONS; i++) {
        let type, idx;
        if (i < suggestions.formHistory.length) {
          [type, idx] = ["formHistory", i];
        } else {
          let j = i - suggestions.formHistory.length;
          if (j < suggestions.remote.length) {
            [type, idx] = ["remote", j];
          } else {
            break;
          }
        }
        this._suggestionsList.appendChild(
          this._makeTableRow(type, suggestions[type][idx], i, searchWords)
        );
      }

      if (this._table.hidden) {
        this.selectedIndex = -1;
        if (this._pendingOneOffRefresh) {
          this._setUpOneOffButtons();
          delete this._pendingOneOffRefresh;
        }
        this._currentEngineIndex = this._engines.findIndex(
          aEngine => aEngine.name == this.defaultEngine.name
        );
        this._table.hidden = false;
        this.input.setAttribute("aria-expanded", "true");
      }
    },

    _onMsgSuggestionsCancelled() {
      if (!this._table.hidden) {
        this._hideSuggestions();
      }
    },

    _onMsgState(state) {
      if ("isPrivateWindow" in state) {
        this._isPrivateEngine = state.isPrivateEngine;
      }

      this.engines = state.engines;

      let currentEngine = state.currentEngine;
      if (this._isPrivateEngine) {
        currentEngine = state.currentPrivateEngine;
      }

      if (
        this.defaultEngine &&
        this.defaultEngine.name == currentEngine.name &&
        this.defaultEngine.icon == currentEngine.icon
      ) {
        return;
      }
      this.defaultEngine = currentEngine;
    },

    _onMsgCurrentState(state) {
      this._onMsgState(state);
    },

    _onMsgCurrentEngine(engine) {
      if (this._isPrivateEngine) {
        return;
      }
      this.defaultEngine = engine;
      this._pendingOneOffRefresh = true;
    },

    _onMsgCurrentPrivateEngine(engine) {
      if (!this._isPrivateEngine) {
        return;
      }
      this.defaultEngine = engine;
      this._pendingOneOffRefresh = true;
    },

    _onMsgStrings(strings) {
      this._strings = strings;
      this._updateDefaultEngineHeader();
      this._updateSearchWithHeader();
      this._tableParent.querySelector(
        "#contentSearchSettingsButton"
      ).textContent = this._strings.searchSettings;
    },

    _updateDefaultEngineIcon() {
      let icon = this.defaultEngine.isConfigEngine
        ? this.defaultEngine.icon
        : "chrome://global/skin/icons/search-glass.svg";

      document.body.style.setProperty(
        "--newtab-search-icon",
        "url(" + icon + ")"
      );
    },

    _updateDefaultEngineHeader() {
      let header = this._tableParent.querySelector(
        "#contentSearchDefaultEngineHeader"
      );
      header.firstChild.setAttribute("src", this.defaultEngine.icon);
      if (!this._strings) {
        return;
      }
      while (header.firstChild.nextSibling) {
        header.firstChild.nextSibling.remove();
      }
      header.appendChild(
        document.createTextNode(
          this._strings.searchHeader.replace("%S", this.defaultEngine.name)
        )
      );
    },

    _updateSearchWithHeader() {
      if (!this._strings) {
        return;
      }
      let searchWithHeader = this._tableParent.querySelector(
        "#contentSearchSearchWithHeader"
      );
      let labels = searchWithHeader.querySelectorAll("label");
      if (this.input.value) {
        let header = this._strings.searchForSomethingWith2;
        header = header.replace("%1$S", "%S").split("%S");
        labels[0].textContent = header[0];
        labels[1].textContent = this.input.value;
        labels[2].textContent = header[1];
      } else {
        labels[0].textContent = this._strings.searchWithHeader;
        labels[1].textContent = "";
        labels[2].textContent = "";
      }
    },

    _speculativeConnect() {
      if (this.defaultEngine) {
        this._sendMsg("SpeculativeConnect", this.defaultEngine.name);
      }
    },

    _makeTableRow(type, suggestionStr, currentRow, searchWords) {
      let row = document.createElementNS(HTML_NS, "tr");
      row.dir = "auto";
      row.classList.add("contentSearchSuggestionRow");
      row.classList.add(type);
      row.setAttribute("role", "presentation");
      row.addEventListener("mousemove", this);
      row.addEventListener("mouseup", this);

      let entry = document.createElementNS(HTML_NS, "td");
      let img = document.createElementNS(HTML_NS, "div");
      img.setAttribute("class", "historyIcon");
      entry.appendChild(img);
      entry.classList.add("contentSearchSuggestionEntry");
      entry.setAttribute("role", "option");
      entry.id = this._idPrefix + SUGGESTION_ID_PREFIX + currentRow;
      entry.setAttribute("aria-selected", "false");

      let suggestionWords = suggestionStr.trim().toLowerCase().split(/\s+/);
      for (let i = 0; i < suggestionWords.length; i++) {
        let word = suggestionWords[i];
        let wordSpan = document.createElementNS(HTML_NS, "span");
        if (searchWords.has(word)) {
          wordSpan.classList.add("typed");
        }
        wordSpan.textContent = word;
        entry.appendChild(wordSpan);
        if (i < suggestionWords.length - 1) {
          entry.appendChild(document.createTextNode(" "));
        }
      }

      row.appendChild(entry);
      return row;
    },

    _getFaviconURIFromIconData(data) {
      if (typeof data == "string") {
        return data;
      }

      let blob = new Blob([data.icon], { type: data.mimeType });
      return URL.createObjectURL(blob);
    },

    _getImageURIForCurrentResolution(uri) {
      if (window.devicePixelRatio > 1) {
        return uri.replace(/\.png$/, "@2x.png");
      }
      return uri;
    },

    _getSearchEngines() {
      this._sendMsg("GetState");
    },

    _getStrings() {
      this._sendMsg("GetStrings");
    },

    _getSuggestions() {
      this._stickyInputValue = this.input.value;
      if (this.defaultEngine) {
        this._sendMsg("GetSuggestions", {
          engineName: this.defaultEngine.name,
          searchString: this.input.value,
        });
      }
    },

    _clearSuggestionRows() {
      while (this._suggestionsList.firstElementChild) {
        this._suggestionsList.firstElementChild.remove();
      }
    },

    _hideSuggestions() {
      this.input.setAttribute("aria-expanded", "false");
      this.selectedIndex = -1;
      this.selectedButtonIndex = -1;
      this._currentEngineIndex = -1;
      this._table.hidden = true;
    },

    _indexOfTableItem(elt) {
      if (elt.classList.contains("contentSearchOneOffItem")) {
        return this.numSuggestions + this._oneOffButtons.indexOf(elt);
      }
      if (elt.classList.contains("contentSearchSettingsButton")) {
        return this.numSuggestions + this._oneOffButtons.length;
      }
      while (elt && elt.localName != "tr") {
        elt = elt.parentNode;
      }
      if (!elt) {
        throw new Error("Element is not a row");
      }
      return elt.rowIndex;
    },

    _makeTable(id) {
      this._table = document.createElementNS(HTML_NS, "table");
      this._table.id = id;
      this._table.hidden = true;
      this._table.classList.add("contentSearchSuggestionTable");
      this._table.setAttribute("role", "presentation");

      this._table.addEventListener("mousedown", () => {
        this._mousedown = true;
      });
      document.addEventListener("mouseup", () => {
        delete this._mousedown;
      });

      this._table.addEventListener("mouseout", this);

      let headerRow = document.createElementNS(HTML_NS, "tr");
      let header = document.createElementNS(HTML_NS, "td");
      headerRow.setAttribute("class", "contentSearchHeaderRow");
      header.setAttribute("class", "contentSearchHeader");
      let iconImg = document.createElementNS(HTML_NS, "img");
      header.appendChild(iconImg);
      header.id = "contentSearchDefaultEngineHeader";
      headerRow.appendChild(header);
      headerRow.addEventListener("click", this);
      this._table.appendChild(headerRow);

      let row = document.createElementNS(HTML_NS, "tr");
      row.setAttribute("class", "contentSearchSuggestionsContainer");
      let cell = document.createElementNS(HTML_NS, "td");
      cell.setAttribute("class", "contentSearchSuggestionsContainer");
      this._suggestionsList = document.createElementNS(HTML_NS, "table");
      this._suggestionsList.setAttribute(
        "class",
        "contentSearchSuggestionsList"
      );
      cell.appendChild(this._suggestionsList);
      row.appendChild(cell);
      this._table.appendChild(row);
      this._suggestionsList.setAttribute("role", "listbox");

      this._oneOffsTable = document.createElementNS(HTML_NS, "table");
      this._oneOffsTable.setAttribute("class", "contentSearchOneOffsTable");
      this._oneOffsTable.classList.add("contentSearchSuggestionsContainer");
      this._oneOffsTable.setAttribute("role", "group");
      this._table.appendChild(this._oneOffsTable);

      headerRow = document.createElementNS(HTML_NS, "tr");
      header = document.createElementNS(HTML_NS, "td");
      headerRow.setAttribute("class", "contentSearchHeaderRow");
      header.setAttribute("class", "contentSearchHeader");
      headerRow.appendChild(header);
      header.id = "contentSearchSearchWithHeader";
      let start = document.createElement("label");
      let inputLabel = document.createElement("label");
      inputLabel.setAttribute(
        "class",
        "contentSearchSearchWithHeaderSearchText"
      );
      let end = document.createElement("label");
      header.appendChild(start);
      header.appendChild(inputLabel);
      header.appendChild(end);
      this._oneOffsTable.appendChild(headerRow);

      let button = document.createElementNS(HTML_NS, "button");
      button.setAttribute("class", "contentSearchSettingsButton");
      button.classList.add("contentSearchHeaderRow");
      button.classList.add("contentSearchHeader");
      button.id = "contentSearchSettingsButton";
      button.addEventListener("click", this);
      button.addEventListener("mousemove", this);
      this._table.appendChild(button);

      return this._table;
    },

    _setUpOneOffButtons() {
      if (!this._engines) {
        return;
      }

      while (this._oneOffsTable.firstChild.nextSibling) {
        this._oneOffsTable.firstChild.nextSibling.remove();
      }

      this._oneOffButtons = [];

      let engines = this._engines
        .filter(aEngine => aEngine.name != this.defaultEngine.name)
        .filter(aEngine => !aEngine.hidden);
      if (!engines.length) {
        this._oneOffsTable.hidden = true;
        return;
      }

      const kDefaultButtonWidth = 49; 
      let rowWidth = this.input.offsetWidth - 2; 
      let enginesPerRow = Math.floor(rowWidth / kDefaultButtonWidth);
      let buttonWidth = Math.floor(rowWidth / enginesPerRow);

      let row = document.createElementNS(HTML_NS, "tr");
      let cell = document.createElementNS(HTML_NS, "td");
      row.setAttribute("class", "contentSearchSuggestionsContainer");
      cell.setAttribute("class", "contentSearchSuggestionsContainer");

      for (let i = 0; i < engines.length; ++i) {
        let engine = engines[i];
        if (i > 0 && i % enginesPerRow == 0) {
          row.appendChild(cell);
          this._oneOffsTable.appendChild(row);
          row = document.createElementNS(HTML_NS, "tr");
          cell = document.createElementNS(HTML_NS, "td");
          row.setAttribute("class", "contentSearchSuggestionsContainer");
          cell.setAttribute("class", "contentSearchSuggestionsContainer");
        }
        let button = document.createElementNS(HTML_NS, "button");
        button.setAttribute("class", "contentSearchOneOffItem");
        let img = document.createElementNS(HTML_NS, "img");
        let uri;
        if (engine.iconData) {
          uri = this._getFaviconURIFromIconData(engine.iconData);
        } else {
          uri = this._getImageURIForCurrentResolution(
            "chrome://browser/skin/search-engine-placeholder.png"
          );
        }
        img.setAttribute("src", uri);
        img.addEventListener(
          "load",
          function () {
            URL.revokeObjectURL(uri);
          },
          { once: true }
        );
        button.appendChild(img);
        button.style.width = buttonWidth + "px";
        button.setAttribute("engine-name", engine.name);

        button.engineName = engine.name;
        button.addEventListener("click", this);
        button.addEventListener("mousemove", this);
        button.setAttribute("aria-label", engine.name);

        if (engines.length - i <= enginesPerRow - (i % enginesPerRow)) {
          button.classList.add("last-row");
        }

        if ((i + 1) % enginesPerRow == 0) {
          button.classList.add("end-of-row");
        }

        button.id = ONE_OFF_ID_PREFIX + i;
        cell.appendChild(button);
        this._oneOffButtons.push(button);
      }
      row.appendChild(cell);
      this._oneOffsTable.appendChild(row);
      this._oneOffsTable.hidden = false;
    },

    _sendMsg(type, data = null) {
      dispatchEvent(
        new CustomEvent("ContentSearchClient", {
          detail: {
            type,
            data,
          },
        })
      );
    },
  };

  return ContentSearchUIController;
})();

class ContentSearchHandoffUI extends MozLitElement {
  static queries = {
    fakeCaret: ".fake-caret",
    nonHandoffSearchInput: "#newtab-search-text",
  };

  static properties = {
    fakeFocus: { type: Boolean, reflect: true },
    disabled: { type: Boolean, reflect: true },
    nonHandoff: { type: String, reflect: true },
  };

  #controller = null;

  constructor() {
    super();
    this.fakeFocus = false;
    this.disabled = false;
    this.nonHandoff = "";
  }

  get nonHandoffMode() {
    return this.nonHandoff === "true";
  }

  #doSearchHandoff(text = "") {
    this.fakeFocus = true;
    this.#controller.doSearchHandoff(text);
  }

  #onSearchHandoffClick(event) {
    event.preventDefault();
    this.#doSearchHandoff();
  }

  #onSearchHandoffPaste(event) {
    event.preventDefault();
    this.#doSearchHandoff(event.clipboardData.getData("Text"));
  }

  #onSearchHandoffDrop(event) {
    event.preventDefault();
    let text = event.dataTransfer.getData("text");
    if (text) {
      this.#doSearchHandoff(text);
    }
  }

  firstUpdated() {
    if (!this.#controller) {
      if (this.nonHandoffMode) {
        const isNewTab =
          globalThis.document &&
          globalThis.document.documentURI === "about:newtab";
        const healthReportKey = isNewTab ? "newtab" : "abouthome";
        this.#controller = new window.ContentSearchUIController(
          this.nonHandoffSearchInput,
          this.nonHandoffSearchInput.parentElement,
          healthReportKey
        );
      } else {
        this.#controller = new window.ContentSearchHandoffUIController(this);
      }
    }
  }

  render() {
    return html`
      <link
        rel="stylesheet"
        href="chrome://browser/content/contentSearchHandoffUI.css"
      />
      ${this.nonHandoffMode
        ? this.#nonHandoffTemplate()
        : this.#handoffTemplate()}
    `;
  }

  #onNonHandoffSearchClick(event) {
    this.#controller.search(event);
  }

  #nonHandoffTemplate() {
    return html`
      <div class="non-handoff-container">
        <input
          id="newtab-search-text"
          data-l10n-id="newtab-search-box-input"
          maxlength="256"
          type="search"
        />
        <button
          id="searchSubmit"
          class="search-button"
          data-l10n-id="newtab-search-box-search-button"
          @click=${this.#onNonHandoffSearchClick}
        ></button>
      </div>
    `;
  }

  #handoffTemplate() {
    return html`<button
      class="search-handoff-button"
      @click=${this.#onSearchHandoffClick}
      tabindex="-1"
    >
      <div class="fake-textbox"></div>
      <input
        type="search"
        class="fake-editable"
        tabindex="-1"
        aria-hidden="true"
        @drop=${this.#onSearchHandoffDrop}
        @paste=${this.#onSearchHandoffPaste}
      />
      <div class="fake-caret"></div>
    </button>`;
  }
}

customElements.define("content-search-handoff-ui", ContentSearchHandoffUI);
