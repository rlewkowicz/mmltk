/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  GeckoViewAutocomplete: "resource://gre/modules/GeckoViewAutocomplete.sys.mjs",
  clearTimeout: "resource://gre/modules/Timer.sys.mjs",
  setTimeout: "resource://gre/modules/Timer.sys.mjs",
});

const PREF_SECURITY_DELAY = "security.notification_enable_delay";

let currentActor = null;

let autoCompleteListeners = new Set();

function compareContext(message) {
  if (
    !currentActor ||
    (currentActor.browsingContext != message.data.browsingContext &&
      currentActor.browsingContext.top != message.data.browsingContext)
  ) {
    return false;
  }

  return true;
}

Services.ppmm.addMessageListener("AutoComplete:GetSelectedIndex", message => {
  if (compareContext(message)) {
    let actor = currentActor;
    if (actor && actor.openedPopup) {
      return actor.openedPopup.selectedIndex;
    }
  }

  return -1;
});

Services.ppmm.addMessageListener("AutoComplete:SelectBy", message => {
  if (compareContext(message)) {
    let actor = currentActor;
    if (actor && actor.openedPopup) {
      actor.openedPopup.selectBy(message.data.reverse, message.data.page);
    }
  }
});

var AutoCompleteResultView = {
  QueryInterface: ChromeUtils.generateQI([
    "nsIAutoCompleteController",
    "nsIAutoCompleteInput",
  ]),

  results: [],

  currentActor: null,

  get matchCount() {
    return this.results.length;
  },

  getValueAt(index) {
    return this.results[index].value;
  },

  getFinalCompleteValueAt(index) {
    return this.results[index].value;
  },

  getLabelAt(index) {
    return this.results[index].label;
  },

  getCommentAt(index) {
    return this.results[index].comment;
  },

  getStyleAt(index) {
    return this.results[index].style;
  },

  getImageAt(index) {
    return this.results[index].image;
  },

  handleEnter(aIsPopupSelection) {
    if (this.currentActor) {
      this.currentActor.handleEnter(aIsPopupSelection);
    }
  },

  stopSearch() {},

  searchString: "",

  get controller() {
    return this;
  },

  get popup() {
    return null;
  },

  _focus() {
    if (this.currentActor) {
      this.currentActor.requestFocus();
    }
  },

  clearResults() {
    this.currentActor = null;
    this.results = [];
  },

  setResults(actor, results) {
    this.currentActor = actor;
    this.results = results;
  },
};

export class AutoCompleteParent extends JSWindowActorParent {
  didDestroy() {
    if (this.openedPopup) {
      this.openedPopup.closePopup();
    }
  }

  static getCurrentActor() {
    return currentActor;
  }

  static addPopupStateListener(listener) {
    autoCompleteListeners.add(listener);
  }

  static removePopupStateListener(listener) {
    autoCompleteListeners.delete(listener);
  }

  handleEvent(evt) {
    switch (evt.type) {
      case "popupshowing": {
        this.sendAsyncMessage("AutoComplete:PopupOpened", {});
        break;
      }

      case "popuphidden": {
        let selectedIndex = this.openedPopup.selectedIndex;
        let selectedRowComment =
          selectedIndex != -1
            ? AutoCompleteResultView.getCommentAt(selectedIndex)
            : "";
        let selectedRowStyle =
          selectedIndex != -1
            ? AutoCompleteResultView.getStyleAt(selectedIndex)
            : "";

        this.clearAutoCompletePreview();

        this.sendAsyncMessage("AutoComplete:PopupClosed", {
          selectedRowComment,
          selectedRowStyle,
        });
        AutoCompleteResultView.clearResults();
        this.openedPopup.adjustHeight();
        this.openedPopup = null;
        currentActor = null;
        evt.target.removeEventListener("popuphidden", this);
        evt.target.removeEventListener("popupshowing", this);
        break;
      }
    }
  }

