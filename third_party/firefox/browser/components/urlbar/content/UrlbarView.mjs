/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { UrlbarShared } from "chrome://browser/content/urlbar/UrlbarShared.mjs";
import { L10nCache } from "chrome://browser/content/urlbar/L10nCache.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ContextualIdentityService:
    "moz-src:///toolkit/components/contextualidentity/ContextualIdentityService.sys.mjs",
  ObjectUtils: "resource://gre/modules/ObjectUtils.sys.mjs",
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  UrlbarProviderOpenTabs:
    "moz-src:///browser/components/urlbar/UrlbarProviderOpenTabs.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarSearchOneOffs:
    "moz-src:///browser/components/urlbar/UrlbarSearchOneOffs.sys.mjs",
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
});

const SELECTABLE_ELEMENT_SELECTOR = "[role=button], [selectable], a";
const KEYBOARD_SELECTABLE_ELEMENT_SELECTOR =
  "[role=button]:not([keyboard-inaccessible]), [selectable], a";

const RESULT_MENU_COMMANDS = {
  DISMISS: "dismiss",
  HELP: "help",
  MANAGE: "manage",
};

const getBoundsWithoutFlushing = element =>
  element.documentGlobal.windowUtils.getBoundsWithoutFlushing(element);

let gUniqueIdSerial = 1;
function getUniqueId(prefix) {
  return prefix + (gUniqueIdSerial++ % 9999);
}

export class UrlbarView {
  constructor(input) {
    this.input = input;
    this.panel = input.panel;
    this.controller = input.controller;
    this.document = this.panel.ownerDocument;
    this.window = this.document.defaultView;

    this.#rows = this.panel.querySelector(".urlbarView-results");
    this.resultMenu = this.panel.querySelector(".urlbarView-result-menu");
    this.#resultMenuCommands = new WeakMap();

    this.#rows.addEventListener("mousedown", this);

    this.#rows.addEventListener("overflow", this);
    this.#rows.addEventListener("underflow", this);

    this.resultMenu.addEventListener("click", this);
    this.resultMenu.addEventListener("showing", this);

    this.input.toggleAttribute("noresults", true);

    this.controller.setView(this);
    this.controller.addListener(this);
    this.queryContextCache = new QueryContextCache(5);

    this.#l10nCache = new L10nCache();

    this.input.addEventListener("contextmenu", this);
  }

