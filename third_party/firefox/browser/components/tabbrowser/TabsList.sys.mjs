/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PanelMultiView:
    "moz-src:///browser/components/customizableui/PanelMultiView.sys.mjs",
});

const TAB_DROP_TYPE = "application/x-moz-tabbrowser-tab";

const ROW_VARIANT_TAB = "tab";
const ROW_VARIANT_TAB_GROUP = "tab-group";

function setAttributes(element, attrs) {
  for (let [name, value] of Object.entries(attrs)) {
    if (value) {
      element.setAttribute(name, value);
    } else {
      element.removeAttribute(name);
    }
  }
}

function getTabFromRow(element) {
  return element.closest("toolbaritem")?._tab;
}

function getTabGroupFromRow(element) {
  return element.closest("toolbaritem")?._tabGroup;
}

function getRowVariant(element) {
  return element.closest("toolbaritem")?.getAttribute("row-variant");
}

class TabsListBase {
  get domRefreshComplete() {
    return this.#domRefreshPromise ?? Promise.resolve();
  }

  #domRefreshPromise;

  tabToElement = new Map();

  constructor({
    className,
    filterFn,
    containerNode,
    dropIndicator = null,
    onlyHiddenTabs,
  }) {
    this.className = className;
    this.filterFn = onlyHiddenTabs
      ? tab => filterFn(tab) && tab.hidden
      : filterFn;
    this.containerNode = containerNode;
    this.dropIndicator = dropIndicator;

    if (this.dropIndicator) {
      this.dropTargetRow = null;
      this.dropTargetDirection = 0;
    }

    this.doc = containerNode.ownerDocument;
    this.gBrowser = this.doc.defaultView.gBrowser;
    this.listenersRegistered = false;
    this.onlyHiddenTabs = onlyHiddenTabs;
  }

  get rows() {
    return this.tabToElement.values();
  }

  handleEvent(event) {
    switch (event.type) {
      case "TabAttrModified":
        this._tabAttrModified(event.target);
        break;
      case "TabClose":
        this._tabClose(event.target);
        break;
      case "TabGroupCollapse":
      case "TabGroupExpand":
      case "TabGroupCreate":
      case "TabGroupRemoved":
      case "TabGrouped":
      case "TabGroupMoved":
      case "TabUngrouped":
        this._refreshDOM();
        break;
      case "TabMove":
        this._moveTab(event.target);
        break;
      case "TabPinned":
        if (!this.filterFn(event.target)) {
          this._tabClose(event.target);
        }
        break;
      case "command":
        this.#handleCommand(event);
        break;
      case "dragstart":
        this._onDragStart(event);
        break;
      case "dragover":
        this._onDragOver(event);
        break;
      case "dragleave":
        this._onDragLeave(event);
        break;
      case "dragend":
        this._onDragEnd(event);
        break;
      case "drop":
        this._onDrop(event);
        break;
      case "click":
        this._onClick(event);
        break;
    }
  }

