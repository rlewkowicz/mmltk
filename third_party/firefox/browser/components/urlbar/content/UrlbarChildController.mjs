/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { UrlbarShared } from "chrome://browser/content/urlbar/UrlbarShared.mjs";
import { UrlbarParentControllerProxy } from "chrome://browser/content/urlbar/UrlbarParentControllerProxy.mjs";

const { AppConstants } = ChromeUtils.importESModule(
  "resource://gre/modules/AppConstants.sys.mjs"
);

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  UrlbarParentController:
    "moz-src:///browser/components/urlbar/UrlbarParentController.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
});


export class UrlbarChildController {
  static #logger;

  get logger() {
    if (!UrlbarChildController.#logger) {
      UrlbarChildController.#logger = UrlbarShared.getLogger({
        prefix: "ChildController",
      });
    }
    return UrlbarChildController.#logger;
  }

  #parent;

  #input;

  #view = null;

  #listeners = new Set();

  #userSelectionBehavior =  ("none");


  constructor(options) {
    if (!options.input) {
      throw new Error("Missing options: input");
    }
    this.#input = options.input;
    let actor =  (
       (
        options.input.window.windowGlobalChild.getActor("Urlbar")
      )
    );
    let { sapName, isPrivate } = options.input;
    this.#parent = actor.usesMessagePath
      ? new UrlbarParentControllerProxy(
          actor,
          actor.registerMessagePathInput(options.input),
          { sapName, isPrivate }
        )
      : new lazy.UrlbarParentController({ sapName, isPrivate, actor });
    this.#parent.setChild(this);
  }

  get input() {
    return this.#input;
  }
  get window() {
    return this.#input.window;
  }
  get view() {
    return this.#view;
  }
  get parentController() {
    return this.#parent;
  }
  get platform() {
    return AppConstants.platform;
  }
  get userSelectionBehavior() {
    return this.#userSelectionBehavior;
  }

  set userSelectionBehavior(behavior) {
    if (behavior == "arrow" && this.#userSelectionBehavior == "tab") {
      return;
    }
    this.#userSelectionBehavior = behavior;
  }
  get _lastQueryContextWrapper() {
    return this.#parent._lastQueryContextWrapper;
  }

  setView(view) {
    this.#view = view;
  }
  getViewUpdate(result, idsByName) {
    return this.#parent.getViewUpdate(result, idsByName);
  }
  onBeforeSelection(result, element) {
    return this.#parent.onBeforeSelection(result, element);
  }
  onSelection(result, element) {
    return this.#parent.onSelection(result, element);
  }
  getHeuristicResult(queryContext) {
    return this.#parent.getHeuristicResult(queryContext);
  }
  addListener(listener) {
    if (!listener || typeof listener != "object") {
      throw new TypeError("Expected listener to be an object");
    }
    this.#listeners.add(listener);
  }
  removeListener(listener) {
    this.#listeners.delete(listener);
  }
  notify(notification, ...params) {
    if (
      notification === UrlbarShared.NOTIFICATIONS.QUERY_RESULTS &&
      params[0].firstResultChanged
    ) {
      this.speculativeConnect(params[0].results[0], params[0], "resultsadded");
    }
    for (let listener of this.#listeners) {
      if (typeof listener[notification] != "undefined") {
        try {
          listener[notification](...params);
        } catch (ex) {
          console.error(ex);
        }
      }
    }
  }
  startQuery(queryContext) {
    return this.#parent.startQuery(queryContext);
  }
  cancelQuery() {
    return this.#parent.cancelQuery();
  }
  receiveResults(queryContext) {
    return this.#parent.receiveResults(queryContext);
  }
  removeResult(result) {
    return this.#parent.removeResult(result);
  }
  setLastQueryContextCache(queryContext) {
    return this.#parent.setLastQueryContextCache(queryContext);
  }
  clearLastQueryContextCache() {
    return this.#parent.clearLastQueryContextCache();
  }
  // eslint-disable-next-line complexity
  handleKeyNavigation(event, executeAction = true) {
    if (this.view.resultMenu.hasAttribute("open")) {
      return;
    }

    const isMac = AppConstants.platform == "macosx";
    if (
      isMac &&
      this.view.isOpen &&
      event.ctrlKey &&
      (event.key == "n" || event.key == "p")
    ) {
      if (executeAction) {
        this.view.selectBy(1, { reverse: event.key == "p" });
      }
      event.preventDefault();
      return;
    }

    if (executeAction) {
      if (
        event.shiftKey &&
        (event.keyCode === KeyEvent.DOM_VK_UP ||
          event.keyCode === KeyEvent.DOM_VK_DOWN)
      ) {
        return;
      }

      let handled = this.input.searchModeSwitcher.handleKeyDown(event);
      if (handled) {
        return;
      }
    }

    switch (event.keyCode) {
      case KeyEvent.DOM_VK_ESCAPE:
        if (executeAction) {
          if (this.view.isOpen) {
            this.view.close();
          } else if (
            this.window.gBrowser &&
            lazy.UrlbarPrefs.get("focusContentDocumentOnEsc") &&
            !this.input.searchMode &&
            (this.input.sapName == "searchbar"
              ? this.input.value == ""
              : this.input.getAttribute("pageproxystate") == "valid" ||
                (this.input.value == "" &&
                  this.window.isBlankPageURL(
                    this.window.gBrowser.currentURI.spec
                  )))
          ) {
            this.window.gBrowser.selectedBrowser.focus();
          } else {
            this.input.handleRevert();
          }
        }
        event.preventDefault();
        break;
      case KeyEvent.DOM_VK_SPACE:
        if (!this.view.shouldSpaceActivateSelectedElement()) {
          break;
        }
      case KeyEvent.DOM_VK_RETURN:
        this.logger.debug(`Enter pressed${executeAction ? "" : " delayed"}`);
        if (executeAction) {
          this.input.handleCommand(event);
        }
        event.preventDefault();
        break;
      case KeyEvent.DOM_VK_TAB: {
        if (!this.view.visibleRowCount) {
          break;
        }


        if (
          this.view.isOpen &&
          !event.ctrlKey &&
          !event.altKey
        ) {
          if (
            (event.shiftKey &&
              this.view.selectedElement ==
                this.view.getFirstSelectableElement()) ||
            (!event.shiftKey &&
              this.view.selectedElement == this.view.getLastSelectableElement())
          ) {
            event.preventDefault();
            this.view.selectedRowIndex = -1;
            this.focusOnUnifiedSearchButton();
            break;
          } else if (
            !this.view.selectedElement &&
            this.input.focusedViaMousedown
          ) {
            if (event.shiftKey) {
              this.focusOnUnifiedSearchButton();
            } else {
              this.view.selectBy(1, {
                userPressedTab: true,
              });
            }
            event.preventDefault();
            break;
          }
        }

        let allowTabbingThroughResults =
          this.input.focusedViaMousedown ||
          this.input.searchMode?.isPreview ||
          this.input.searchMode?.source == UrlbarShared.RESULT_SOURCE.ACTIONS ||
          this.view.selectedElement ||
          (this.input.value &&
            this.input.getAttribute("pageproxystate") != "valid");
        if (
          (this.view.isOpen || !executeAction) &&
          !event.ctrlKey &&
          !event.altKey &&
          allowTabbingThroughResults
        ) {
          if (executeAction) {
            this.userSelectionBehavior = "tab";
            this.view.selectBy(1, {
              reverse: event.shiftKey,
              userPressedTab: true,
            });
          }
          event.preventDefault();
        }
        break;
      }
      case KeyEvent.DOM_VK_PAGE_DOWN:
      case KeyEvent.DOM_VK_PAGE_UP:
        if (event.ctrlKey) {
          break;
        }
      // eslint-disable-next-lined no-fallthrough
      case KeyEvent.DOM_VK_DOWN:
      case KeyEvent.DOM_VK_UP:
        if (event.altKey) {
          break;
        }
        if (this.view.isOpen) {
          if (executeAction) {
            this.userSelectionBehavior = "arrow";
            this.view.selectBy(
              event.keyCode == KeyEvent.DOM_VK_PAGE_DOWN ||
                event.keyCode == KeyEvent.DOM_VK_PAGE_UP
                ? lazy.UrlbarUtils.PAGE_UP_DOWN_DELTA
                : 1,
              {
                reverse:
                  event.keyCode == KeyEvent.DOM_VK_UP ||
                  event.keyCode == KeyEvent.DOM_VK_PAGE_UP,
              }
            );
          }
        } else {
          if (this.keyEventMovesCaret(event)) {
            break;
          }
          if (executeAction) {
            this.userSelectionBehavior = "arrow";
            this.input.startQuery({
              searchString: this.input.value,
              event,
            });
          }
        }
        event.preventDefault();
        break;
      case KeyEvent.DOM_VK_RIGHT:
      case KeyEvent.DOM_VK_END:
        this.input.maybeConfirmSearchModeFromResult({
          entry: "typed",
          startQuery: true,
        });
      case KeyEvent.DOM_VK_LEFT:
      case KeyEvent.DOM_VK_HOME:
        this.view.removeAccessibleFocus();
        break;
      case KeyEvent.DOM_VK_BACK_SPACE:
        if (
          this.input.searchMode &&
          this.input.selectionStart == 0 &&
          this.input.selectionEnd == 0 &&
          !event.shiftKey
        ) {
          this.input.searchMode = null;
          if (this.input.view.oneOffSearchButtons) {
            this.input.view.oneOffSearchButtons.selectedButton = null;
          }
          this.input.startQuery({
            allowAutofill: false,
            event,
          });
        }
      case KeyEvent.DOM_VK_DELETE:
        if (!this.view.isOpen) {
          break;
        }
        if (event.shiftKey) {
          if (!executeAction || this.#dismissSelectedResult(event)) {
            event.preventDefault();
          }
        } else if (executeAction) {
          this.userSelectionBehavior = "none";
        }
        break;
    }
  }

  #dismissSelectedResult(event) {
    if (!this._lastQueryContextWrapper) {
      console.error("Cannot dismiss selected result, last query not present");
      return false;
    }
    let { queryContext } = this._lastQueryContextWrapper;

    let { selectedElement } = this.input.view;
    if (selectedElement?.classList.contains("urlbarView-button")) {
      return false;
    }

    let result = this.input.view.selectedResult;
    if (!result) {
      return false;
    }
    if (result.heuristic && !result.autofill) {
      return false;
    }


    return true;
  }

  keyEventMovesCaret(event) {
    if (this.view.isOpen) {
      return false;
    }
    if (AppConstants.platform != "macosx" && AppConstants.platform != "linux") {
      return false;
    }
    let isArrowUp = event.keyCode == KeyEvent.DOM_VK_UP;
    let isArrowDown = event.keyCode == KeyEvent.DOM_VK_DOWN;
    if (!isArrowUp && !isArrowDown) {
      return false;
    }
    let start = this.input.selectionStart;
    let end = this.input.selectionEnd;
    if (
      end != start ||
      (isArrowUp && start > 0) ||
      (isArrowDown && end < this.input.value.length)
    ) {
      return true;
    }
    return false;
  }

  speculativeConnect(result, context, reason) {
    return this.#parent.speculativeConnect(result, context, reason);
  }
  focusOnUnifiedSearchButton() {
    this.input.setUnifiedSearchButtonAvailability(true);

    const switcher = this.input.querySelector(".searchmode-switcher");
    switcher.setAttribute("tabindex", "-1");
    this.input.inputField.removeEventListener("blur", this.input);
    switcher.focus();
    this.input.inputField.addEventListener("blur", this.input);
    switcher.addEventListener(
      "blur",
      e => {
        switcher.removeAttribute("tabindex");

        let relatedTarget =  (e.relatedTarget);
        if (
          this.input.hasAttribute("focused") &&
          !this.input.contains(relatedTarget)
        ) {
          this.input.inputField.dispatchEvent(
            new FocusEvent("blur", {
              relatedTarget: e.relatedTarget,
            })
          );
        }
      },
      { once: true }
    );
  }
}