  showPopupWithResults({ rect, dir, results, selectedIndex }) {
    if (!results.length || this.openedPopup) {
      return;
    }

    if (!this.browsingContext.canOpenModalPicker) {
      return;
    }

    let browser = this.browsingContext.top.embedderElement;

    let tabbrowser = browser.getTabBrowser();
    if (tabbrowser && tabbrowser.selectedBrowser != browser) {
      return;
    }

    let resultStyles = new Set(results.map(r => r.style).filter(r => !!r));
    currentActor = this;
    this.openedPopup = browser.autoCompletePopup;
    this.openedPopup.setAttribute("resultstyles", [...resultStyles].join(" "));
    this.openedPopup.hidden = false;
    this.openedPopup.style.direction = dir;

    AutoCompleteResultView.setResults(this, results);

    this.openedPopup.view = AutoCompleteResultView;

    this.openedPopup.mInput = AutoCompleteResultView;
    browser.constrainPopup(this.openedPopup);
    this.openedPopup.addEventListener("popuphidden", this);
    this.openedPopup.addEventListener("popupshowing", this);
    this.openedPopup.openPopupAtScreenRect(
      "after_start",
      rect.left,
      rect.top,
      rect.width,
      rect.height,
      false,
      false
    );
    this.openedPopup.invalidate();
    this.openedPopup.selectedIndex = selectedIndex;
  }

  invalidate(results) {
    if (!this.openedPopup) {
      return;
    }

    if (!results.length) {
      this.closePopup();
    } else {
      AutoCompleteResultView.setResults(this, results);
      this.openedPopup.invalidate();
    }
  }

  closePopup() {
    if (this.openedPopup) {
      this.openedPopup.hidePopup();
    }
  }

  async receiveMessage(message) {
    let browser = this.browsingContext.top.embedderElement;

    if (
      !browser ||
      (!AppConstants.MOZ_GECKOVIEW && !browser.autoCompletePopup)
    ) {
      if (this.openedPopup) {
        this.openedPopup.closePopup();
      }

      return false;
    }

    switch (message.name) {
      case "AutoComplete:SelectEntry": {
        if (this.openedPopup) {
          this.selectAutoCompleteEntry();
        }
        break;
      }

      case "AutoComplete:SetSelectedIndex": {
        let { index } = message.data;
        if (this.openedPopup) {
          this.openedPopup.selectedIndex = index;
        }
        break;
      }

      case "AutoComplete:MaybeOpenPopup": {
        let {
          results,
          rect,
          dir,
          inputElementIdentifier,
          selectedIndex,
        } = message.data;
        if (AppConstants.MOZ_GECKOVIEW) {
          lazy.GeckoViewAutocomplete.delegateSelection({
            browsingContext: this.browsingContext,
            options: results,
            inputElementIdentifier,
          });
        } else {
          this.showPopupWithResults({
            results,
            rect,
            dir,
            selectedIndex,
          });
          this.notifyListeners();

          this.notifyAutoCompletePopupOpened(
            JSON.stringify(inputElementIdentifier)
          );
        }
        break;
      }

      case "AutoComplete:Invalidate": {
        let { results } = message.data;
        this.invalidate(results);
        break;
      }

      case "AutoComplete:ClosePopup": {
        if (AppConstants.MOZ_GECKOVIEW) {
          lazy.GeckoViewAutocomplete.delegateDismiss();
          break;
        }
        this.closePopup();
        break;
      }

      case "AutoComplete:StartSearch": {
        const { searchString, data } = message.data;
        const result = await this.#startSearch(searchString, data);
        return Promise.resolve(result);
      }
    }
    return false;
  }