  #handleCommand(event) {
    if (event.target.classList.contains("all-tabs-mute-button")) {
      getTabFromRow(event.target)?.toggleMuteAudio();
    } else if (event.target.classList.contains("all-tabs-close-button")) {
      const tab = getTabFromRow(event.target);
      if (tab) {
        this.gBrowser.removeTab(tab);
      }
    } else {
      const rowVariant = getRowVariant(event.target);
      if (rowVariant == ROW_VARIANT_TAB) {
        const tab = getTabFromRow(event.target);
        if (tab) {
          this._selectTab(tab);
        }
      } else if (rowVariant == ROW_VARIANT_TAB_GROUP) {
        getTabGroupFromRow(event.target)?.select();
      }
    }
  }

  _selectTab(tab) {
    if (this.gBrowser.selectedTab != tab) {
      this.gBrowser.setSelectedTab(tab);
    } else {
      this.gBrowser.tabContainer._handleTabSelect();
    }
  }

  _populate() {
    this._populateDOM();
    this._setupListeners();
  }

  _populateDOM() {
    let fragment = this.doc.createDocumentFragment();
    let currentGroupId;

    for (let tab of this.gBrowser.tabs) {
      if (this.filterFn(tab)) {
        if (tab.group && tab.group.id != currentGroupId) {
          fragment.appendChild(this._createGroupRow(tab.group));
          currentGroupId = tab.group.id;
        }

        let tabHiddenByGroup = tab.group?.collapsed && !tab.selected;
        if (!tabHiddenByGroup || this.onlyHiddenTabs) {
          fragment.appendChild(this._createRow(tab));
        }
      }
    }

    this._addElement(fragment);
  }

  _addElement(elementOrFragment) {
    this.containerNode.appendChild(elementOrFragment);
  }

  _cleanup() {
    this._cleanupDOM();
    this._cleanupListeners();
    this._clearDropTarget();
  }

  _cleanupDOM() {
    this.containerNode
      .querySelectorAll(":scope toolbaritem")
      .forEach(node => node.remove());
    this.tabToElement = new Map();
  }

  _refreshDOM() {
    if (!this.#domRefreshPromise) {
      this.#domRefreshPromise = new Promise(resolve => {
        this.containerNode.documentGlobal.requestAnimationFrame(() => {
          if (this.#domRefreshPromise) {
            if (this.listenersRegistered) {
              this._cleanupDOM();
              this._populateDOM();
            }
            resolve();
            this.#domRefreshPromise = undefined;
          }
        });
      });
    }
  }

  _setupListeners() {
    this.listenersRegistered = true;

    this.gBrowser.tabContainer.addEventListener("TabAttrModified", this);
    this.gBrowser.tabContainer.addEventListener("TabClose", this);
    this.gBrowser.tabContainer.addEventListener("TabMove", this);
    this.gBrowser.tabContainer.addEventListener("TabPinned", this);
    this.gBrowser.tabContainer.addEventListener("TabGroupCollapse", this);
    this.gBrowser.tabContainer.addEventListener("TabGroupExpand", this);
    this.gBrowser.tabContainer.addEventListener("TabGroupCreate", this);
    this.gBrowser.tabContainer.addEventListener("TabGroupRemoved", this);
    this.gBrowser.tabContainer.addEventListener("TabGroupMoved", this);
    this.gBrowser.tabContainer.addEventListener("TabGrouped", this);
    this.gBrowser.tabContainer.addEventListener("TabUngrouped", this);

    this.containerNode.addEventListener("click", this);
    this.containerNode.addEventListener("command", this);

    if (this.dropIndicator) {
      this.containerNode.addEventListener("dragstart", this);
      this.containerNode.addEventListener("dragover", this);
      this.containerNode.addEventListener("dragleave", this);
      this.containerNode.addEventListener("dragend", this);
      this.containerNode.addEventListener("drop", this);
    }
  }

  _cleanupListeners() {
    this.gBrowser.tabContainer.removeEventListener("TabAttrModified", this);
    this.gBrowser.tabContainer.removeEventListener("TabClose", this);
    this.gBrowser.tabContainer.removeEventListener("TabMove", this);
    this.gBrowser.tabContainer.removeEventListener("TabPinned", this);
    this.gBrowser.tabContainer.removeEventListener("TabGroupCollapse", this);
    this.gBrowser.tabContainer.removeEventListener("TabGroupExpand", this);
    this.gBrowser.tabContainer.removeEventListener("TabGroupCreate", this);
    this.gBrowser.tabContainer.removeEventListener("TabGroupRemoved", this);
    this.gBrowser.tabContainer.removeEventListener("TabGroupMoved", this);
    this.gBrowser.tabContainer.removeEventListener("TabGrouped", this);
    this.gBrowser.tabContainer.removeEventListener("TabUngrouped", this);

    this.containerNode.removeEventListener("click", this);
    this.containerNode.removeEventListener("command", this);

    if (this.dropIndicator) {
      this.containerNode.removeEventListener("dragstart", this);
      this.containerNode.removeEventListener("dragover", this);
      this.containerNode.removeEventListener("dragleave", this);
      this.containerNode.removeEventListener("dragend", this);
      this.containerNode.removeEventListener("drop", this);
    }

    this.listenersRegistered = false;
  }

  _tabAttrModified(tab) {
    let item = this.tabToElement.get(tab);
    if (item) {
      if (!this.filterFn(tab)) {
        this._removeItem(item, tab);
      } else {
        this._setRowAttributes(item, tab);
      }
    } else if (this.filterFn(tab)) {
      this._addTab(tab);
    }
  }

  _moveTab(tab) {
    for (let t of tab.splitview?.tabs ?? [tab]) {
      let item = this.tabToElement.get(t);
      if (item) {
        this._removeItem(item, t);
        this._addTab(t);
      }
    }
  }

  _addTab(newTab) {
    if (!this.filterFn(newTab)) {
      return;
    }
    if (newTab.group?.collapsed && !this.onlyHiddenTabs) {
      return;
    }

    let newRow = this._createRow(newTab);
    let nextTab = this.gBrowser.tabContainer.findNextTab(newTab, {
      filter: this.filterFn,
    });
    if (!nextTab) {
      this._addElement(newRow);
    } else if (!newTab.group && nextTab.group) {
      let nextTabTabGroupRow = this.containerNode.querySelector(
        `:scope [tab-group-id="${nextTab.group.id}"]`
      );
      this.containerNode.insertBefore(newRow, nextTabTabGroupRow);
    } else {
      let nextRow = this.tabToElement.get(nextTab);
      if (!nextRow) {
        this._addElement(newRow);
      } else {
        this.containerNode.insertBefore(newRow, nextRow);
      }
    }
  }

  _tabClose(tab) {
    let item = this.tabToElement.get(tab);
    if (item) {
      this._removeItem(item, tab);
    }
  }

  _removeItem(item, tab) {
    this.tabToElement.delete(tab);
    item.remove();
    if (
      tab.group &&
      !this.tabToElement.keys().some(t => t.group == tab.group)
    ) {
      this.containerNode
        .querySelector(`:scope [tab-group-id="${tab.group.id}"]`)
        ?.remove();
    }
  }
}