  get oneOffSearchButtons() {
    if (this.input.sapName != "urlbar") {
      return null;
    }
    if (!this.#oneOffSearchButtons) {
      this.#oneOffSearchButtons = new lazy.UrlbarSearchOneOffs(this);
      this.#oneOffSearchButtons.addEventListener(
        "SelectedOneOffButtonChanged",
        this
      );
    }
    return this.#oneOffSearchButtons;
  }

  get chromeWindow() {
    return this.input.window;
  }

  get isOpen() {
    return this.input.hasAttribute("open");
  }

  get queryContext() {
    return this.#queryContext;
  }

  get allowEmptySelection() {
    let { heuristicResult } = this.#queryContext || {};
    return !heuristicResult || !this.#shouldShowHeuristic(heuristicResult);
  }

  get selectedRowIndex() {
    if (!this.isOpen) {
      return -1;
    }

    let selectedRow = this.#getSelectedRow();

    if (!selectedRow) {
      return -1;
    }

    return selectedRow.result.rowIndex;
  }

  set selectedRowIndex(val) {
    if (!this.isOpen) {
      throw new Error(
        "UrlbarView: Cannot select an item if the view isn't open."
      );
    }

    if (val < 0) {
      this.#selectElement(null);
      return;
    }

    let items = Array.from(this.#rows.children).filter(r =>
      this.#isElementVisible(r)
    );
    if (val >= items.length) {
      throw new Error(`UrlbarView: Index ${val} is out of bounds.`);
    }

    let row = items[val];
    let element = this.#getNextSelectableElement(row);
    if (this.#getRowFromElement(element) != row) {
      element = null;
    }

    this.#selectElement(element);
  }

  get selectedElementIndex() {
    if (!this.isOpen || !this.#selectedElement) {
      return -1;
    }

    return this.#selectedElement.elementIndex;
  }

  get selectedResult() {
    if (!this.isOpen) {
      return null;
    }

    return this.#getSelectedRow()?.result;
  }

  get selectedElement() {
    if (!this.isOpen) {
      return null;
    }

    return this.#selectedElement;
  }

  shouldSpaceActivateSelectedElement() {
    if (this.selectedElement?.dataset.name == "result-menu") {
      return true;
    }

    if (this.selectedElement?.getAttribute("role") != "button") {
      return false;
    }

    if (this.input.value) {
      return false;
    }
    return true;
  }

  clearSelection() {
    this.#selectElement(null, { updateInput: false });
  }

  get visibleRowCount() {
    let sum = 0;
    for (let row of this.#rows.children) {
      sum += Number(this.#isElementVisible(row));
    }
    return sum;
  }

  getResultFromElement(element) {
    return element?.classList.contains("urlbarView-result-menuitem")
      ? this.#resultMenuResult
      : this.#getRowFromElement(element)?.result;
  }



  getResultAtIndex(index) {
    if (
      !this.isOpen ||
      !this.#rows.children.length ||
      index >= this.#rows.children.length
    ) {
      return null;
    }

    return this.#rows.children[index].result;
  }

  resultIsSelected(result) {
    if (this.selectedRowIndex < 0) {
      return false;
    }

    return result.rowIndex == this.selectedRowIndex;
  }

  selectBy(amount, { reverse = false, userPressedTab = false } = {}) {
    if (!this.isOpen) {
      throw new Error(
        "UrlbarView: Cannot select an item if the view isn't open."
      );
    }

    if (!this.input.eventBufferer.isDeferringEvents) {
      this.controller.cancelQuery();
    }

    if (!userPressedTab) {
      let { selectedRowIndex } = this;
      let end = this.visibleRowCount - 1;
      if (selectedRowIndex == -1) {
        this.selectedRowIndex = reverse ? end : 0;
        return;
      }
      let endReached = selectedRowIndex == (reverse ? 0 : end);
      if (endReached) {
        if (this.allowEmptySelection) {
          this.#selectElement(null);
        } else {
          this.selectedRowIndex = reverse ? end : 0;
        }
        return;
      }

      let index = Math.min(end, selectedRowIndex + amount * (reverse ? -1 : 1));
      if (
        this.#rows.children[index]?.result.providerName ==
          "UrlbarProviderGlobalActions" &&
        this.#rows.children.length > 2
      ) {
        index = index + (reverse ? -1 : 1);
      }
      this.selectedRowIndex = Math.max(0, index);
      return;
    }


    let selectedElement = this.#selectedElement;

    let firstSelectableElement = this.getFirstSelectableElement();
    let lastSelectableElement = this.getLastSelectableElement();

    if (!selectedElement) {
      selectedElement = reverse
        ? lastSelectableElement
        : firstSelectableElement;
      this.#selectElement(selectedElement, {
        setAccessibleFocus: true,
      });
      return;
    }
    let endReached = reverse
      ? selectedElement == firstSelectableElement
      : selectedElement == lastSelectableElement;
    if (endReached) {
      if (this.allowEmptySelection) {
        selectedElement = null;
      } else {
        selectedElement = reverse
          ? lastSelectableElement
          : firstSelectableElement;
      }
      this.#selectElement(selectedElement, {
        setAccessibleFocus: true,
      });
      return;
    }

    while (amount-- > 0) {
      let next = reverse
        ? this.#getPreviousSelectableElement(selectedElement)
        : this.#getNextSelectableElement(selectedElement);
      if (!next) {
        break;
      }
      selectedElement = next;
    }
    this.#selectElement(selectedElement, {
      setAccessibleFocus: true,
    });
  }

  async acknowledgeFeedback(result) {
    let row = this.#rows.children[result.rowIndex];
    if (!row) {
      return;
    }

    let l10n = { id: "urlbar-feedback-acknowledgment" };
    await this.#l10nCache.ensure(l10n);
    if (row.result != result) {
      return;
    }

    let { value } = this.#l10nCache.get(l10n);
    row.setAttribute("feedback-acknowledgment", value);
    row._content.closest("[role=option]").ariaNotify(value);
  }

  #acknowledgeDismissal(result, titleL10n) {
    let row = this.#rows.children[result.rowIndex];
    if (!row || row.result != result) {
      return;
    }

    let isSelected = this.#getSelectedRow() == row;
    if (isSelected) {
      this.#selectElement(null, { updateInput: false });
    }
    this.#setRowSelectable(row, false);

    let tip = new lazy.UrlbarResult({
      type: UrlbarShared.RESULT_TYPE.TIP,
      source: UrlbarShared.RESULT_SOURCE.OTHER_LOCAL,
      payload: {
        type: "dismissalAcknowledgment",
        titleL10n,
        buttons: [{ l10n: { id: "urlbar-search-tips-confirm-short" } }],
        icon: "chrome://branding/content/icon32.png",
      },
      rowLabel: !result.hideRowLabel && this.#rowLabel(row),
      hideRowLabel: result.hideRowLabel,
      richSuggestionIconSize: 32,
    });
    this.#updateRow(row, tip);
    this.#updateIndices();

    if (isSelected) {
      this.#selectElement(this.#getNextSelectableElement(row), {
        updateInput: false,
      });
    }
  }

  removeAccessibleFocus() {
    this.#setAccessibleFocus(null);
  }

  clear() {
    this.#rows.textContent = "";
    this.input.toggleAttribute("noresults", true);
    this.clearSelection();
    this.visibleResults = [];
  }

  close({ elementPicked = false, showFocusBorder = true } = {}) {
    const isShowingZeroPrefix =
      this.#queryContext && !this.#queryContext.searchString;
    this.controller.cancelQuery();
    if (!elementPicked && showFocusBorder) {
      this.input.removeAttribute("suppress-focus-border");
    }

    if (!this.isOpen) {
      this.input.updateLayoutExtend();
      return;
    }

    this.#stopTail150();

    this.#containerWidthOnLastClose = getBoundsWithoutFlushing(
      this.input.parentElement
    ).width;

    if (this.input.searchMode?.isPreview) {
      this.input.searchMode = null;
      this.chromeWindow.gBrowser.userTypedValue = null;
    }

    this.resultMenu.hide?.();
    this.removeAccessibleFocus();
    this.input.inputField.setAttribute("aria-expanded", "false");
    this.#openPanelInstance = null;

    this.input.toggleAttribute("open", false);


    this.window.removeEventListener("resize", this);
    this.window.removeEventListener("blur", this);

    this.controller.notify(UrlbarShared.NOTIFICATIONS.VIEW_CLOSE);

    if (this.#blobUrlsByResultUrl) {
      for (let blobUrl of this.#blobUrlsByResultUrl.values()) {
        URL.revokeObjectURL(blobUrl);
      }
      this.#blobUrlsByResultUrl.clear();
    }

    if (isShowingZeroPrefix) {
      if (elementPicked) {
      } else {
      }
    }
  }

  startTail150() {
    if (this.#tail150) {
      return;
    }

    let doc = this.document;
    let ns = "http://www.w3.org/1999/xhtml";
    let overlay = doc.createElementNS(ns, "div");
    overlay.className = "urlbarView-tail150-overlay";

    let closeBtn = doc.createElementNS(ns, "div");
    closeBtn.className = "close-button";
    closeBtn.setAttribute("role", "button");
    closeBtn.addEventListener("click", () => this.close());

    let canvas = doc.createElementNS(ns, "canvas");
    let dpr = this.window.devicePixelRatio || 1;
    canvas.width = 400 * dpr;
    canvas.height = 400 * dpr;
    canvas.getContext("2d").scale(dpr, dpr);
    canvas.className = "urlbarView-tail150-canvas";
    overlay.append(closeBtn, canvas);

    this.input.appendChild(overlay);
    this.#tail150 = { overlay, keyHandler: null };
    this.#runTail150(canvas);
  }

  #stopTail150() {
    if (!this.#tail150) {
      return;
    }
    if (this.#tail150.keyHandler) {
      this.window.removeEventListener(
        "keydown",
        this.#tail150.keyHandler,
        true
      );
    }
    this.#tail150.overlay.remove();
    this.#tail150 = null;
  }

  #runTail150(canvas) {
    let S = this.window.getComputedStyle(canvas);
    let AC = S.getPropertyValue("--color-gray-0");
    let FD = S.getPropertyValue("--color-yellow-30");
    let SP = new this.window.Image();
    SP.src = "chrome://branding/content/icon48.png";
    // prettier-ignore
    (() => { let c=canvas,W=this.window,A=t=>W.requestAnimationFrame(t),X=c.getContext("2d"),CA=(x,y,r)=>{X.beginPath();X.arc(x,y,r,0,7);X.fill()},g=()=>20*Math.random()|0,V=[,[-1,0],[0,-1],[1,0],[0,1]],s,d,n,f,e,r=0,l=0,GO=m=>{r=0,X.shadowColor="#000",X.shadowBlur=8,X.fillStyle=AC,X.fillText(m,200,180),X.fillText(e,200,230)},PF=()=>{let a=[];for(let x=0;x<20;x++)for(let y=0;y<20;y++)s.every($=>$.x!=x||$.y!=y)&&a.push({x,y});a.length?f=a[a.length*Math.random()|0]:GO("GG")},I=()=>{s=[...Array(8)].map(($,t)=>({x:10-t,y:10})),d=n=V[3],e=0,f={x:15,y:15},r=1,A(L)},L=$=>{if(!this.#tail150||!r)return;A(L);let p=($-l)/100;if(p>=1){l=$,d=n,p=0;let t={x:s[0].x+d[0],y:s[0].y+d[1]};if(t.x<0||t.x>19||t.y<0||t.y>19||s.some($=>$.x==t.x&&$.y==t.y)){GO("GAME OVER");return}s.unshift(t),t.x==f.x&&t.y==f.y?(e++,PF()):s.pop();if(!r)return}X.clearRect(0,0,400,400),X.fillStyle=FD,CA(20*f.x+10,20*f.y+10,6),s.map(($,t)=>{if(X.save(),X.translate(Math.min(390,Math.max(10,20*($.x+(!t&&d[0]*p))+10)),Math.min(390,Math.max(10,20*($.y+(!t&&d[1]*p))+10))),t){let i=t/s.length;X.fillStyle=`oklch(${62+17*i}% ${.21-.01*i} ${90*i})`,CA(0,0,8)}else SP.complete&&X.drawImage(SP,-10,-10,20,20);X.restore()})};X.fillStyle=AC;X.textAlign="center";X.font="30px Arial";X.fillText("← ↑ ↓ →",200,200);W.addEventListener("keydown",this.#tail150.keyHandler=$=>{$.keyCode!=27&&($.preventDefault(),$.stopPropagation());let t=V[$.keyCode-36];!r&&t&&I(),t&&(t[0]!=-d[0]||t[1]!=-d[1])&&(n=t)},true); })(); // eslint-disable-line
  }

  autoOpen({ event, suppressFocusBorder = true }) {
    if (!event) {
      return false;
    }
    if (this.input.readOnly) {
      return false;
    }
    if (this.#pickSearchTipIfPresent(event)) {
      return false;
    }
    if (this.input.inOverflowPanel) {
      return false;
    }

    let queryOptions = { event };
    if (
      !this.input.value ||
      this.input.getAttribute("pageproxystate") == "valid"
    ) {
      if (!this.isOpen && ["mousedown", "command"].includes(event.type)) {
        this.input.startQuery(queryOptions);
        if (suppressFocusBorder) {
          this.input.toggleAttribute("suppress-focus-border", true);
        }
        return true;
      }
      return false;
    }

    if (!this.input.focused) {
      return false;
    }

    if (this.isOpen && event.type != "tabswitch") {
      return false;
    }

    if (
      this.#rows.firstElementChild &&
      this.#queryContext.searchString == this.input.value &&
      this.#containerWidthOnLastClose ==
        getBoundsWithoutFlushing(this.input.parentElement).width
    ) {
      queryOptions.allowAutofill = this.#queryContext.allowAutofill;
    } else {
      let cachedQueryContext = this.queryContextCache.get(this.input.value);
      if (cachedQueryContext) {
        this.onQueryResults(cachedQueryContext);
      }
    }

    let state = this.input.getBrowserState(
      this.chromeWindow.gBrowser.selectedBrowser
    );
    if (state.persist?.shouldPersist) {
      queryOptions.allowAutofill = false;
    }

    queryOptions.searchString = this.input.value;
    queryOptions.autofillIgnoresSelection = true;
    queryOptions.interactionType = "returned";

    if (
      this.#queryContext?.results?.length &&
      this.#queryContext.searchString == this.input.value &&
      this.#queryContext.results[0].type != UrlbarShared.RESULT_TYPE.TIP
    ) {
      this.#openPanel();
    }

    this.input.startQuery(queryOptions);
    if (suppressFocusBorder) {
      this.input.toggleAttribute("suppress-focus-border", true);
    }
    return true;
  }

  onQueryStarted(queryContext) {
    this.#queryWasCancelled = false;
    this.#queryUpdatedResults = false;
    this.#openPanelInstance = null;
    this.#startRemoveStaleRowsTimer();

    this.#cacheL10nStrings();
  }

  onQueryCancelled() {
    this.#queryWasCancelled = true;
    this.#cancelRemoveStaleRowsTimer();
  }

  onQueryFinished(queryContext) {
    this.#cancelRemoveStaleRowsTimer();
    if (this.#queryWasCancelled) {
      return;
    }

    if (this.#queryUpdatedResults) {
      this.#removeStaleRows();
    } else {
      this.clear();
    }


    if (this.#queryUpdatedResults) {
      return;
    }

    if (!this.input.searchMode) {
      this.close();
      return;
    }

    let openPanelInstance = (this.#openPanelInstance = {});
    this.oneOffSearchButtons?.willHide().then(willHide => {
      if (!willHide && openPanelInstance == this.#openPanelInstance) {
        this.oneOffSearchButtons.enable(true);
        this.#openPanel();
      }
    });
  }

  onQueryResults(queryContext) {
    this.queryContextCache.put(queryContext);
    this.#queryContext = queryContext;

    if (!this.isOpen) {
      this.clear();
    }

    if (this.input.searchMode?.source == UrlbarShared.RESULT_SOURCE.ACTIONS) {
      this.#rows.toggleAttribute("actionmode", true);
    }

    this.#queryUpdatedResults = true;
    this.#updateResults();

    let firstResult = queryContext.results[0];

    if (queryContext.lastResultCount == 0) {
      this.#selectElement(null, {
        updateInput: false,
      });

      this.oneOffSearchButtons?.enable(
        queryContext.trimmedSearchString[0] != "@" &&
          (queryContext.trimmedSearchString[0] !=
            UrlbarShared.RESTRICT_TOKENS.SEARCH ||
            queryContext.trimmedSearchString.length != 1)
      );
    }

    if (!this.#selectedElement && !this.oneOffSearchButtons?.selectedButton) {
      if (firstResult.heuristic) {
        if (this.#shouldShowHeuristic(firstResult)) {
          this.#selectElement(this.getFirstSelectableElement(), {
            updateInput: false,
            setAccessibleFocus:
              this.controller.userSelectionBehavior == "arrow",
          });
        } else {
          this.input.setResultForCurrentValue(firstResult);
        }
      } else if (
        firstResult.payload.providesSearchMode &&
        queryContext.trimmedSearchString != "@"
      ) {
        this.input.setResultForCurrentValue(firstResult);
      }
    }

    if (this.#selectedElement && !this.oneOffSearchButtons?.selectedButton) {
      let aadID = this.input.inputField.getAttribute("aria-activedescendant");
      if (aadID && !this.document.getElementById(aadID)) {
        this.#setAccessibleFocus(this.#selectedElement);
      }
    }

    this.#openPanel();

    if (firstResult.heuristic) {
      this.input.formatValue();
    }

    if (queryContext.deferUserSelectionProviders.size) {
      queryContext.results.forEach(r => {
        queryContext.deferUserSelectionProviders.delete(r.providerName);
      });
    }

    if (lazy.UrlbarPrefs.get("unifiedSearchButton.always")) {
      this.input.searchModeSwitcher?.updateSearchIcon();
    }
  }

  onQueryResultRemoved(index) {
    let rowToRemove = this.#rows.children[index];

    let { result } = rowToRemove;
    if (result.acknowledgeDismissalL10n) {
      this.#acknowledgeDismissal(result, result.acknowledgeDismissalL10n);
      return;
    }

    let updateSelection = rowToRemove == this.#getSelectedRow();
    rowToRemove.remove();
    this.#updateIndices();

    if (!updateSelection) {
      return;
    }
    let newSelectionIndex = index;
    if (index >= this.#queryContext.results.length) {
      newSelectionIndex = this.#queryContext.results.length - 1;
    }
    if (newSelectionIndex >= 0) {
      this.selectedRowIndex = newSelectionIndex;
    }
  }

  openResultMenu(result, anchor) {
    this.#resultMenuResult = result;
    let event = new CustomEvent("ResultMenuTriggered", {
      detail: { target: anchor },
    });
    this.resultMenu.toggle(event, anchor);
  }

  invalidateResultMenuCommands() {
    this.#resultMenuCommands = new WeakMap();
  }

  handleEvent(event) {
    let methodName = "on_" + event.type;
    if (methodName in this) {
      this[methodName](event);
    } else {
      throw new Error("Unrecognized UrlbarView event: " + event.type);
    }
  }

  #blobUrlsByResultUrl = null;
  #contextMenu;
  #containerWidthOnLastClose = 0;
  #l10nCache;
  #mousedownSelectedElement;
  #openPanelInstance;
  #oneOffSearchButtons;
  #queryContext;
  #queryUpdatedResults;
  #queryWasCancelled;
  #removeStaleRowsTimer;
  #resultMenuResult;
  #resultMenuCommands;
  #rows;
  #rawSelectedElement;
  #tail150 = null;

  get #selectedElement() {
    return this.#rawSelectedElement?.isConnected
      ? this.#rawSelectedElement
      : null;
  }

  #createElement(tag) {
    return this.document.createElementNS("http://www.w3.org/1999/xhtml", tag);
  }

  #openPanel() {
    if (this.input.isSidebarMode) {
      return;
    }
    if (this.isOpen) {
      this.input.updateLayoutExtend();
      return;
    }
    this.controller.userSelectionBehavior = "none";

    this.panel.removeAttribute("action-override");

    this.#enableOrDisableRowWrap();

    this.input.inputField.setAttribute("aria-expanded", "true");

    this.input.toggleAttribute("suppress-focus-border", true);
    this.input.toggleAttribute("open", true);

    this.window.addEventListener("resize", this);
    this.window.addEventListener("blur", this);

    this.controller.notify(UrlbarShared.NOTIFICATIONS.VIEW_OPEN);

    this.maybeRollupPopups();
  }

  maybeRollupPopups() {
    if (
      lazy.UrlbarPrefs.get("closeOtherPanelsOnOpen") &&
      !this.input.inOverflowPanel
    ) {
      this.window.docShell.treeOwner
        .QueryInterface(Ci.nsIInterfaceRequestor)
        .getInterface(Ci.nsIAppWindow)
        .rollupAllPopups();
    }
  }

  #shouldShowHeuristic(result) {
    if (!result?.heuristic) {
      throw new Error("A heuristic result must be given");
    }
    return (
      !lazy.UrlbarPrefs.get("experimental.hideHeuristic") ||
      result.type == UrlbarShared.RESULT_TYPE.TIP
    );
  }

  #rowCanUpdateToResult(rowIndex, result) {
    if (result.heuristic) {
      return true;
    }
    let row = this.#rows.children[rowIndex];
    if (result.hasSuggestedIndex != row.result.hasSuggestedIndex) {
      return false;
    }
    if (
      result.hasSuggestedIndex &&
      result.suggestedIndex != row.result.suggestedIndex
    ) {
      return false;
    }
    if (result.providerName != row.result.providerName) {
      return false;
    }
    return true;
  }

  #updateResults() {

    let results = this.#queryContext.results;
    if (results[0]?.heuristic && !this.#shouldShowHeuristic(results[0])) {
      results = results.slice(1);
    }
    let rowIndex = 0;
    let resultsToInsert = results.slice();
    let visibleSpanCount = 0;
    let seenMisplacedResult = false;

    while (rowIndex < this.#rows.children.length && resultsToInsert.length) {
      let row = this.#rows.children[rowIndex];
      if (this.#isElementVisible(row)) {
        visibleSpanCount += lazy.UrlbarUtils.getSpanForResult(row.result);
      }

      if (!seenMisplacedResult) {
        let result = resultsToInsert[0];
        if (this.#rowCanUpdateToResult(rowIndex, result)) {
          resultsToInsert.shift();

          this.#updateRow(row, result);
          rowIndex++;
          continue;
        }

        if (result.hasSuggestedIndex || row.result.hasSuggestedIndex) {
          seenMisplacedResult = true;
        }
      }

      row.setAttribute("stale", "true");
      rowIndex++;
    }

    for (; rowIndex < this.#rows.children.length; ++rowIndex) {
      let row = this.#rows.children[rowIndex];
      row.setAttribute("stale", "true");
      if (this.#isElementVisible(row)) {
        visibleSpanCount += lazy.UrlbarUtils.getSpanForResult(row.result);
      }
    }

    for (let result of resultsToInsert) {
      if (!seenMisplacedResult && result.hasSuggestedIndex) {
        if (result.isSuggestedIndexRelativeToGroup) {
          seenMisplacedResult = true;
        } else {
          let finalIndex =
            result.suggestedIndex >= 0
              ? Math.min(results.length - 1, result.suggestedIndex)
              : Math.max(0, results.length + result.suggestedIndex);
          if (this.#rows.children.length != finalIndex) {
            seenMisplacedResult = true;
          }
        }
      }
      let newSpanCount =
        visibleSpanCount + lazy.UrlbarUtils.getSpanForResult(result);
      let canBeVisible =
        newSpanCount <= this.#queryContext.maxResults && !seenMisplacedResult;
      let row = this.#createRow();
      this.#updateRow(row, result);
      if (canBeVisible) {
        visibleSpanCount = newSpanCount;
      } else {
        this.#setRowVisibility(row, false);
      }
      this.#rows.appendChild(row);
    }

    this.#updateIndices();
  }

  #createRow() {
    let item = this.#createElement("div");
    item.className = "urlbarView-row";
    item._elements = new Map();
    item._buttons = new Map();

    item.setAttribute("role", "presentation");

    item._sharedAttributes = new Set(
      [...item.attributes].map(v => v.name).concat(["stale", "id", "hidden"])
    );
    item._sharedClassList = new Set(item.classList);

    return item;
  }

  #createRowContent(item) {
    let noWrap = this.#createElement("span");
    noWrap.className = "urlbarView-no-wrap";
    item._content.appendChild(noWrap);

    let favicon = this.#createElement("img");
    favicon.className = "urlbarView-favicon";
    noWrap.appendChild(favicon);
    item._elements.set("favicon", favicon);

    let typeIcon = this.#createElement("span");
    typeIcon.className = "urlbarView-type-icon";
    noWrap.appendChild(typeIcon);

    let title = this.#createElement("span");
    title.classList.add("urlbarView-title", "urlbarView-overflowable");
    noWrap.appendChild(title);
    item._elements.set("title", title);

    let tagsContainer = this.#createElement("span");
    tagsContainer.classList.add("urlbarView-tags", "urlbarView-overflowable");
    noWrap.appendChild(tagsContainer);
    item._elements.set("tagsContainer", tagsContainer);

    let titleSeparator = this.#createElement("span");
    titleSeparator.className = "urlbarView-title-separator";
    noWrap.appendChild(titleSeparator);
    item._elements.set("titleSeparator", titleSeparator);

    let action = this.#createElement("span");
    action.className = "urlbarView-action";
    noWrap.appendChild(action);
    item._elements.set("action", action);

    let url = this.#createElement("span");
    url.className = "urlbarView-url";
    item._content.appendChild(url);
    item._elements.set("url", url);

    this.#createExplanation(item._content, item);
  }

  #createExplanation(parentNode, item) {
    if (!lazy.UrlbarPrefs.get("resultExplanationsFeatureGate")) {
      return;
    }

    let explanation = this.#createElement("span");
    explanation.classList.add(
      "urlbarView-explanation",
      "urlbarView-overflowable"
    );
    parentNode.appendChild(explanation);
    item._elements.set("explanation", explanation);

    let bookmarked = this.#createElement("span");
    bookmarked.className = "urlbarView-explanation-bookmarked";
    explanation.appendChild(bookmarked);
    item._elements.set("explanationBookmarked", bookmarked);

    let lastVisited = this.#createElement("span");
    lastVisited.className = "urlbarView-explanation-last-visited";
    explanation.appendChild(lastVisited);
    item._elements.set("explanationLastVisited", lastVisited);
  }

  #updateExplanation(item, result, setURL) {
    let explanation = item._elements.get("explanation");
    if (!explanation) {
      return;
    }

    let bookmarked = item._elements.get("explanationBookmarked");
    let hasBookmark = setURL && !!result.payload.bookmarkDateMs;
    if (hasBookmark) {
      let { formattedDate } = lazy.UrlbarUtils.formatDate(
        new Date(result.payload.bookmarkDateMs),
        { forceAbsoluteDate: true }
      );
      this.document.l10n.setAttributes(
        bookmarked,
        "urlbar-result-explanation-bookmarked",
        { date: formattedDate }
      );
    } else {
      this.#l10nCache.removeElementL10n(bookmarked);
    }

    let lastVisited = item._elements.get("explanationLastVisited");
    let hasLastVisit = setURL && !!result.payload.lastVisit;
    if (hasLastVisit) {
      let { isRelative, formattedDate } = lazy.UrlbarUtils.formatDate(
        new Date(result.payload.lastVisit)
      );
      this.document.l10n.setAttributes(
        lastVisited,
        isRelative
          ? "urlbar-result-explanation-last-visited-relative-2"
          : "urlbar-result-explanation-last-visited-absolute-2",
        { date: formattedDate }
      );
    } else {
      this.#l10nCache.removeElementL10n(lastVisited);
    }

    item.toggleAttribute("has-explanation", hasLastVisit || hasBookmark);
  }

  #updateElementForDynamicType(element, update, item, result = null) {
    if (update.attributes) {
      for (let [key, value] of Object.entries(update.attributes)) {
        if (key == "id") {
          console.error(
            `Not setting id="${value}", as dynamic attributes may not include IDs.`
          );
          continue;
        }
        if (value === undefined) {
          continue;
        }
        if (value === null) {
          element.removeAttribute(key);
        } else if (typeof value == "boolean") {
          element.toggleAttribute(key, value);
        } else if (Blob.isInstance(value) && result) {
          element.setAttribute(key, this.#getBlobUrlForResult(result, value));
        } else {
          element.setAttribute(key, value);
        }
      }
    }

    if (update.dataset) {
      for (let [key, value] of Object.entries(update.dataset)) {
        if (value === null) {
          delete element.dataset[key];
        } else if (value !== undefined) {
          if (typeof value != "string") {
            console.error(
              `Trying to set a dataset value that is not a string`,
              { element, value }
            );
          } else {
            element.dataset[key] = value;
          }
        }
      }
    }

    if (update.classList) {
      if (element == item._content) {
        element.className = "urlbarView-row-inner";
      } else {
        element.className = "";
      }
      element.classList.add(...update.classList);
    }
  }

  #createRowContentForDynamicType(item, result) {
    let { dynamicType } = result.payload;
    let viewTemplate = result.viewTemplate;
    if (!viewTemplate) {
      console.error(`No viewTemplate found for ${result.providerName}`);
      return;
    }
    let classes = this.#buildViewForDynamicType(
      dynamicType,
      item._content,
      item._elements,
      viewTemplate,
      item
    );
    item.toggleAttribute("has-url", classes.has("urlbarView-url"));
    item.toggleAttribute("has-action", classes.has("urlbarView-action"));
    this.#setRowSelectable(item, item._content.hasAttribute("selectable"));
  }

  #buildViewForDynamicType(
    type,
    parentNode,
    elementsByName,
    template,
    item,
    classes = new Set()
  ) {
    this.#updateElementForDynamicType(parentNode, template, item);

    if (template.classList) {
      for (let c of template.classList) {
        classes.add(c);
      }
    }
    if (template.overflowable) {
      parentNode.classList.add("urlbarView-overflowable");
    }

    if (template.name) {
      parentNode.setAttribute("name", template.name);
      parentNode.classList.add(`urlbarView-dynamic-${type}-${template.name}`);
      elementsByName.set(template.name, parentNode);
    }

    for (let childTemplate of template.children || []) {
      let child = this.#createElement(childTemplate.tag);
      parentNode.appendChild(child);
      this.#buildViewForDynamicType(
        type,
        child,
        elementsByName,
        childTemplate,
        item,
        classes
      );
    }

    return classes;
  }

  #createRowContentForRichSuggestion(item, result) {
    item._content.toggleAttribute("selectable", true);

    let favicon = this.#createElement("img");
    favicon.className = "urlbarView-favicon";
    item._content.appendChild(favicon);
    item._elements.set("favicon", favicon);

    let typeIcon = this.#createElement("span");
    typeIcon.className = "urlbarView-type-icon";
    item._content.appendChild(typeIcon);

    let body = this.#createElement("span");
    body.className = "urlbarView-row-body";
    item._content.appendChild(body);

    let bodyTop = this.#createElement("div");
    bodyTop.className = "urlbarView-row-body-top";
    body.appendChild(bodyTop);

    let noWrap = this.#createElement("div");
    noWrap.className = "urlbarView-row-body-top-no-wrap";
    bodyTop.appendChild(noWrap);
    item._elements.set("noWrap", noWrap);

    let title = this.#createElement("span");
    title.classList.add("urlbarView-title", "urlbarView-overflowable");
    noWrap.appendChild(title);
    item._elements.set("title", title);

    let tagsContainer = this.#createElement("span");
    tagsContainer.classList.add("urlbarView-tags", "urlbarView-overflowable");
    noWrap.appendChild(tagsContainer);
    item._elements.set("tagsContainer", tagsContainer);

    let titleSeparator = this.#createElement("span");
    titleSeparator.className = "urlbarView-title-separator";
    noWrap.appendChild(titleSeparator);
    item._elements.set("titleSeparator", titleSeparator);

    let action = this.#createElement("span");
    action.className = "urlbarView-action";
    noWrap.appendChild(action);
    item._elements.set("action", action);

    let url = this.#createElement("span");
    url.className = "urlbarView-url";
    bodyTop.appendChild(url);
    item._elements.set("url", url);

    this.#createExplanation(bodyTop, item);

    let description = this.#createElement("div");
    description.classList.add("urlbarView-row-body-description");
    body.appendChild(description);
    item._elements.set("description", description);

    let bottom = this.#createElement("div");
    bottom.className = "urlbarView-row-body-bottom";
    body.appendChild(bottom);
    item._elements.set("bottom", bottom);
  }

  #createRowContentForBottomUrl(item, _result) {
    item._content.toggleAttribute("selectable", true);

    let favicon = this.#createElement("img");
    favicon.className = "urlbarView-favicon";
    item._content.appendChild(favicon);
    item._elements.set("favicon", favicon);

    let body = this.#createElement("span");
    body.className = "urlbarView-row-body";
    item._content.appendChild(body);

    let bodyTop = this.#createElement("div");
    bodyTop.className = "urlbarView-row-body-top";
    body.appendChild(bodyTop);

    let noWrap = this.#createElement("div");
    noWrap.className = "urlbarView-row-body-top-no-wrap";
    bodyTop.appendChild(noWrap);
    item._elements.set("noWrap", noWrap);

    let title = this.#createElement("span");
    title.classList.add("urlbarView-title", "urlbarView-overflowable");
    noWrap.appendChild(title);
    item._elements.set("title", title);

    let subtitleSeparator = this.#createElement("span");
    subtitleSeparator.className = "urlbarView-subtitle-separator";
    noWrap.appendChild(subtitleSeparator);
    item._elements.set("subtitleSeparator", subtitleSeparator);

    let subtitle = this.#createElement("span");
    subtitle.className = "urlbarView-subtitle";
    noWrap.appendChild(subtitle);
    item._elements.set("subtitle", subtitle);

    let description = this.#createElement("div");
    description.classList.add("urlbarView-row-body-description");
    body.appendChild(description);
    item._elements.set("description", description);

    let bottom = this.#createElement("div");
    bottom.className = "urlbarView-row-body-bottom";
    body.appendChild(bottom);

    let bottomLabel = this.#createElement("span");
    bottomLabel.className = "urlbarView-bottom-label";
    bottom.appendChild(bottomLabel);
    item._elements.set("bottomLabel", bottomLabel);

    let bottomSeparator = this.#createElement("span");
    bottomSeparator.className = "urlbarView-bottom-separator";
    bottom.appendChild(bottomSeparator);
    item._elements.set("bottomSeparator", bottomSeparator);

    let url = this.#createElement("span");
    url.className = "urlbarView-url";
    bottom.appendChild(url);
    item._elements.set("url", url);
  }

  #needsNewButtons(item, oldResult, newResult) {
    if (!oldResult) {
      return true;
    }

    if (
      !!this.#getResultMenuCommands(newResult) !=
      item._buttons.has("result-menu")
    ) {
      return true;
    }

    if (!!oldResult.showFeedbackMenu != !!newResult.showFeedbackMenu) {
      return true;
    }

    if (
      oldResult.payload.buttons?.length != newResult.payload.buttons?.length ||
      !lazy.ObjectUtils.deepEqual(
        oldResult.payload.buttons,
        newResult.payload.buttons
      )
    ) {
      return true;
    }

    return newResult.testForceNewContent;
  }

  #updateRowButtons(item, oldResult, result) {
    for (let i = 0; i < result.payload.buttons?.length; i++) {
      let button = result.payload.buttons[i];
      button.name ??= i.toString();
    }

    if (!this.#needsNewButtons(item, oldResult, result)) {
      return;
    }

    let container = item._elements.get("buttons");
    if (container) {
      container.innerHTML = "";
    } else {
      container = this.#createElement("div");
      container.className = "urlbarView-row-buttons";
      item.appendChild(container);
      item._elements.set("buttons", container);
    }

    item._buttons.clear();

    if (result.payload.buttons) {
      for (let button of result.payload.buttons) {
        this.#addRowButton(item, button);
      }
    }

    let hasResultMenu = !!this.#getResultMenuCommands(result);
    item.toggleAttribute("has-menu-button", hasResultMenu);
    if (hasResultMenu) {
      this.#addRowButton(item, {
        name: "result-menu",
        classList: ["urlbarView-button-menu"],
        l10n: result.showFeedbackMenu
          ? { id: "urlbar-result-menu-button-feedback" }
          : { id: "urlbar-result-menu-button" },
        attributes: lazy.UrlbarPrefs.get("resultMenu.keyboardAccessible")
          ? null
          : {
              "keyboard-inaccessible": true,
            },
      });
    }
  }

  #addRowButton(
    item,
    {
      name: buttonName,
      command,
      l10n,
      url,
      classList = [],
      attributes = {},
      menu = null,
      input = null,
    }
  ) {
    let button = this.#createElement("span");
    this.#updateElementForDynamicType(
      button,
      {
        attributes: {
          ...attributes,
          role: "button",
        },
        classList: [
          ...classList,
          "urlbarView-button",
          "urlbarView-button-" + buttonName,
        ],
        dataset: {
          name: buttonName,
          command,
          url,
          input,
        },
      },
      item
    );

    button.id = `${item.id}-button-${buttonName}`;
    if (l10n) {
      this.#l10nCache.setElementL10n(button, l10n);
    }

    item._buttons.set(buttonName, button);

    if (!menu) {
      item._elements.get("buttons").appendChild(button);
      return;
    }

    let container = this.#createElement("span");
    container.classList.add("urlbarView-splitbutton");

    button.classList.add("urlbarView-splitbutton-main");
    container.appendChild(button);

    let dropmarker = this.#createElement("span");
    dropmarker.classList.add(
      "urlbarView-button",
      "urlbarView-button-menu",
      "urlbarView-splitbutton-dropmarker"
    );
    this.#l10nCache.setElementL10n(dropmarker, {
      id: "urlbar-splitbutton-dropmarker",
    });
    dropmarker.setAttribute("role", "button");
    container.appendChild(dropmarker);

    item._elements.get("buttons").appendChild(container);
  }

  #createSecondaryAction(action, global = false) {
    let actionContainer = this.#createElement("div");
    actionContainer.classList.add("urlbarView-actions-container");

    let button = this.#createElement("span");
    button.classList.add("urlbarView-action-btn");
    if (global) {
      button.classList.add("urlbarView-global-action-btn");
    }
    if (action.classList) {
      button.classList.add(...action.classList);
    }
    button.setAttribute("role", "button");
    if (action.icon) {
      let icon = this.#createElement("img");
      icon.src = action.icon;
      button.appendChild(icon);
    }
    for (let key in action.dataset ?? {}) {
      button.dataset[key] = action.dataset[key];
    }
    button.dataset.action = action.key;
    button.dataset.providerName = action.providerName;

    let label = this.#createElement("span");
    if (action.l10nId) {
      this.#l10nCache.setElementL10n(label, {
        id: action.l10nId,
        args: action.l10nArgs,
      });
    } else {
      this.document.l10n.setAttributes(label, action.label, action.l10nArgs);
    }
    button.appendChild(label);
    actionContainer.appendChild(button);
    return actionContainer;
  }

  #needsNewContent(item, oldResult, newResult) {
    if (!oldResult) {
      return true;
    }

    if (
      (oldResult.type == UrlbarShared.RESULT_TYPE.DYNAMIC) !=
      (newResult.type == UrlbarShared.RESULT_TYPE.DYNAMIC)
    ) {
      return true;
    }

    if (
      oldResult.type == UrlbarShared.RESULT_TYPE.DYNAMIC &&
      newResult.type == UrlbarShared.RESULT_TYPE.DYNAMIC &&
      oldResult.payload.dynamicType != newResult.payload.dynamicType
    ) {
      return true;
    }

    if (oldResult.isRichSuggestion != newResult.isRichSuggestion) {
      return true;
    }

    if (oldResult.heuristic != newResult.heuristic) {
      return true;
    }

    if (
      oldResult.type == UrlbarShared.RESULT_TYPE.TAB_SWITCH &&
      newResult.type != oldResult.type
    ) {
      return true;
    }

    if (newResult.type == UrlbarShared.RESULT_TYPE.DYNAMIC) {
      if (oldResult.providerName != newResult.providerName) {
        return true;
      }

      if (
        !lazy.ObjectUtils.deepEqual(
          oldResult.viewTemplate,
          newResult.viewTemplate
        )
      ) {
        return true;
      }
    }

    if (oldResult.isBottomUrlSuggestion != newResult.isBottomUrlSuggestion) {
      return true;
    }

    return newResult.testForceNewContent;
  }

  // eslint-disable-next-line complexity
  #updateRow(item, result) {
    let oldResult = item.result;
    item.result = result;
    item.removeAttribute("stale");
    item.id = getUniqueId("urlbarView-row-");

    if (this.#needsNewContent(item, oldResult, result)) {
      let buttons = item._elements.get("buttons");
      while (item.lastChild) {
        item.lastChild.remove();
      }
      item._elements.clear();

      item._content = this.#createElement("span");
      item._content.className = "urlbarView-row-inner";
      item.appendChild(item._content);

      for (const attribute of [...item.attributes]) {
        if (!item._sharedAttributes.has(attribute.name)) {
          item.removeAttribute(attribute.name);
        }
      }
      for (const className of item.classList) {
        if (!item._sharedClassList.has(className)) {
          item.classList.remove(className);
        }
      }

      if (item.result.type == UrlbarShared.RESULT_TYPE.DYNAMIC) {
        this.#createRowContentForDynamicType(item, result);
      } else if (result.isBottomUrlSuggestion) {
        this.#createRowContentForBottomUrl(item, result);
      } else if (
        result.isRichSuggestion ||
        Services.prefs.getBoolPref("browser.nova.enabled", false)
      ) {
        this.#createRowContentForRichSuggestion(item, result);
      } else {
        this.#createRowContent(item, result);
      }

      if (buttons) {
        item.appendChild(buttons);
        item._elements.set("buttons", buttons);
      }
    }

    this.#updateRowButtons(item, oldResult, result);

    item._content.id = item.id + "-inner";

    item.toggleAttribute("is-top-pick", !!result.isBestMatch);

    if (result.isBottomUrlSuggestion) {
      this.#updateRowContentForBottomUrl(item, result);
      return;
    }

    let isFirstChild = item === this.#rows.children[0];
    let secAction = result.payload.action;
    let actionsContainer = item.querySelector(".urlbarView-actions-container");
    item.toggleAttribute("secondary-action", !!secAction);
    if (secAction && !actionsContainer) {
      item.appendChild(this.#createSecondaryAction(secAction, isFirstChild));
    } else if (
      secAction &&
      secAction.key != actionsContainer.firstChild.dataset.action
    ) {
      item.replaceChild(
        this.#createSecondaryAction(secAction, isFirstChild),
        actionsContainer
      );
    } else if (!secAction && actionsContainer) {
      item.removeChild(actionsContainer);
    }

    item.removeAttribute("feedback-acknowledgment");

    if (
      result.type == UrlbarShared.RESULT_TYPE.SEARCH &&
      !result.payload.providesSearchMode &&
      !result.payload.inPrivateWindow
    ) {
      item.setAttribute(
        "type",
        result.isRichSuggestion ? "rich-search" : "search"
      );
    } else if (result.type == UrlbarShared.RESULT_TYPE.TAB_SWITCH) {
      item.setAttribute("type", "switchtab");
    } else if (result.type == UrlbarShared.RESULT_TYPE.TIP) {
      item.setAttribute("type", "tip");
      item.setAttribute("tip-type", result.payload.type);

      item.addEventListener("focus", () => this.input.focus(), true);

      if (result.payload.type == "dismissalAcknowledgment") {
        this.#ariaNotifyLocalizedString(
          item,
          result.payload.titleL10n.id,
          result.payload.titleL10n.args
        );
      }
    } else if (result.source == UrlbarShared.RESULT_SOURCE.BOOKMARKS) {
      item.setAttribute("type", "bookmark");
    } else if (result.type == UrlbarShared.RESULT_TYPE.DYNAMIC) {
      item.setAttribute("type", "dynamic");
      this.#updateRowForDynamicType(item, result);
      return;
    } else {
      item.setAttribute(
        "type",
        lazy.UrlbarUtils.resultType(result)
      );
    }

    let favicon = item._elements.get("favicon");
    favicon.src = this.#iconForResult(result);

    let title = item._elements.get("title");
    this.#setResultTitle(result, title);

    this.#updateOverflowTooltip(
      title,
      result.getDisplayableValueAndHighlights("title").value
    );

    let tagsContainer = item._elements.get("tagsContainer");
    if (tagsContainer) {
      tagsContainer.textContent = "";

      let { value: tags, highlights } = result.getDisplayableValueAndHighlights(
        "tags",
        {
          tokens: this.#queryContext.tokens,
        }
      );

      if (tags?.length) {
        tagsContainer.append(
          ...tags.map((tag, i) => {
            const element = this.#createElement("span");
            element.className = "urlbarView-tag";
            lazy.UrlbarUtils.addTextContentWithHighlights(
              element,
              tag,
              highlights[i]
            );
            return element;
          })
        );
      }
    }

    let action = item._elements.get("action");
    let actionSetter = null;
    let isVisitAction = false;
    let setURL = false;
    let isRowSelectable = true;
    switch (result.type) {
      case UrlbarShared.RESULT_TYPE.TAB_SWITCH:
        if (!lazy.UrlbarPrefs.get("secondaryActions.switchToTab")) {
          actionSetter = () => {
            this.#setSwitchTabActionChiclet(result, action);
          };
        }
        setURL = true;
        break;
      case UrlbarShared.RESULT_TYPE.SEARCH:
        if (result.payload.inPrivateWindow) {
          if (result.payload.isPrivateEngine) {
            actionSetter = () => {
              this.#l10nCache.setElementL10n(action, {
                id: "urlbar-result-action-search-in-private-w-engine",
                args: { engine: result.payload.engine },
              });
            };
          } else {
            actionSetter = () => {
              this.#l10nCache.setElementL10n(action, {
                id: "urlbar-result-action-search-in-private",
              });
            };
          }
        } else if (!result.payload.providesSearchMode) {
          actionSetter = () => {
            this.#l10nCache.setElementL10n(action, {
              id: "urlbar-result-action-search-w-engine",
              args: { engine: result.payload.engine },
            });
          };
        }
        break;
      case UrlbarShared.RESULT_TYPE.KEYWORD:
        isVisitAction = result.payload.input.trim() == result.payload.keyword;
        break;
      case UrlbarShared.RESULT_TYPE.TIP:
        isRowSelectable = false;
        break;
      case UrlbarShared.RESULT_TYPE.URL:
      default:
        if (
          result.heuristic &&
          result.payload.url &&
          result.providerName != "UrlbarProviderHistoryUrlHeuristic" &&
          !result.autofill?.noVisitAction
        ) {
          isVisitAction = true;
        } else if (!result.payload.providesSearchMode) {
          setURL = true;
        }
        break;
    }

    this.#setRowSelectable(item, isRowSelectable);

    action?.removeAttribute("slide-in");

    item.toggleAttribute("pinned", !!result.payload.isPinned);

    if (
      result.isRichSuggestion ||
      Services.prefs.getBoolPref("browser.nova.enabled", false)
    ) {
      this.#updateRowForRichSuggestion(item, result);
    }

    item.toggleAttribute("has-url", setURL);
    let url = item._elements.get("url");
    if (setURL) {
      let { value: displayedUrl, highlights } =
        result.getDisplayableValueAndHighlights("url", {
          tokens: this.#queryContext.tokens,
          isURL: true,
        });
      this.#updateOverflowTooltip(url, displayedUrl);

      if (lazy.UrlbarUtils.isTextDirectionRTL(displayedUrl, this.window)) {
        displayedUrl = "\u200e" + displayedUrl;
        highlights = this.#offsetHighlights(highlights, 1);
      }
      lazy.UrlbarUtils.addTextContentWithHighlights(
        url,
        displayedUrl,
        highlights
      );
    } else {
      url.textContent = "";
      this.#updateOverflowTooltip(url, "");
    }

    this.#updateExplanation(item, result, setURL);

    title.toggleAttribute("is-url", isVisitAction);
    if (isVisitAction) {
      actionSetter = () => {
        this.#l10nCache.setElementL10n(action, {
          id: "urlbar-result-action-visit",
        });
      };
    }

    item.toggleAttribute("has-action", actionSetter);
    if (actionSetter) {
      actionSetter();
      item._originalActionSetter = actionSetter;
    } else {
      item._originalActionSetter = () => {
        this.#l10nCache.removeElementL10n(action);
        action.textContent = "";
      };
      item._originalActionSetter();
    }

    if (!title.hasAttribute("is-url")) {
      title.setAttribute("dir", "auto");
    } else {
      title.removeAttribute("dir");
    }
  }

  #setRowSelectable(item, isRowSelectable) {
    item.toggleAttribute("row-selectable", isRowSelectable);
    item._content.toggleAttribute("selectable", isRowSelectable);

    if (isRowSelectable) {
      item._content.setAttribute("role", "option");
    } else if (item._content.getAttribute("role") == "option") {
      item._content.removeAttribute("role");
    }
  }

  #iconForResult(result, iconUrlOverride = null) {
    if (
      result.source == UrlbarShared.RESULT_SOURCE.HISTORY &&
      (result.type == UrlbarShared.RESULT_TYPE.SEARCH ||
        result.type == UrlbarShared.RESULT_TYPE.KEYWORD)
    ) {
      return lazy.UrlbarUtils.ICON.HISTORY;
    }

    if (iconUrlOverride) {
      return iconUrlOverride;
    }

    if (result.payload.icon) {
      return result.payload.icon;
    }
    if (result.payload.iconBlob) {
      let blobUrl = this.#getBlobUrlForResult(result, result.payload.iconBlob);
      if (blobUrl) {
        return blobUrl;
      }
    }

    if (
      result.type == UrlbarShared.RESULT_TYPE.SEARCH ||
      result.type == UrlbarShared.RESULT_TYPE.KEYWORD
    ) {
      return lazy.UrlbarUtils.ICON.SEARCH_GLASS;
    }

    return lazy.UrlbarUtils.ICON.DEFAULT;
  }

  #getBlobUrlForResult(result, blob) {
    let resultUrl = result.payload.url;
    if (resultUrl) {
      let blobUrl = this.#blobUrlsByResultUrl?.get(resultUrl);
      if (!blobUrl) {
        blobUrl = URL.createObjectURL(blob);
        this.#blobUrlsByResultUrl ||= new Map();
        this.#blobUrlsByResultUrl.set(resultUrl, blobUrl);
      }
      return blobUrl;
    }
    return null;
  }

  async #updateRowForDynamicType(item, result) {
    item.setAttribute("dynamicType", result.payload.dynamicType);

    let idsByName = new Map();
    for (let [elementName, node] of item._elements) {
      node.id = `${item.id}-${elementName}`;
      idsByName.set(elementName, node.id);
    }

    let viewUpdate = await this.controller.getViewUpdate(result, idsByName);
    if (item.result != result || !viewUpdate) {
      return;
    }

    for (let [nodeName, update] of Object.entries(viewUpdate)) {
      if (!update) {
        continue;
      }
      let node = item.querySelector(`#${item.id}-${nodeName}`);
      this.#updateElementForDynamicType(node, update, item, result);
      if (update.style) {
        for (let [styleName, value] of Object.entries(update.style)) {
          if (styleName.includes("-")) {
            node.style.setProperty(styleName, value);
          } else {
            node.style[styleName] = value;
          }
        }
      }
      if (update.l10n) {
        this.#l10nCache.setElementL10n(node, update.l10n);
      } else if (update.hasOwnProperty("textContent")) {
        this.#l10nCache.removeElementL10n(node);
        lazy.UrlbarUtils.addTextContentWithHighlights(
          node,
          update.textContent,
          update.highlights
        );
      }
    }
  }

  #updateRowForRichSuggestion(item, result) {
    item.toggleAttribute(
      "rich-suggestion",
      !Services.prefs.getBoolPref("browser.nova.enabled", false)
    );

    this.#setRowSelectable(item, result.type != UrlbarShared.RESULT_TYPE.TIP);

    let favicon = item._elements.get("favicon");
    if (result.richSuggestionIconSize) {
      item.setAttribute("icon-size", result.richSuggestionIconSize);
      favicon.setAttribute("icon-size", result.richSuggestionIconSize);
    } else {
      item.removeAttribute("icon-size");
      favicon.removeAttribute("icon-size");
    }

    if (result.richSuggestionIconVariation) {
      favicon.setAttribute(
        "icon-variation",
        result.richSuggestionIconVariation
      );
    } else {
      favicon.removeAttribute("icon-variation");
    }

    let description = item._elements.get("description");
    if (result.payload.descriptionL10n) {
      this.#l10nCache.setElementL10n(
        description,
        result.payload.descriptionL10n
      );

    } else {
      this.#l10nCache.removeElementL10n(description);
      if (result.payload.description) {
        description.textContent = result.payload.description;
      }
    }

    let bottom = item._elements.get("bottom");
    if (result.payload.bottomTextL10n) {
      this.#l10nCache.setElementL10n(bottom, result.payload.bottomTextL10n);
    } else {
      this.#l10nCache.removeElementL10n(bottom);
    }
  }

  #updateRowContentForBottomUrl(item, result) {
    item.classList.add("with-bottom-url");

    item.toggleAttribute(
      "rich-suggestion",
      !Services.prefs.getBoolPref("browser.nova.enabled", false)
    );

    item.setAttribute(
      "type",
      lazy.UrlbarUtils.resultType(result)
    );
    this.#setRowSelectable(item, true);

    let favicon = item._elements.get("favicon");
    favicon.src = this.#iconForResult(result);
    if (result.richSuggestionIconSize) {
      item.setAttribute("icon-size", result.richSuggestionIconSize);
      favicon.setAttribute("icon-size", result.richSuggestionIconSize);
    } else {
      item.removeAttribute("icon-size");
      favicon.removeAttribute("icon-size");
    }

    let title = item._elements.get("title");
    this.#setResultTitle(result, title);

    let subtitle = item._elements.get("subtitle");
    if (result.payload.subtitleL10n) {
      this.#l10nCache.setElementL10n(subtitle, result.payload.subtitleL10n);
    } else {
      this.#l10nCache.removeElementL10n(subtitle);
      if (result.payload.subtitle) {
        subtitle.textContent = result.payload.subtitle;
      }
    }

    let description = item._elements.get("description");
    description.textContent = result.payload.description;

    let bottomLabel = item._elements.get("bottomLabel");
    this.#l10nCache.setElementL10n(bottomLabel, result.payload.bottomTextL10n);

    let url = item._elements.get("url");
    url.textContent = lazy.UrlbarUtils.prepareUrlForDisplay(result.payload.url);
  }

  #updateIndices() {
    this.visibleResults = [];

    let lastVisibleLabel = null;

    for (let i = 0; i < this.#rows.children.length; i++) {
      let item = this.#rows.children[i];
      let { result } = item;
      result.rowIndex = i;

      let visible = this.#isElementVisible(item);
      if (visible) {
        this.visibleResults.push(result);
      }

      lastVisibleLabel = this.#updateRowLabel(item, visible, lastVisibleLabel);
    }

    let selectableElement = this.getFirstSelectableElement();
    let uiIndex = 0;
    while (selectableElement) {
      selectableElement.elementIndex = uiIndex++;
      selectableElement = this.#getNextSelectableElement(selectableElement);
    }

    this.input.toggleAttribute("noresults", !this.visibleResults.length);
  }

  #updateRowLabel(item, isItemVisible, lastVisibleLabel) {
    let label = isItemVisible ? this.#rowLabel(item) : null;

    let groupAriaLabel = item._elements.get("groupAriaLabel");

    if (
      !label ||
      item.result.hideRowLabel ||
      lazy.ObjectUtils.deepEqual(label, lastVisibleLabel)
    ) {
      this.#l10nCache.removeElementL10n(item, { attribute: "label" });
      if (groupAriaLabel) {
        groupAriaLabel.remove();
        item._elements.delete("groupAriaLabel");
      }
      return lastVisibleLabel;
    }

    this.#l10nCache.setElementL10n(item, {
      attribute: "label",
      id: label.id,
      args: label.args,
    });

    if (!groupAriaLabel) {
      groupAriaLabel = this.#createElement("span");
      groupAriaLabel.className = "urlbarView-group-aria-label";
      item._content.insertBefore(groupAriaLabel, item._content.firstChild);
      item._elements.set("groupAriaLabel", groupAriaLabel);
    }

    this.#l10nCache.ensure(label).then(() => {
      let message = this.#l10nCache.get(label);
      groupAriaLabel.setAttribute("aria-label", message?.attributes.label);
    });

    return label;
  }

  #rowLabel(row) {
    if (!lazy.UrlbarPrefs.get("groupLabels.enabled")) {
      return null;
    }

    if (row.result.rowLabel) {
      return row.result.rowLabel;
    }

    if (row.result.isBestMatch) {
      return { id: "urlbar-group-best-match" };
    }

    if (!this.#queryContext?.searchString || row.result.heuristic) {
      return null;
    }

    return null;
  }

  #setRowVisibility(row, visible) {
    row.toggleAttribute("hidden", !visible);

    if (
      !visible &&
      row.result.type != UrlbarShared.RESULT_TYPE.TIP &&
      row.result.type != UrlbarShared.RESULT_TYPE.DYNAMIC
    ) {
      this.#setElementOverflowing(row._elements.get("title"), false);
      this.#setElementOverflowing(row._elements.get("url"), false);
      let tagsContainer = row._elements.get("tagsContainer");
      if (tagsContainer) {
        this.#setElementOverflowing(tagsContainer, false);
      }
      let explanation = row._elements.get("explanation");
      if (explanation) {
        this.#setElementOverflowing(explanation, false);
      }
    }
  }

  async #ariaNotifyLocalizedString(element, l10nId, l10nArgs) {
    let message = await this.document.l10n.formatValue(l10nId, l10nArgs);
    element.ariaNotify(message);
  }

  #isElementVisible(element) {
    if (!element || element.style.display == "none") {
      return false;
    }
    let row = this.#getRowFromElement(element);
    return row && !row.hasAttribute("hidden");
  }

  #removeStaleRows() {
    let row = this.#rows.lastElementChild;
    while (row) {
      let next = row.previousElementSibling;
      if (row.hasAttribute("stale")) {
        row.remove();
      } else {
        this.#setRowVisibility(row, true);
      }
      row = next;
    }
    this.#updateIndices();


    if (
      this.input.searchMode?.source != UrlbarShared.RESULT_SOURCE.ACTIONS &&
      this.visibleResults[0]?.source != UrlbarShared.RESULT_SOURCE.ACTIONS
    ) {
      this.#rows.toggleAttribute("actionmode", false);
    }

  }

  #startRemoveStaleRowsTimer() {
    this.#removeStaleRowsTimer = this.window.setTimeout(() => {
      this.#removeStaleRowsTimer = null;
      this.#removeStaleRows();
    }, lazy.UrlbarPrefs.get("removeStaleRowsTimeout"));
  }

  #cancelRemoveStaleRowsTimer() {
    if (this.#removeStaleRowsTimer) {
      this.window.clearTimeout(this.#removeStaleRowsTimer);
      this.#removeStaleRowsTimer = null;
    }
  }

  #selectElement(
    element,
    { updateInput = true, setAccessibleFocus = true } = {}
  ) {
    if (element && !element.matches(KEYBOARD_SELECTABLE_ELEMENT_SELECTOR)) {
      throw new Error("Element is not keyboard-selectable");
    }

    if (this.#selectedElement) {
      this.#selectedElement.toggleAttribute("selected", false);
      this.#selectedElement.removeAttribute("aria-selected");
      let row = this.#getSelectedRow();
      row?.toggleAttribute("selected", false);
      row?.toggleAttribute("descendant-selected", false);
    }
    let row = this.#getRowFromElement(element);
    if (element) {
      element.toggleAttribute("selected", true);
      element.setAttribute("aria-selected", "true");
      if (row?.hasAttribute("row-selectable")) {
        row?.toggleAttribute("selected", true);
      }
      if (element != row) {
        row?.toggleAttribute("descendant-selected", true);
      }
    }

    let result = row?.result;
    if (result) {
      this.controller.onBeforeSelection(result, element);
    }

    this.#setAccessibleFocus(setAccessibleFocus && element);
    this.#rawSelectedElement = element;

    if (updateInput) {
      let urlOverride = null;
      if (element?.classList?.contains("urlbarView-button")) {
        urlOverride = "";
      }
      this.input.setValueFromResult({ result, urlOverride, element });
    } else {
      this.input.setResultForCurrentValue(result);
    }

    if (result) {
      this.controller.onSelection(result, element);
    }
  }

  #getClosestSelectableElement(element, { byMouse = false } = {}) {
    let closest = element.closest(
      byMouse
        ? SELECTABLE_ELEMENT_SELECTOR
        : KEYBOARD_SELECTABLE_ELEMENT_SELECTOR
    );
    if (closest && this.#isElementVisible(closest)) {
      return closest;
    }
    if (
      element.classList.contains("urlbarView-row") &&
      element.hasAttribute("row-selectable")
    ) {
      return element._content;
    }
    return null;
  }

  #isSelectableElement(element) {
    return this.#getClosestSelectableElement(element) == element;
  }

  getFirstSelectableElement() {
    let element = this.#rows.firstElementChild;
    if (element && !this.#isSelectableElement(element)) {
      element = this.#getNextSelectableElement(element);
    }
    return element;
  }

  getLastSelectableElement() {
    let element = this.#rows.lastElementChild;
    if (element && !this.#isSelectableElement(element)) {
      element = this.#getPreviousSelectableElement(element);
    }
    return element;
  }

  #getNextSelectableElement(element) {
    let row = this.#getRowFromElement(element);
    if (!row) {
      return null;
    }

    let next = row.nextElementSibling;
    let selectables = this.#getKeyboardSelectablesInRow(row);
    if (selectables.length) {
      let index = selectables.indexOf(element);
      if (index < selectables.length - 1) {
        next = selectables[index + 1];
      }
    }

    if (next && !this.#isSelectableElement(next)) {
      next = this.#getNextSelectableElement(next);
    }

    return next;
  }

  #getPreviousSelectableElement(element) {
    let row = this.#getRowFromElement(element);
    if (!row) {
      return null;
    }

    let previous = row.previousElementSibling;
    let selectables = this.#getKeyboardSelectablesInRow(row);
    if (selectables.length) {
      let index = selectables.indexOf(element);
      if (index < 0) {
        previous = selectables[selectables.length - 1];
      } else if (index > 0) {
        previous = selectables[index - 1];
      }
    }

    if (previous && !this.#isSelectableElement(previous)) {
      previous = this.#getPreviousSelectableElement(previous);
    }

    return previous;
  }

  #getKeyboardSelectablesInRow(row) {
    let selectables = [
      ...row.querySelectorAll(KEYBOARD_SELECTABLE_ELEMENT_SELECTOR),
    ];

    selectables.sort(
      (a, b) => Number(a.localName == "a") - Number(b.localName == "a")
    );

    return selectables;
  }

  #getSelectedRow() {
    return this.#getRowFromElement(this.#selectedElement);
  }

  #getRowFromElement(element) {
    return element?.closest(".urlbarView-row");
  }

  #setAccessibleFocus(item) {
    if (item) {
      if (!item.id) {
        item.id = getUniqueId("aria-activedescendant-target-");
      }
      this.input.inputField.setAttribute("aria-activedescendant", item.id);
    } else {
      this.input.inputField.removeAttribute("aria-activedescendant");
    }
  }

  #setResultTitle(result, titleNode) {
    if (result.payload.titleL10n) {
      this.#l10nCache.setElementL10n(titleNode, result.payload.titleL10n);
      return;
    }

    if (result.payload.providesSearchMode) {
      if (result.type == UrlbarShared.RESULT_TYPE.RESTRICT) {
        let localSearchMode =
          result.payload.l10nRestrictKeywords[0].toLowerCase();
        let keywords = result.payload.l10nRestrictKeywords
          .map(keyword => `@${keyword.toLowerCase()}`)
          .join(", ");

        this.#l10nCache.setElementL10n(titleNode, {
          id: "urlbar-result-search-with-local-search-mode",
          args: {
            keywords,
            localSearchMode,
          },
        });
      } else if (
        result.providerName == "UrlbarProviderTokenAliasEngines" &&
        lazy.UrlbarPrefs.get(
          "searchRestrictKeywords.featureGate"
        )
      ) {
        this.#l10nCache.setElementL10n(titleNode, {
          id: "urlbar-result-search-with-engine-keywords",
          args: {
            keywords: result.payload.keywords,
            engine: result.payload.engine,
          },
        });
      } else {
        this.#l10nCache.setElementL10n(titleNode, {
          id: "urlbar-result-action-search-w-engine",
          args: { engine: result.payload.engine },
        });
      }

      return;
    }

    this.#l10nCache.removeElementL10n(titleNode);

    let titleAndHighlights = result.getDisplayableValueAndHighlights("title", {
      tokens: this.#queryContext.tokens,
    });
    lazy.UrlbarUtils.addTextContentWithHighlights(
      titleNode,
      titleAndHighlights.value,
      titleAndHighlights.highlights
    );
  }

  #offsetHighlights(highlights, startOffset) {
    return highlights.map(highlight => [
      highlight[0] + startOffset,
      highlight[1],
    ]);
  }

  #setSwitchTabActionChiclet(result, actionNode) {
    actionNode.classList.add("urlbarView-switchToTab");

    let contextualIdentityAction = actionNode.parentNode.querySelector(
      ".action-contextualidentity"
    );

    if (
      result.type == UrlbarShared.RESULT_TYPE.TAB_SWITCH &&
      lazy.UrlbarProviderOpenTabs.isContainerUserContextId(
        result.payload.userContextId
      )
    ) {
      if (!contextualIdentityAction) {
        contextualIdentityAction = actionNode.cloneNode(true);
        contextualIdentityAction.classList.add("action-contextualidentity");
        this.#l10nCache.removeElementL10n(contextualIdentityAction);
        actionNode.parentNode.insertBefore(
          contextualIdentityAction,
          actionNode
        );
      }

      this.#addContextualIdentityToSwitchTabChiclet(
        result,
        contextualIdentityAction
      );
    } else {
      contextualIdentityAction?.remove();
    }

    let tabGroupAction = actionNode.parentNode.querySelector(
      ".urlbarView-tabGroup"
    );

    if (
      result.type == UrlbarShared.RESULT_TYPE.TAB_SWITCH &&
      result.payload.tabGroup
    ) {
      if (!tabGroupAction) {
        tabGroupAction = actionNode.cloneNode(true);
        this.#l10nCache.removeElementL10n(tabGroupAction);
        actionNode.parentNode.insertBefore(tabGroupAction, actionNode);
      }

      this.#addGroupToSwitchTabChiclet(result, tabGroupAction);
    } else {
      tabGroupAction?.remove();
    }
    let splitview = this.chromeWindow.gBrowser.selectedTab.splitview;
    let shouldMoveTabToSplitView =
      splitview &&
      !splitview.tabs.some(
        tab => tab.linkedBrowser.currentURI.spec === result.payload.url
      );
    this.#l10nCache.setElementL10n(actionNode, {
      id: shouldMoveTabToSplitView
        ? "urlbar-result-action-move-tab-to-split-view"
        : "urlbar-result-action-switch-tab",
    });
  }

  #addContextualIdentityToSwitchTabChiclet(result, actionNode) {
    let label = lazy.ContextualIdentityService.getUserContextLabel(
      result.payload.userContextId
    );
    if (
      actionNode.classList.contains("urlbarView-userContext") &&
      label &&
      actionNode == label
    ) {
      return;
    }
    actionNode.innerHTML = "";
    let identity = lazy.ContextualIdentityService.getPublicIdentityFromId(
      result.payload.userContextId
    );
    if (identity) {
      actionNode.classList.add("urlbarView-userContext");
      actionNode.classList.remove("urlbarView-switchToTab");
      if (identity.color) {
        actionNode.className = actionNode.className.replace(
          /identity-color-\w*/g,
          ""
        );
        actionNode.classList.add("identity-color-" + identity.color);
      }

      let textModeLabel = this.#createElement("div");
      textModeLabel.classList.add("urlbarView-userContext-textMode");

      if (label) {
        textModeLabel.innerText = label;
        actionNode.appendChild(textModeLabel);

        let iconModeLabel = this.#createElement("div");
        iconModeLabel.classList.add("urlbarView-userContext-iconMode");
        actionNode.appendChild(iconModeLabel);
        let iconURL = lazy.ContextualIdentityService.getContainerIconURL(
          identity.icon
        );
        if (iconURL) {
          let userContextIcon = this.#createElement("img");
          userContextIcon.classList.add("urlbarView-userContext-icon");
          userContextIcon.setAttribute("alt", label);
          userContextIcon.src = iconURL;
          iconModeLabel.appendChild(userContextIcon);
        }
        actionNode.setAttribute("tooltiptext", label);
      }
    }
  }

  #addGroupToSwitchTabChiclet(result, actionNode) {
    const group = this.chromeWindow.gBrowser.getTabGroupById(
      result.payload.tabGroup
    );
    if (!group) {
      actionNode.remove();
      return;
    }

    actionNode.classList.add("urlbarView-tabGroup");
    actionNode.classList.remove("urlbarView-switchToTab");

    actionNode.innerHTML = "";
    let fullWidthModeLabel = this.#createElement("div");
    fullWidthModeLabel.classList.add("urlbarView-tabGroup-fullWidthMode");

    let narrowWidthModeLabel = this.#createElement("div");
    narrowWidthModeLabel.classList.add("urlbarView-tabGroup-narrowWidthMode");

    if (group.label) {
      fullWidthModeLabel.textContent = group.label;
      narrowWidthModeLabel.textContent = group.label[0];
    } else {
      this.#l10nCache.setElementL10n(fullWidthModeLabel, {
        id: `urlbar-result-action-tab-group-unnamed`,
      });
    }

    actionNode.appendChild(fullWidthModeLabel);
    actionNode.appendChild(narrowWidthModeLabel);

    actionNode.style.setProperty(
      "--tab-group-color",
      group.style.getPropertyValue("--tab-group-color")
    );
    actionNode.style.setProperty(
      "--tab-group-color-invert",
      group.style.getPropertyValue("--tab-group-color-invert")
    );
    actionNode.style.setProperty(
      "--tab-group-color-pale",
      group.style.getPropertyValue("--tab-group-color-pale")
    );
    actionNode.style.setProperty(
      "--tab-group-background-color",
      group.style.getPropertyValue("--tab-group-background-color")
    );
    actionNode.style.setProperty(
      "--tab-group-text-color",
      group.style.getPropertyValue("--tab-group-text-color")
    );
  }

  #enableOrDisableRowWrap() {
    let wrap = getBoundsWithoutFlushing(this.input).width < 650;
    this.#rows.toggleAttribute("wrap", wrap);
    this.oneOffSearchButtons?.container.toggleAttribute("wrap", wrap);
  }

  #canElementOverflow(element) {
    let { classList } = element;
    return (
      classList.contains("urlbarView-overflowable") ||
      classList.contains("urlbarView-url")
    );
  }

  #setElementOverflowing(element, overflowing) {
    element.toggleAttribute("overflow", overflowing);
    this.#updateOverflowTooltip(element);
  }

  #updateOverflowTooltip(element, tooltip) {
    if (typeof tooltip == "string") {
      element._tooltip = tooltip;
    }
    if (element.hasAttribute("overflow") && element._tooltip) {
      element.setAttribute("title", element._tooltip);
    } else {
      element.removeAttribute("title");
    }
  }

  #pickSearchTipIfPresent(event) {
    if (
      !this.isOpen ||
      !this.#queryContext ||
      this.#queryContext.results.length != 1
    ) {
      return false;
    }
    let result = this.#queryContext.results[0];
    if (result.type != UrlbarShared.RESULT_TYPE.TIP) {
      return false;
    }
    let buttons = this.#rows.firstElementChild._buttons;
    let tipButton = buttons.get("tip") || buttons.get("0");
    if (!tipButton) {
      throw new Error("Expected a tip button");
    }
    this.input.pickElement(tipButton, event);
    return true;
  }

  async #cacheL10nStrings() {
    let idArgs = [
      ...this.#cacheL10nIDArgsForSearchService(),
      { id: "urlbar-result-action-search-bookmarks" },
      { id: "urlbar-result-action-search-history" },
      { id: "urlbar-result-action-search-in-private" },
      { id: "urlbar-result-action-search-tabs" },
      { id: "urlbar-result-action-switch-tab" },
      { id: "urlbar-result-action-visit" },
      { id: "urlbar-result-action-move-tab-to-split-view" },
    ];

    if (lazy.UrlbarPrefs.get("groupLabels.enabled")) {
      idArgs.push({ id: "urlbar-group-best-match" });
    }

    await this.#l10nCache.ensureAll(idArgs);
  }

  #cacheL10nIDArgsForSearchService() {
    if (!lazy.SearchService.hasSuccessfullyInitialized) {
      return [];
    }

    let idArgs = [];

    let { defaultEngine, defaultPrivateEngine } = lazy.SearchService;
    let engineNames = [defaultEngine?.name, defaultPrivateEngine?.name].filter(
      engineName => engineName
    );

    if (defaultPrivateEngine) {
      idArgs.push({
        id: "urlbar-result-action-search-in-private-w-engine",
        args: { engine: defaultPrivateEngine.name },
      });
    }

    let engineStringIDs = [
      "urlbar-result-action-tabtosearch-web",
      "urlbar-result-action-tabtosearch-other-engine",
      "urlbar-result-action-search-w-engine",
    ];
    for (let id of engineStringIDs) {
      idArgs.push(
        ...engineNames.map(engineName => ({
          id,
          args: { engine: engineName },
        }))
      );
    }

    if (lazy.UrlbarPrefs.get("groupLabels.enabled")) {
      idArgs.push(
        ...engineNames.map(engineName => ({
          id: "urlbar-group-search-suggestions",
          args: { engine: engineName },
        }))
      );
    }

    return idArgs;
  }

  #getResultMenuCommands(result) {
    if (this.#resultMenuCommands.has(result)) {
      return this.#resultMenuCommands.get(result);
    }

    let commands = result.commands;
    if (commands) {
      this.#resultMenuCommands.set(result, commands);
      return commands;
    }

    commands = [];
    if (result.payload.isBlockable) {
      commands.push({
        name: RESULT_MENU_COMMANDS.DISMISS,
        l10n: result.payload.blockL10n || {
          id: "urlbar-result-menu-dismiss-suggestion2",
        },
      });
    }
    if (result.payload.helpUrl) {
      commands.push({
        name: RESULT_MENU_COMMANDS.HELP,
        l10n: result.payload.helpL10n || {
          id: "urlbar-result-menu-learn-more2",
        },
      });
    }
    let rv = commands.length ? commands : null;
    this.#resultMenuCommands.set(result, rv);
    return rv;
  }

  async #populateResultMenu({ panel = this.resultMenu, commands }) {
    panel.textContent = "";
    await this.#l10nCache.ensureAll(commands.map(e => e.l10n).filter(e => e));
    for (let data of commands) {
      if (data.name == "separator") {
        panel.appendChild(this.document.createElement("separator"));
        continue;
      }
      let menuitem = this.document.createElement("panel-item");
      menuitem.dataset.command = data.name;
      menuitem.classList.add("urlbarView-result-menuitem");
      this.#l10nCache.setElementL10n(menuitem, data.l10n);
      panel.appendChild(menuitem);
    }
  }


  on_SelectedOneOffButtonChanged() {
    if (!this.isOpen || !this.#queryContext) {
      return;
    }

    let engine = this.oneOffSearchButtons.selectedButton?.engine;
    let source = this.oneOffSearchButtons.selectedButton?.source;
    let icon = this.oneOffSearchButtons.selectedButton?.image;

    let localSearchMode;
    if (source) {
      localSearchMode = lazy.UrlbarUtils.LOCAL_SEARCH_MODES.find(
        m => m.source == source
      );
    }

    for (let item of this.#rows.children) {
      let result = item.result;

      let isPrivateSearchWithoutPrivateEngine =
        result.payload.inPrivateWindow && !result.payload.isPrivateEngine;
      if (
        !result.heuristic &&
        !isPrivateSearchWithoutPrivateEngine
      ) {
        continue;
      }

      if (
        result.heuristic &&
        !engine &&
        !localSearchMode &&
        this.input.searchMode &&
        !this.input.searchMode.isPreview
      ) {
        continue;
      }

      let action = item._elements.get("action");
      let favicon = item._elements.get("favicon");
      let title = item._elements.get("title");

      if (
        result.heuristic &&
        !this.selectedElement &&
        (localSearchMode || engine)
      ) {
        item.setAttribute("show-action-text", "true");
      } else {
        item.removeAttribute("show-action-text");
      }

      if (result.type == UrlbarShared.RESULT_TYPE.SEARCH) {
        if (engine) {
          if (!result.payload.originalEngine) {
            result.payload.originalEngine = result.payload.engine;
          }
          result.payload.engine = engine.name;
        } else if (result.payload.originalEngine) {
          result.payload.engine = result.payload.originalEngine;
          delete result.payload.originalEngine;
        }
      }

      if (result.heuristic) {
        title.textContent =
          localSearchMode || engine
            ? this.#queryContext.searchString
            : result.getDisplayableValueAndHighlights("title").value;

        if (localSearchMode || engine) {
          item.setAttribute("restyled-search", "true");
        } else {
          item.removeAttribute("restyled-search");
        }
      }

      if (localSearchMode) {
        const messageIDs = {
          actions: "urlbar-result-action-search-actions",
          bookmarks: "urlbar-result-action-search-bookmarks",
          history: "urlbar-result-action-search-history",
          tabs: "urlbar-result-action-search-tabs",
        };
        let sourceName = lazy.UrlbarUtils.getResultSourceName(
          localSearchMode.source
        );
        this.#l10nCache.setElementL10n(action, {
          id: messageIDs[sourceName],
        });
        if (result.heuristic) {
          item.setAttribute("source", sourceName);
        }
      } else if (engine && !result.payload.inPrivateWindow) {
        this.#l10nCache.setElementL10n(action, {
          id: "urlbar-result-action-search-w-engine",
          args: { engine: engine.name },
        });
      } else {
        if (item._originalActionSetter) {
          item._originalActionSetter();
          if (result.heuristic) {
            favicon.src = result.payload.icon || lazy.UrlbarUtils.ICON.DEFAULT;
          }
        } else {
          console.error("An item is missing the action setter");
        }
        item.removeAttribute("source");
      }

      let iconOverride = localSearchMode?.icon;
      if (
        !iconOverride &&
        icon != "chrome://browser/skin/search-engine-placeholder.png"
      ) {
        iconOverride = icon;
      }
      if (!iconOverride && (localSearchMode || engine)) {
        iconOverride = lazy.UrlbarUtils.ICON.SEARCH_GLASS;
      }
      if (
        result.heuristic ||
        (result.payload.inPrivateWindow && !result.payload.isPrivateEngine)
      ) {
        favicon.src = this.#iconForResult(result, iconOverride);
      }
    }
  }

  on_blur() {
    if (!lazy.UrlbarPrefs.get("ui.popup.disable_autohide")) {
      this.close();
    }
  }

  on_mousedown(event) {
    if (event.button == 2) {
      return;
    }

    let element = this.#getClosestSelectableElement(event.target, {
      byMouse: true,
    });
    if (!element) {
      return;
    }

    this.panel.documentGlobal.addEventListener("mouseup", this);

    if (!element.classList.contains("urlbarView-button")) {
      this.#mousedownSelectedElement = element;
      this.#selectElement(element, { updateInput: false });
      this.controller.speculativeConnect(
        this.selectedResult,
        this.#queryContext,
        "mousedown"
      );
    }
  }

  on_mouseup(event) {
    if (event.button == 2) {
      return;
    }

    this.panel.documentGlobal.removeEventListener("mouseup", this);

    const eventTarget = event.composedPath()[0];
    let element =
      eventTarget.nodeType === eventTarget.ELEMENT_NODE
        ? this.#getClosestSelectableElement(eventTarget, {
            byMouse: true,
          })
        : null;
    if (element) {
      this.input.pickElement(element, event);
    }

    if (this.#mousedownSelectedElement?.isConnected) {
      this.clearSelection();
    }
    this.#mousedownSelectedElement = null;
  }

  #isRelevantOverflowEvent(event) {
    return event.detail != 0;
  }

  on_overflow(event) {
    if (
      this.#isRelevantOverflowEvent(event) &&
      this.#canElementOverflow(event.target)
    ) {
      this.#setElementOverflowing(event.target, true);
    }
  }

  on_underflow(event) {
    if (
      this.#isRelevantOverflowEvent(event) &&
      this.#canElementOverflow(event.target)
    ) {
      this.#setElementOverflowing(event.target, false);
    }
  }

  on_resize() {
    this.#enableOrDisableRowWrap();
  }

  on_click(event) {
    let result = this.#resultMenuResult;
    this.#resultMenuResult = null;
    let menuitem = event.target;
    switch (menuitem.dataset.command) {
      case RESULT_MENU_COMMANDS.HELP:
        menuitem.dataset.url =
          result.payload.helpUrl ||
          Services.urlFormatter.formatURLPref("app.support.baseURL") +
            "awesome-bar-result-menu";
        break;
    }
    this.input.pickResult(result, event, menuitem);
  }

  on_command(event) {
    let contextMenu;
    if ((contextMenu = event.target.closest("#urlbarView-context-menu"))) {
      let row = contextMenu.triggerNode.closest(".urlbarView-row");
      this.input.pickResult(row.result, event, event.target);
    }
  }

  on_showing(event) {
    let commands;
    let splitButton = event.target.triggeringEvent.detail.target.closest(
      ".urlbarView-splitbutton"
    );

    if (splitButton) {
      let mainButton = splitButton.firstElementChild;
      let buttonName = mainButton.dataset.name;
      commands = this.#resultMenuResult.payload.buttons.find(
        b => b.name == buttonName
      ).menu;
    } else {
      commands = this.#getResultMenuCommands(this.#resultMenuResult);
    }

    this.#populateResultMenu({ commands });
  }

  on_popupshowing(event) {
    if (event.target.id == "urlbarView-context-menu") {
      if (!lazy.UrlbarPrefs.get("contextMenu.featureGate")) {
        event.preventDefault();
        return;
      }

      let row = event.triggerEvent?.target.closest(".urlbarView-row");
      if (!row) {
        event.preventDefault();
        return;
      }

      row.toggleAttribute("context-menu-trigger", true);

      let url = lazy.UrlbarUtils.getUrlFromResult(row.result, {
        element: row,
      })?.url;
      event.target.toggleAttribute("disabled", !url);
    } else if (
      event.target.id == "urlbarView-context-menu-open-in-container-tab-popup"
    ) {
      event.target.documentGlobal.createUserContextMenu(event, {
        isContextMenu: true,
      });
    }
  }

  on_popuphiding(event) {
    if (event.target.id == "urlbarView-context-menu") {
      event.target.triggerNode
        .closest(".urlbarView-row")
        ?.toggleAttribute("context-menu-trigger", false);
    }
  }

  on_contextmenu(event) {
    if (event.target.closest(".urlbar-input-container")) {
      return;
    }

    event.preventDefault();

    if (
      !lazy.UrlbarPrefs.get("contextMenu.featureGate") ||
      !event.target.closest(".urlbarView-row")
    ) {
      return;
    }

    if (!this.#contextMenu) {
      this.#contextMenu = this.document.querySelector(
        "#urlbarView-context-menu"
      );
      this.#contextMenu.addEventListener("command", this);
      this.#contextMenu.addEventListener("popupshowing", this);
      this.#contextMenu.addEventListener("popuphiding", this);
    }

    this.#contextMenu.openPopupAtScreen(
      event.screenX,
      event.screenY,
      true,
      event
    );
  }
}

class QueryContextCache {
  #cache;
  #size;

  constructor(size) {
    this.#size = size;
    this.#cache = [];
  }

  get size() {
    return this.#size;
  }

  put(queryContext) {
    if (!queryContext.results.length) {
      return;
    }

    let searchString = queryContext.searchString;
    if (!searchString) {
      return;
    }

    let index = this.#cache.findIndex(e => e.searchString == searchString);
    if (index != -1) {
      if (this.#cache[index] == queryContext) {
        return;
      }
      this.#cache.splice(index, 1);
    }
    if (this.#cache.unshift(queryContext) > this.size) {
      this.#cache.length = this.size;
    }
  }

  get(searchString) {
    return this.#cache.find(e => e.searchString == searchString);
  }
}