  delayPopupInput() {
    if (!this.openedPopup) {
      return;
    }
    const popupDelay = Services.prefs.getIntPref(PREF_SECURITY_DELAY);

    if (!popupDelay) {
      return;
    }

    const items = Array.from(
      this.openedPopup.getElementsByTagName("richlistitem")
    );
    items.forEach(item => (item.disabled = true));

    let timerId;
    const delay = () => {
      if (timerId) {
        lazy.clearTimeout(timerId);
      }
      timerId = lazy.setTimeout(() => {
        items.forEach(item => (item.disabled = false));
        this.openedPopup?.removeEventListener("click", delay);
      }, popupDelay);
    };

    this.openedPopup.addEventListener("click", delay);
    delay();
  }

  notifyListeners() {
    let window = this.browsingContext.top.embedderElement.documentGlobal;
    for (let listener of autoCompleteListeners) {
      try {
        listener(window);
      } catch (ex) {
        console.error(ex);
      }
    }
  }

  handleEnter(aIsPopupSelection) {
    if (this.openedPopup) {
      this.sendAsyncMessage("AutoComplete:HandleEnter", {
        selectedIndex: this.openedPopup.selectedIndex,
        isPopupSelection: aIsPopupSelection,
      });
    }
  }

  async #startSearch(searchString, providers) {
    for (const provider of providers) {
      const { actorName, options } = provider;
      const actor =
        this.browsingContext.currentWindowGlobal.getActor(actorName);
      const entries = await actor?.searchAutoCompleteEntries(
        searchString,
        options
      );

      if (entries) {
        return [{ actorName, ...entries }];
      }
    }
    return [];
  }

  stopSearch() {}

  #getActorByMessagePrefix(message) {
    const prefixToActor = [
      { prefix: "PasswordManager", actor: "LoginManager" },
      { prefix: "FormAutofill", actor: "FormAutofill" },
    ];

    const name = prefixToActor.find(x => message.startsWith(x.prefix))?.actor;
    return this.browsingContext.currentWindowGlobal.getActor(name);
  }

  notifyAutoCompletePopupOpened(elementId) {
    const actors = new Set();
    for (const result of AutoCompleteResultView.results) {
      try {
        const { fillMessageName } = JSON.parse(result.comment);
        if (!fillMessageName) {
          continue;
        }

        actors.add(this.#getActorByMessagePrefix(fillMessageName));
      } catch {}
    }

    for (const actor of actors) {
      actor.onAutoCompletePopupOpened?.(elementId);
    }
  }

  clearAutoCompletePreview() {
    const selectedIndex = this.openedPopup?.selectedIndex;
    const result = AutoCompleteResultView.results[selectedIndex];
    if (!result) {
      return;
    }

    const { fillMessageName, fillMessageData } = JSON.parse(
      result.comment || "{}"
    );
    if (!fillMessageName) {
      return;
    }

    const actor = this.#getActorByMessagePrefix(fillMessageName);
    actor?.onAutoCompleteEntryClearPreview?.(fillMessageName, fillMessageData);
  }

  previewAutoCompleteEntry() {
    const selectedIndex = this.openedPopup?.selectedIndex;
    const result = AutoCompleteResultView.results[selectedIndex];
    if (!result) {
      return;
    }

    const { fillMessageName, fillMessageData } = JSON.parse(
      result.comment || "{}"
    );
    if (!fillMessageName) {
      return;
    }

    const actor = this.#getActorByMessagePrefix(fillMessageName);
    actor?.onAutoCompleteEntryHovered?.(fillMessageName, fillMessageData);
  }

  selectAutoCompleteEntry(secondary = false) {
    const selectedIndex = this.openedPopup?.selectedIndex;
    const result = AutoCompleteResultView.results[selectedIndex];
    if (!result) {
      return;
    }

    const parsedComment = JSON.parse(result.comment || "{}");
    const { fillMessageName, fillMessageData } = secondary
      ? (parsedComment.secondaryAction ?? {})
      : parsedComment;
    if (!fillMessageName) {
      return;
    }

    const actor = this.#getActorByMessagePrefix(fillMessageName);
    actor?.onAutoCompleteEntrySelected?.(fillMessageName, fillMessageData);
  }

  requestFocus() {
  }
}