const TABS_PANEL_EVENTS = {
  show: "ViewShowing",
  hide: "PanelMultiViewHidden",
};

export class TabsPanel extends TabsListBase {
  constructor(opts) {
    super({
      ...opts,
      containerNode: opts.containerNode || opts.view.firstElementChild,
    });
    this.view = opts.view;
    this.view.addEventListener(TABS_PANEL_EVENTS.show, this);
    this.panelMultiView = null;
  }

  handleEvent(event) {
    switch (event.type) {
      case TABS_PANEL_EVENTS.hide:
        if (event.target == this.panelMultiView) {
          this._cleanup();
          this.panelMultiView = null;
        }
        break;
      case TABS_PANEL_EVENTS.show:
        if (!this.listenersRegistered && event.target == this.view) {
          this.panelMultiView = this.view.panelMultiView;
          this._populate(event);
          this.gBrowser.translateTabContextMenu();
        }
        break;
      default:
        super.handleEvent(event);
        break;
    }
  }

  _populate(event) {
    super._populate(event);

    for (let row of this.rows) {
      if (getRowVariant(row) == ROW_VARIANT_TAB) {
        this._setImageAttributes(row, getTabFromRow(row));
      }
    }
  }

  _selectTab(tab) {
    super._selectTab(tab);
    lazy.PanelMultiView.hidePopup(this.view.closest("panel"));
  }

  _setupListeners() {
    super._setupListeners();
    this.panelMultiView.addEventListener(TABS_PANEL_EVENTS.hide, this);
  }

  _cleanupListeners() {
    super._cleanupListeners();
    this.panelMultiView.removeEventListener(TABS_PANEL_EVENTS.hide, this);
  }

  _createRow(tab) {
    let { doc } = this;
    let row = doc.createXULElement("toolbaritem");
    row.setAttribute("class", "all-tabs-item");
    if (this.className) {
      row.classList.add(this.className);
    }
    row.setAttribute("context", "tabContextMenu");
    row.setAttribute("row-variant", ROW_VARIANT_TAB);

    row._tab = tab;
    this.tabToElement.set(tab, row);

    let button = doc.createXULElement("toolbarbutton");
    button.setAttribute(
      "class",
      "all-tabs-button subviewbutton subviewbutton-iconic"
    );
    button.setAttribute("flex", "1");
    button.setAttribute("crop", "end");

    button.tab = tab;

    if (tab.userContextId) {
      tab.classList.forEach(property => {
        if (property.startsWith("identity-color")) {
          button.classList.add(property);
          button.classList.add("all-tabs-container-indicator");
        }
      });
    }

    if (tab.group) {
      row.classList.add("grouped");
    }

    row.appendChild(button);

    let muteButton = doc.createXULElement("toolbarbutton");
    muteButton.classList.add(
      "all-tabs-mute-button",
      "all-tabs-secondary-button",
      "subviewbutton"
    );
    muteButton.setAttribute("closemenu", "none");
    row.appendChild(muteButton);

    if (!tab.pinned) {
      let closeButton = doc.createXULElement("toolbarbutton");
      closeButton.classList.add(
        "all-tabs-close-button",
        "all-tabs-secondary-button",
        "subviewbutton"
      );
      closeButton.setAttribute("closemenu", "none");
      doc.l10n.setAttributes(closeButton, "tabbrowser-manager-close-tab");
      row.appendChild(closeButton);
    }

    this._setRowAttributes(row, tab);

    return row;
  }

