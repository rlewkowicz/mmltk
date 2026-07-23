/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


let { LangPackMatcher } = window.top;

ChromeUtils.defineESModuleGetters(this, {
  SelectionChangedMenulist:
    "resource:///modules/SelectionChangedMenulist.sys.mjs",
});


class OrderedListBox {
  constructor({
    richlistbox,
    upButton,
    downButton,
    removeButton,
    onRemove,
    onReorder,
  }) {
    this.richlistbox = richlistbox;
    this.upButton = upButton;
    this.downButton = downButton;
    this.removeButton = removeButton;
    this.onRemove = onRemove;
    this.onReorder = onReorder;

    this.items = [];

    this.richlistbox.addEventListener("select", () => this.setButtonState());
    this.upButton.addEventListener("command", () => this.moveUp());
    this.downButton.addEventListener("command", () => this.moveDown());
    this.removeButton.addEventListener("command", () => this.removeItem());
  }

  get selectedItem() {
    return this.items[this.richlistbox.selectedIndex];
  }

  setButtonState() {
    let { upButton, downButton, removeButton } = this;
    let { selectedIndex, itemCount } = this.richlistbox;
    upButton.disabled = selectedIndex <= 0;
    downButton.disabled = selectedIndex == itemCount - 1;
    removeButton.disabled = itemCount <= 1 || !this.selectedItem.canRemove;
  }

  moveUp() {
    let { selectedIndex } = this.richlistbox;
    if (selectedIndex == 0) {
      return;
    }
    let { items } = this;
    let selectedItem = items[selectedIndex];
    let prevItem = items[selectedIndex - 1];
    items[selectedIndex - 1] = items[selectedIndex];
    items[selectedIndex] = prevItem;
    let prevEl = document.getElementById(prevItem.id);
    let selectedEl = document.getElementById(selectedItem.id);
    this.richlistbox.insertBefore(selectedEl, prevEl);
    this.richlistbox.ensureElementIsVisible(selectedEl);
    this.setButtonState();

    this.onReorder();
  }

  moveDown() {
    let { selectedIndex } = this.richlistbox;
    if (selectedIndex == this.items.length - 1) {
      return;
    }
    let { items } = this;
    let selectedItem = items[selectedIndex];
    let nextItem = items[selectedIndex + 1];
    items[selectedIndex + 1] = items[selectedIndex];
    items[selectedIndex] = nextItem;
    let nextEl = document.getElementById(nextItem.id);
    let selectedEl = document.getElementById(selectedItem.id);
    this.richlistbox.insertBefore(nextEl, selectedEl);
    this.richlistbox.ensureElementIsVisible(selectedEl);
    this.setButtonState();

    this.onReorder();
  }

  removeItem() {
    let { selectedIndex } = this.richlistbox;

    if (selectedIndex == -1) {
      return;
    }

    let [item] = this.items.splice(selectedIndex, 1);
    this.richlistbox.selectedItem.remove();
    this.richlistbox.selectedIndex = Math.min(
      selectedIndex,
      this.richlistbox.itemCount - 1
    );
    this.richlistbox.ensureElementIsVisible(this.richlistbox.selectedItem);
    this.onRemove(item);
  }

  setItems(items) {
    this.items = items;
    this.populate();
    this.setButtonState();
  }

  addItem(item) {
    this.items.unshift(item);
    this.richlistbox.insertBefore(
      this.createItem(item),
      this.richlistbox.firstElementChild
    );
    this.richlistbox.selectedIndex = 0;
    this.richlistbox.ensureElementIsVisible(this.richlistbox.selectedItem);
  }

  populate() {
    this.richlistbox.textContent = "";

    let frag = document.createDocumentFragment();
    for (let item of this.items) {
      frag.appendChild(this.createItem(item));
    }
    this.richlistbox.appendChild(frag);

    this.richlistbox.selectedIndex = 0;
    this.richlistbox.ensureElementIsVisible(this.richlistbox.selectedItem);
  }

