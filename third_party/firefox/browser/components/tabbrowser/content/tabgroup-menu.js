/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

{
  const { TabStateFlusher } = ChromeUtils.importESModule(
    "resource:///modules/sessionstore/TabStateFlusher.sys.mjs"
  );

  class MozTabbrowserTabGroupMenu extends MozXULElement {
    static COLORS = [
      "blue",
      "purple",
      "cyan",
      "orange",
      "yellow",
      "pink",
      "green",
      "gray",
      "red",
    ];

    static MESSAGE_IDS = {
      blue: "tab-group-editor-color-selector2-blue",
      purple: "tab-group-editor-color-selector2-purple",
      cyan: "tab-group-editor-color-selector2-cyan",
      orange: "tab-group-editor-color-selector2-orange",
      yellow: "tab-group-editor-color-selector2-yellow",
      pink: "tab-group-editor-color-selector2-pink",
      green: "tab-group-editor-color-selector2-green",
      gray: "tab-group-editor-color-selector2-gray",
      red: "tab-group-editor-color-selector2-red",
    };

    static markup =  `
      <panel
        type="arrow"
        class="tab-group-editor-panel panel-no-padding"
        orient="vertical"
        role="dialog"
        ignorekeys="true"
        norolluponanchor="true">
        <html:div class="panel-header">
          <html:h1
            id="tab-group-editor-title-create"
            class="tab-group-create-mode-only"
            data-l10n-id="tab-group-editor-title-create">
          </html:h1>
          <html:h1
            id="tab-group-editor-title-edit"
            class="tab-group-edit-mode-only"
            data-l10n-id="tab-group-editor-title-edit">
          </html:h1>
        </html:div>

        <toolbarseparator />

        <html:div class="tab-group-editor-name">
          <html:label
            for="tab-group-name"
            data-l10n-id="tab-group-editor-name-label">
          </html:label>
          <html:input
            id="tab-group-name"
            type="text"
            name="tab-group-name"
            value=""
            data-l10n-id="tab-group-editor-name-field"
          />
        </html:div>

        <html:div class="panel-subview-body">
          <html:div
            class="tab-group-editor-swatches"
            role="radiogroup"
            data-l10n-id="tab-group-editor-color-selector"
          />

          <toolbarseparator class="tab-group-edit-mode-only"/>

          <html:div
            class="tab-group-edit-actions tab-group-edit-mode-only">
            <toolbarbutton
              tabindex="0"
              id="tabGroupEditor_addNewTabInGroup"
              class="subviewbutton"
              data-l10n-id="tab-group-editor-action-new-tab">
            </toolbarbutton>
            <toolbarbutton
              tabindex="0"
              id="tabGroupEditor_moveGroupToNewWindow"
              class="subviewbutton"
              data-l10n-id="tab-group-editor-action-new-window">
            </toolbarbutton>
            <toolbarbutton
              tabindex="0"
              id="tabGroupEditor_copyAllLinks"
              class="subviewbutton">
            </toolbarbutton>
            <toolbarbutton
              tabindex="0"
              id="tabGroupEditor_saveAndCloseGroup"
              class="subviewbutton"
              data-l10n-id="tab-group-editor-action-save">
            </toolbarbutton>
            <toolbarbutton
              tabindex="0"
              id="tabGroupEditor_ungroupTabs"
              class="subviewbutton"
              data-l10n-id="tab-group-editor-action-ungroup">
            </toolbarbutton>
            <toolbarseparator class="tab-group-edit-mode-only" />
            <toolbarbutton
              tabindex="0"
              id="tabGroupEditor_deleteGroup"
              class="subviewbutton"
              data-l10n-id="tab-group-editor-action-delete">
            </toolbarbutton>
          </html:div>

          <html:moz-button-group
            class="tab-group-create-actions tab-group-create-mode-only panel-footer">
            <html:moz-button
              id="tab-group-editor-button-cancel"
              data-l10n-id="tab-group-editor-cancel">
            </html:moz-button>
            <html:moz-button
              type="primary"
              id="tab-group-editor-button-create"
              data-l10n-id="tab-group-editor-done">
            </html:moz-button>
          </html:moz-button-group>
        </html:div>
      </panel>
    `;

    #activeGroup;
    #cancelButton;
    #commandButtons;
    #createButton;
    #createMode;
    #keepNewlyCreatedGroup;
    #nameField;
    #panel;
    #swatches;
    #swatchesContainer;

    connectedCallback() {
      if (this._initialized) {
        return;
      }

      this.textContent = "";
      this.appendChild(this.constructor.fragment);
      this.initializeAttributeInheritance();
      this._initialized = true;

      this.#cancelButton = this.querySelector(
        "#tab-group-editor-button-cancel"
      );
      this.#createButton = this.querySelector(
        "#tab-group-editor-button-create"
      );
      this.#panel = this.querySelector("panel");
      this.#nameField = this.querySelector("#tab-group-name");
      this.#swatchesContainer = this.querySelector(
        ".tab-group-editor-swatches"
      );

      this.#panel.addEventListener("click", event => {
        if (event.target !== this.#nameField) {
          this.#nameField.blur();
        }
      });

      this.#populateSwatches();

      this.#cancelButton.addEventListener("click", () => this.close(false));
      this.#createButton.addEventListener("click", () => this.close());
      this.#nameField.addEventListener("input", () => {
        if (this.activeGroup) {
          this.activeGroup.label = this.#nameField.value;
        }
      });

      this.#commandButtons = {
        addNewTabInGroup: this.querySelector(
          "#tabGroupEditor_addNewTabInGroup"
        ),
        moveGroupToNewWindow: this.querySelector(
          "#tabGroupEditor_moveGroupToNewWindow"
        ),
        copyAllLinks: this.querySelector("#tabGroupEditor_copyAllLinks"),
        ungroupTabs: this.querySelector("#tabGroupEditor_ungroupTabs"),
        saveAndCloseGroup: this.querySelector(
          "#tabGroupEditor_saveAndCloseGroup"
        ),
        deleteGroup: this.querySelector("#tabGroupEditor_deleteGroup"),
      };

      this.#commandButtons.addNewTabInGroup.addEventListener("command", () =>
        this.#handleNewTabInGroup()
      );
      this.#commandButtons.moveGroupToNewWindow.addEventListener(
        "command",
        () => gBrowser.replaceGroupWithWindow(this.activeGroup)
      );
      this.#commandButtons.copyAllLinks.addEventListener("command", () => {
        let links = this.#getGroupLinks(this.activeGroup);
        if (links.length) {
          BrowserUtils.copyLinks(links);
        }
        this.close();
      });
      this.#commandButtons.ungroupTabs.addEventListener("command", () =>
        this.activeGroup.ungroupTabs()
      );
      this.#commandButtons.saveAndCloseGroup.addEventListener("command", () =>
        this.activeGroup.saveAndClose()
      );
      this.#commandButtons.deleteGroup.addEventListener("command", () =>
        gBrowser.removeTabGroup(this.activeGroup)
      );

      this.#panel.addEventListener("popupshown", this);
      this.#panel.addEventListener("popuphidden", this);
      this.#panel.addEventListener("keypress", this);
      this.#swatchesContainer.addEventListener("change", this);
    }

    #populateSwatches() {
      this.#swatchesContainer.replaceChildren();
      this.#swatches = [];
      for (let colorCode of MozTabbrowserTabGroupMenu.COLORS) {
        let input = document.createElement("input");
        input.id = `tab-group-editor-swatch-${colorCode}`;
        input.type = "radio";
        input.name = "tab-group-color";
        input.value = colorCode;

        let label = document.createElement("label");
        label.classList.add("tab-group-editor-swatch");
        label.setAttribute(
          "data-l10n-id",
          MozTabbrowserTabGroupMenu.MESSAGE_IDS[colorCode]
        );
        label.htmlFor = input.id;
        label.style.setProperty(
          "--tabgroup-swatch-color",
          `var(--tab-group-${colorCode})`
        );
        label.style.setProperty(
          "--tabgroup-swatch-color-invert",
          `var(--tab-group-${colorCode}-invert)`
        );
        this.#swatchesContainer.append(input, label);
        this.#swatches.push(input);
      }
    }

    get createMode() {
      return this.#createMode;
    }

    set createMode(enableCreateMode) {
      this.#panel.classList.toggle(
        "tab-group-editor-mode-create",
        enableCreateMode
      );
      this.#panel.setAttribute(
        "aria-labelledby",
        enableCreateMode
          ? "tab-group-editor-title-create"
          : "tab-group-editor-title-edit"
      );
      this.#commandButtons.copyAllLinks.hidden = enableCreateMode;
      this.#createMode = enableCreateMode;
    }

    get activeGroup() {
      return this.#activeGroup;
    }

    set activeGroup(group = null) {
      this.#activeGroup = group;
      this.#nameField.value = group?.label ?? "";
      for (let swatch of this.#swatches) {
        swatch.checked = !!group && swatch.value == group.color;
      }
    }

    get nextUnusedColor() {
      let usedColors = new Set(
        gBrowser.getAllTabGroups().map(group => group.color)
      );
      let color = MozTabbrowserTabGroupMenu.COLORS.find(
        colorCode => !usedColors.has(colorCode)
      );
      if (color) {
        return color;
      }
      return MozTabbrowserTabGroupMenu.COLORS[
        Math.floor(Math.random() * MozTabbrowserTabGroupMenu.COLORS.length)
      ];
    }

    get panel() {
      return this.#panel;
    }

    get #panelPosition() {
      if (gBrowser.tabContainer.verticalMode) {
        return SidebarController._positionStart
          ? "topleft topright"
          : "topright topleft";
      }
      return "bottomleft topleft";
    }

    openCreateModal(group) {
      this.activeGroup = group;
      this.createMode = true;
      this.#panel.openPopup(group.firstChild, {
        position: this.#panelPosition,
      });
    }

    openEditModal(group) {
      this.activeGroup = group;
      this.createMode = false;
      this.#panel.openPopup(group.firstChild, {
        position: this.#panelPosition,
      });

      this.#commandButtons.moveGroupToNewWindow.disabled =
        gBrowser.openTabs.length == this.activeGroup?.tabs.length;
      let linkCount = this.#getGroupLinks(this.activeGroup).length;
      document.l10n.setAttributes(
        this.#commandButtons.copyAllLinks,
        "tab-group-editor-action-copy-links",
        { linkCount }
      );
      this.#commandButtons.copyAllLinks.disabled = !linkCount;
      this.#maybeDisableOrHideSaveButton();
    }

    #maybeDisableOrHideSaveButton() {
      let saveAndCloseGroup = this.#commandButtons.saveAndCloseGroup;
      if (PrivateBrowsingUtils.isWindowPrivate(this.documentGlobal)) {
        saveAndCloseGroup.hidden = true;
        return;
      }

      let flushes = this.activeGroup.tabs.map(tab =>
        TabStateFlusher.flush(tab.linkedBrowser)
      );
      Promise.allSettled(flushes).then(() => {
        if (this.activeGroup?.tabs) {
          saveAndCloseGroup.disabled = !SessionStore.shouldSaveTabsToGroup(
            this.activeGroup.tabs
          );
        }
      });
    }

    close(keepNewlyCreatedGroup = true) {
      if (this.createMode) {
        this.#keepNewlyCreatedGroup = keepNewlyCreatedGroup;
      }
      this.#panel.hidePopup();
    }

    on_popupshown() {
      if (this.createMode) {
        this.#keepNewlyCreatedGroup = true;
      }
      this.#nameField.focus();
      for (let button of Object.values(this.#commandButtons)) {
        button.tooltipText = button.label;
      }
    }

    on_popuphidden() {
      if (this.createMode) {
        if (this.#keepNewlyCreatedGroup) {
          this.dispatchEvent(
            new CustomEvent("TabGroupCreateDone", { bubbles: true })
          );
        } else {
          this.activeGroup.ungroupTabs();
        }
      }
      this.activeGroup = null;
    }

    on_keypress(event) {
      if (event.defaultPrevented) {
        return;
      }

      switch (event.keyCode) {
        case KeyEvent.DOM_VK_ESCAPE:
          this.close(false);
          break;
        case KeyEvent.DOM_VK_RETURN:
          if (
            event.target.localName != "toolbarbutton" &&
            event.target.localName != "moz-button"
          ) {
            this.close();
          }
          break;
      }
    }

    on_change(event) {
      if (event.target.name == "tab-group-color" && this.activeGroup) {
        this.activeGroup.color = event.target.value;
      }
    }

    async #handleNewTabInGroup() {
      let lastTab = this.activeGroup?.tabs.at(-1);
      let onTabOpened = event => {
        this.activeGroup?.addTabs([event.target]);
        this.close();
        window.removeEventListener("TabOpen", onTabOpened);
      };
      window.addEventListener("TabOpen", onTabOpened);
      window.focus();
      gBrowser.addAdjacentNewTab(lastTab);
    }

    #getGroupLinks(group) {
      let links = [];
      for (let tab of group.tabs) {
        let browser = tab.linkedBrowser;
        let shareableURL = BrowserUtils.getShareableURL(browser.currentURI);
        if (shareableURL) {
          links.push({
            url: gURLBar.makeURIReadable(shareableURL).displaySpec,
            title: browser.contentTitle,
          });
        }
      }
      return links;
    }
  }

  customElements.define("tabgroup-menu", MozTabbrowserTabGroupMenu);
}