  _createGroupRow(group) {
    let { doc } = this;
    let row = doc.createXULElement("toolbaritem");
    row.setAttribute("class", "all-tabs-item all-tabs-group-item");
    row.setAttribute("row-variant", ROW_VARIANT_TAB_GROUP);
    row.setAttribute("tab-group-id", group.id);
    row._tabGroup = group;

    row.style.setProperty(
      "--tab-group-color",
      `var(--tab-group-${group.color})`
    );
    row.style.setProperty(
      "--tab-group-color-invert",
      `var(--tab-group-${group.color}-invert)`
    );
    row.style.setProperty(
      "--tab-group-color-pale",
      `var(--tab-group-${group.color}-pale)`
    );
    row.style.setProperty(
      "--tab-group-background-color",
      `var(--tab-group-${group.color})`
    );

    let button = doc.createXULElement("toolbarbutton");
    button.setAttribute("context", "open-tab-group-context-menu");
    button.classList.add(
      "all-tabs-button",
      "all-tabs-group-button",
      "subviewbutton",
      "subviewbutton-iconic",
      "tab-group-icon"
    );
    if (group.collapsed) {
      button.classList.add("tab-group-icon-collapsed");
    }
    button.setAttribute("flex", "1");
    button.setAttribute("crop", "end");

    let setName = tabGroupName => {
      doc.l10n.setAttributes(
        button,
        "tabbrowser-manager-current-window-tab-group",
        { tabGroupName }
      );
    };

    if (group.label) {
      setName(group.label);
    } else {
      doc.l10n
        .formatValues([{ id: "tab-group-name-default" }])
        .then(([msg]) => {
          setName(msg);
        });
    }
    row.appendChild(button);
    return row;
  }

  _setRowAttributes(row, tab) {
    setAttributes(row, { selected: tab.selected });

    let tooltiptext = this.gBrowser.getTabTooltip(tab);
    let busy = tab.getAttribute("busy");
    let button = row.firstElementChild;
    setAttributes(button, {
      busy,
      label: tab.label,
      tooltiptext,
      image: !busy && tab.getAttribute("image"),
    });

    this._setImageAttributes(row, tab);

    let muteButton = row.querySelector(".all-tabs-mute-button");
    let muteButtonTooltipString = tab.muted
      ? "tabbrowser-manager-unmute-tab"
      : "tabbrowser-manager-mute-tab";
    this.doc.l10n.setAttributes(muteButton, muteButtonTooltipString);

    setAttributes(muteButton, {
      muted: tab.muted,
      soundplaying: tab.soundPlaying,
      hidden: !(tab.muted || tab.soundPlaying),
    });
  }

  _setImageAttributes(row, tab) {
    let button = row.firstElementChild;
    let image = button.icon;

    if (image) {
      let busy = tab.getAttribute("busy");
      let progress = tab.getAttribute("progress");
      setAttributes(image, { busy, progress });
      if (busy) {
        image.classList.add("tab-throbber-tabslist");
      } else {
        image.classList.remove("tab-throbber-tabslist");
      }
    }
  }

  _onDragStart(event) {
    const row = this._getTargetRowFromEvent(event);
    if (!row) {
      return;
    }

    const elementToDrag =
      getRowVariant(row) == ROW_VARIANT_TAB_GROUP
        ? getTabGroupFromRow(row).labelElement
        : getTabFromRow(row);

    this.gBrowser.tabContainer.tabDragAndDrop.startTabDrag(
      event,
      elementToDrag.splitview || elementToDrag,
      {
        fromTabList: true,
      }
    );
  }

  _getTargetRowFromEvent(event) {
    return event.target.closest("toolbaritem");
  }

  _isMovingTabs(event) {
    var effects =
      this.gBrowser.tabContainer.tabDragAndDrop.getDropEffectForTabDrag(event);
    return effects == "move";
  }