  createItem({ id, label, value }) {
    let listitem = document.createXULElement("richlistitem");
    listitem.id = id;
    listitem.setAttribute("value", value);

    let labelEl = document.createXULElement("label");
    labelEl.textContent = label;
    listitem.appendChild(labelEl);

    return listitem;
  }
}

class SortedItemSelectList {
  constructor({ menulist, button, onSelect, onChange, compareFn }) {
    this.menulist = menulist;

    this.popup = menulist.menupopup;

    this.button = button;

    this.compareFn = compareFn;

    this.items = [];

    new SelectionChangedMenulist(this.menulist, () => {
      button.disabled = !menulist.selectedItem;
      if (menulist.selectedItem) {
        onChange(this.items[menulist.selectedIndex]);
      }
    });
    button.addEventListener("command", () => {
      if (!menulist.selectedItem) {
        return;
      }

      let [item] = this.items.splice(menulist.selectedIndex, 1);
      menulist.selectedItem.remove();
      menulist.setAttribute("label", menulist.getAttribute("placeholder"));
      button.disabled = true;
      menulist.disabled = menulist.itemCount == 0;
      menulist.selectedIndex = -1;

      onSelect(item);
    });
  }

  setItems(items) {
    this.items = items.sort(this.compareFn);
    this.populate();
  }

  populate() {
    let { button, items, menulist, popup } = this;
    popup.textContent = "";

    let frag = document.createDocumentFragment();
    for (let item of items) {
      frag.appendChild(this.createItem(item));
    }
    popup.appendChild(frag);

    menulist.setAttribute("label", menulist.getAttribute("placeholder"));
    menulist.disabled = menulist.itemCount == 0;
    menulist.selectedIndex = -1;
    button.disabled = true;
  }

  addItem(item) {
    let { compareFn, items, menulist, popup } = this;

    let i = items.findIndex(el => compareFn(el, item) >= 0);
    items.splice(i, 0, item);
    popup.insertBefore(this.createItem(item), menulist.getItemAtIndex(i));

    menulist.disabled = menulist.itemCount == 0;
  }

  createItem({ label, value, className, disabled }) {
    let item = document.createXULElement("menuitem");
    item.setAttribute("label", label);
    if (value) {
      item.value = value;
    }
    if (className) {
      item.classList.add(className);
    }
    if (disabled) {
      item.setAttribute("disabled", "true");
    }
    return item;
  }

  disableWithMessageId(messageId) {
    document.l10n.setAttributes(this.menulist, messageId);
    this.menulist.setAttribute(
      "image",
      "chrome://global/skin/icons/loading.svg"
    );
    this.menulist.disabled = true;
    this.button.disabled = true;
  }

  enableWithMessageId(messageId) {
    document.l10n.setAttributes(this.menulist, messageId);
    this.menulist.removeAttribute("image");
    this.menulist.disabled = this.menulist.itemCount == 0;
    this.button.disabled = !this.menulist.selectedItem;
  }
}


async function getLocaleDisplayInfo(localeCodes) {
  let availableLocales = new Set(await LangPackMatcher.getAvailableLocales());
  let localeNames = Services.intl.getLocaleDisplayNames(
    undefined,
    localeCodes,
    { preferNative: true }
  );
  return localeCodes.map((code, i) => {
    return {
      id: "locale-" + code,
      label: localeNames[i],
      value: code,
      canRemove: code != Services.locale.defaultLocale,
      installed: availableLocales.has(code),
    };
  });
}

function compareItems(a, b) {
  if (a.installed != b.installed) {
    return a.installed ? -1 : 1;

  } else if (a.value == "search") {
    return 1;
  } else if (b.value == "search") {
    return -1;

  } else if (a.value && b.value) {
    return a.label.localeCompare(b.label);

  } else if (a.value) {
    return 1;
  }
  return -1;
}

var gBrowserLanguagesDialog = {
  selected: null,

  _telemetryId: null,

  _availableLocalesUI: null,

  _selectedLocalesUI: null,

  recordTelemetry(method, extra = {}) {
    extra.value = this._telemetryId;
  },

  async onLoad() {

    let { telemetryId, selectedLocalesForRestart } = window.arguments[0];

    this._telemetryId = telemetryId;

    let selectedLocales =
      selectedLocalesForRestart || Services.locale.appLocalesAsBCP47;
    let selectedLocaleSet = new Set(selectedLocales);
    let available = await LangPackMatcher.getAvailableLocales();
    let availableSet = new Set(available);

    selectedLocales = selectedLocales.filter(locale =>
      availableSet.has(locale)
    );
    available = available.filter(locale => !selectedLocaleSet.has(locale));

    await this.initSelectedLocales(selectedLocales);
    await this.initAvailableLocales(available);

    this.initialized = true;

    document
      .getElementById("BrowserLanguagesDialog")
      .addEventListener("beforeaccept", () => {
        this.selected = this._selectedLocalesUI.items.map(item => item.value);
      });
  },

  async initSelectedLocales(selectedLocales) {
    this._selectedLocalesUI = new OrderedListBox({
      richlistbox: document.getElementById("selectedLocales"),
      upButton: document.getElementById("up"),
      downButton: document.getElementById("down"),
      removeButton: document.getElementById("remove"),
      onRemove: item => this.selectedLocaleRemoved(item),
      onReorder: () => this.recordTelemetry("reorder"),
    });
    this._selectedLocalesUI.setItems(
      await getLocaleDisplayInfo(selectedLocales)
    );
  },

  async initAvailableLocales(available) {
    this._availableLocalesUI = new SortedItemSelectList({
      menulist: document.getElementById("availableLocales"),
      button: document.getElementById("add"),
      compareFn: compareItems,
      onSelect: item => this.availableLanguageSelected(item),
      onChange: () => this.hideError(),
    });

    await this.loadLocalesFromInstalled(available);
  },

  async loadLocalesFromInstalled(available) {
    let items;
    if (available.length) {
      items = await getLocaleDisplayInfo(available);
      items.push(await this.createInstalledLabel());
    } else {
      items = [];
    }
    this._availableLocalesUI.setItems(items);
  },

  async availableLanguageSelected(item) {
    if ((await LangPackMatcher.getAvailableLocales()).includes(item.value)) {
      this.recordTelemetry("add");
      await this.requestLocalLanguage(item);
    } else {
      this.showError();
    }
  },

  async requestLocalLanguage(item) {
    this._selectedLocalesUI.addItem(item);
    let selectedCount = this._selectedLocalesUI.items.length;
    let availableCount = (await LangPackMatcher.getAvailableLocales()).length;
    if (selectedCount == availableCount) {
      this._availableLocalesUI.items.shift();
      this._availableLocalesUI.setItems(this._availableLocalesUI.items);
    }
    this._availableLocalesUI.enableWithMessageId(
      "browser-languages-select-language"
    );
  },

  showError() {
    document.getElementById("warning-message").hidden = false;
    this._availableLocalesUI.enableWithMessageId(
      "browser-languages-select-language"
    );

    requestAnimationFrame(() => {
      let dialogs = window.opener.gSubDialog._dialogs;
      let index = dialogs.findIndex(d => d._frame.contentDocument == document);
      if (index != -1) {
        dialogs[index].resizeDialog();
      }
    });
  },

  hideError() {
    document.getElementById("warning-message").hidden = true;
  },

  async selectedLocaleRemoved(item) {
    this.recordTelemetry("remove");

    this._availableLocalesUI.addItem(item);

    if (this._availableLocalesUI.items[0] == item) {
      this._availableLocalesUI.addItem(await this.createInstalledLabel());
    }
  },

  async createInstalledLabel() {
    return {
      label: await document.l10n.formatValue(
        "browser-languages-installed-label"
      ),
      className: "label-item",
      disabled: true,
      installed: true,
    };
  },
};

window.addEventListener("load", () => gBrowserLanguagesDialog.onLoad());