  _onDragOver(event) {
    if (!this._isMovingTabs(event)) {
      return;
    }

    if (!this._updateDropTarget(event)) {
      return;
    }

    event.preventDefault();
    event.stopPropagation();
  }

  _getRowIndex(row) {
    return Array.prototype.indexOf.call(this.containerNode.children, row);
  }

  _onDrop(event) {
    if (!this._isMovingTabs(event)) {
      return;
    }

    if (!this._updateDropTarget(event)) {
      return;
    }

    event.preventDefault();
    event.stopPropagation();

    let draggedElement = event.dataTransfer.mozGetDataAt(TAB_DROP_TYPE, 0);
    let targetElement =
      getRowVariant(this.dropTargetRow) == ROW_VARIANT_TAB_GROUP
        ? getTabGroupFromRow(this.dropTargetRow).labelElement
        : getTabFromRow(this.dropTargetRow);

    if (draggedElement === targetElement) {
      this._clearDropTarget();
      return;
    }

    if (this.dropTargetDirection == -1) {
      this.gBrowser.moveTabBefore(draggedElement, targetElement);
    } else {
      this.gBrowser.moveTabAfter(draggedElement, targetElement);
    }

    this._clearDropTarget();
  }

  _onDragLeave(event) {
    if (!this._isMovingTabs(event)) {
      return;
    }

    let target = event.relatedTarget;
    while (target && target != this.containerNode) {
      target = target.parentNode;
    }
    if (target) {
      return;
    }

    this._clearDropTarget();
  }

  _onDragEnd(event) {
    if (!this._isMovingTabs(event)) {
      return;
    }

    this._clearDropTarget();
  }

  _updateDropTarget(event) {
    const row = this._getTargetRowFromEvent(event);
    if (!row) {
      return false;
    }

    const rect = row.getBoundingClientRect();
    const index = this._getRowIndex(row);
    if (index === -1) {
      return false;
    }

    const threshold = rect.height * 0.5;
    const direction = event.clientY < rect.top + threshold ? -1 : 0;
    if (
      getRowVariant(row) === ROW_VARIANT_TAB &&
      getTabFromRow(row).splitview
    ) {
      const tab = getTabFromRow(row);
      if (tab == tab.splitview.tabs[0]) {
        this._setDropTarget(row, -1);
      } else if (tab == tab.splitview.tabs[1]) {
        this._setDropTarget(row, 0);
      }
    } else {
      this._setDropTarget(row, direction);
    }

    return true;
  }

  _setDropTarget(row, direction) {
    this.dropTargetRow = row;
    this.dropTargetDirection = direction;

    const holder = this.dropIndicator.parentNode;
    const holderOffset = holder.getBoundingClientRect().top;

    let top;
    if (this.dropTargetDirection === -1) {
      if (this.dropTargetRow.previousSibling) {
        const rect = this.dropTargetRow.previousSibling.getBoundingClientRect();
        top = rect.top + rect.height;
      } else {
        const rect = this.dropTargetRow.getBoundingClientRect();
        top = rect.top;
      }
    } else {
      const rect = this.dropTargetRow.getBoundingClientRect();
      top = rect.top + rect.height;
    }

    const indicatorHeight = 12;
    const subViewBody = holder.parentNode;
    const subViewBodyRect = subViewBody.getBoundingClientRect();
    top = Math.min(top, subViewBodyRect.bottom - indicatorHeight);

    this.dropIndicator.style.top = `${top - holderOffset - 12}px`;
    this.dropIndicator.collapsed = false;
  }

  _clearDropTarget() {
    if (this.dropTargetRow) {
      this.dropTargetRow = null;
    }

    if (this.dropIndicator) {
      this.dropIndicator.style.top = `0px`;
      this.dropIndicator.collapsed = true;
    }
  }

  _onClick(event) {
    if (event.button == 1) {
      const row = this._getTargetRowFromEvent(event);
      if (!row) {
        return;
      }

      const rowVariant = getRowVariant(row);

      if (rowVariant == ROW_VARIANT_TAB) {
        const tab = getTabFromRow(row);
        this.gBrowser.removeTab(tab, { animate: true });
      } else if (rowVariant == ROW_VARIANT_TAB_GROUP) {
        getTabGroupFromRow(row)?.saveAndClose();
      }
    }
  }
}
