/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import {
  createRef,
  html,
  ref,
  classMap,
  ifDefined,
} from "../vendor/lit.all.mjs";
import { MozBaseInputElement, MozLitElement } from "../lit-utils.mjs";



export default class MozSelect extends MozBaseInputElement {
  static properties = {
    size: { type: String, reflect: true },
    options: { type: Array, state: true },
    selectedOption: { type: Object, state: true },
    selectedIndex: { type: Number, state: true },
    usePanelList: { type: Boolean, state: true },
  };
  static inputLayout = "block";

  static queries = {
    panelList: "panel-list",
    panelTrigger: ".panel-trigger",
  };

  constructor() {
    super();
    this.size = "default";
    this.value = "";
    this.options = [];
    this.usePanelList = false;
    this.selectedOption = null;
    this.selectedIndex = 0;
    this.slotRef = createRef();
    this.optionsMutationObserver = new MutationObserver(
      this.populateOptions.bind(this)
    );
  }

  firstUpdated(changedProperties) {
    super.firstUpdated(changedProperties);
    this.optionsMutationObserver.observe(this, {
      attributeFilter: ["label", "value", "iconsrc", "disabled", "hidden"],
      childList: true,
      subtree: true,
    });
  }

  update(changedProperties) {
    super.update(changedProperties);
    if (this.hasUpdated && changedProperties.has("options")) {
      this.value = this.inputEl.value;
    }
  }

  willUpdate(changedProperties) {
    super.willUpdate(changedProperties);
    if (changedProperties.has("value") || changedProperties.has("options")) {
      this.selectedIndex = this.options.findIndex(
        opt => opt.value === this.value
      );
      this.selectedOption = this.options[this.selectedIndex] ?? this.options[0];
    }
  }

  updated() {
    if (
      this.panelTrigger &&
      this.panelList &&
      this.panelTrigger.popoverTargetElement !== this.panelList
    ) {
      this.panelTrigger.popoverTargetElement = this.panelList;
    }
  }

  get _selectedOptionIconSrc() {
    return this.selectedOption?.iconSrc ?? "";
  }

  populateOptions() {
    if (!this.slotRef.value) {
      this.options = [];
      this.usePanelList = false;
      return;
    }

    let options = [];

    for (const node of this.slotRef.value.assignedNodes()) {
      if (node.localName === "moz-option") {
        options.push({
          value: node.getAttribute("value"),
          label: node.getAttribute("label"),
          iconSrc: node.getAttribute("iconsrc"),
          disabled: node.getAttribute("disabled") !== null,
          hidden: node.getAttribute("hidden") !== null,
        });
      } else if (node.localName === "hr") {
        options.push({
          separator: true,
        });
      }
    }

    this.options = options;
    this.usePanelList = options.some(opt => opt.iconSrc);

    if (this.usePanelList && !this.value && this.options.length) {
      this.value = this.options[0].value;
    }
  }

  handleStateChange(event) {
    this.value = event.target.value;
  }

  handlePanelChange(event) {
    this.handleStateChange(event);
    this.redispatchEvent(new Event("change", { bubbles: true }));
  }

  handlePanelHidden() {
    let active = document.activeElement;
    if (!active || active === document.body || active === this) {
      this.panelTrigger?.focus();
    }
  }

  handlePanelMousedown(event) {
    if (event.button !== 0) {
      return;
    }
    if (navigator.platform.includes("Mac")) {
      this.panelTrigger?.focus();
    }
    this.panelList?.toggle(event, this.panelTrigger);
  }

  handlePanelClick(event) {
    event.preventDefault();
    if (event.detail === 0) {
      this.panelList?.toggle(event);
    }
  }

  togglePanel(event) {
    this.panelList?.toggle(event);
  }

  handlePanelKeydown(event) {
    if (this.panelList?.open) {
      return;
    }

    switch (event.key) {
      case "ArrowDown":
      case "ArrowUp":
        event.preventDefault();
        if (navigator.platform.includes("Mac")) {
          this.togglePanel(event);
        } else {
          this.selectNextOption(event.key === "ArrowDown" ? 1 : -1);
        }
        break;
      case "Enter":
        event.preventDefault();
        this.togglePanel(event);
        break;
    }
  }

  selectNextOption(direction) {
    let currentIndex = this.selectedIndex;
    let options = this.options;

    for (let i = 1; i < options.length; i++) {
      let nextIndex = currentIndex + direction * i;
      let nextOption = options[nextIndex];
      if (
        nextOption &&
        !nextOption.disabled &&
        !nextOption.hidden &&
        !nextOption.separator
      ) {
        this.value = nextOption.value;
        this.redispatchEvent(new Event("change", { bubbles: true }));
        return;
      }
    }
  }

  inputStylesTemplate() {
    return html` <link
      rel="stylesheet"
      href="chrome://global/content/elements/moz-select.css"
    />`;
  }

  selectedOptionIconTemplate() {
    if (this._selectedOptionIconSrc) {
      return html`<img
        src=${this._selectedOptionIconSrc}
        role="presentation"
        class="select-option-icon"
      />`;
    }
    return null;
  }

  selectTemplate() {
    return html`<select
      id="input"
      name=${this.name}
      .value=${this.value}
      accesskey=${this.accessKey}
      @input=${this.handleStateChange}
      @change=${this.redispatchEvent}
      ?disabled=${this.disabled || this.parentDisabled}
      size=${this.size}
      aria-label=${ifDefined(this.ariaLabel ?? undefined)}
      aria-describedby="description"
      aria-description=${ifDefined(
        this.hasDescription ? undefined : this.ariaDescription
      )}
    >
      ${this.options.map(option =>
        option.separator
          ? html`<hr />`
          : html`
              <option
                value=${option.value}
                .selected=${option.value == this.value}
                ?disabled=${option.disabled}
                ?hidden=${option.hidden}
              >
                ${option.label}
              </option>
            `
      )}
    </select>`;
  }

  panelTargetTemplate() {
    return html`<button
      id="input"
      name=${this.name}
      .value=${this.value}
      class="panel-trigger"
      type="button"
      role="combobox"
      aria-label=${ifDefined(this.ariaLabel)}
      aria-description=${ifDefined(
        this.hasDescription ? undefined : this.ariaDescription
      )}
      aria-describedby="description"
      aria-haspopup="menu"
      aria-expanded=${this.panelList?.open ? "true" : "false"}
      accesskey=${ifDefined(this.accessKey)}
      @mousedown=${this.handlePanelMousedown}
      @click=${this.handlePanelClick}
      @keydown=${this.handlePanelKeydown}
      ?disabled=${this.disabled || this.parentDisabled}
      size=${this.size}
    >
      <span class="panel-trigger-text">${this.selectedOption?.label}</span>
    </button>`;
  }

  panelListTemplate() {
    return html`<panel-list
      .value=${this.value}
      min-width-from-anchor
      @click=${this.handlePanelChange}
      @hidden=${this.handlePanelHidden}
    >
      ${this.options.map(option =>
        option.separator
          ? html`<hr />`
          : html`<panel-item
              .value=${option.value}
              ?selected=${option.value == this.value}
              ?disabled=${option.disabled}
              ?hidden=${option.hidden}
              icon=${ifDefined(option.iconSrc)}
              style=${option.iconSrc
                ? `--select-item-icon-url: url(${option.iconSrc})`
                : ""}
            >
              ${option.label}
            </panel-item>`
      )}
    </panel-list>`;
  }

  inputTemplate() {
    return html`
      <div
        class=${classMap({
          "select-wrapper": true,
          "with-icon": !!this._selectedOptionIconSrc,
        })}
      >
        ${this.selectedOptionIconTemplate()}
        ${!this.usePanelList
          ? this.selectTemplate()
          : this.panelTargetTemplate()}
        <img
          src="chrome://global/skin/icons/arrow-down.svg"
          role="presentation"
          class="select-chevron-icon"
        />
      </div>
      ${this.usePanelList ? this.panelListTemplate() : ""}
      <slot
        @slotchange=${this.populateOptions}
        hidden
        ${ref(this.slotRef)}
      ></slot>
    `;
  }
}
customElements.define("moz-select", MozSelect);

export class MozOption extends MozLitElement {
  static properties = {
    value: { type: String, reflect: true },
    label: { type: String, reflect: true, fluent: true },
    iconSrc: { type: String, reflect: true },
    disabled: { type: Boolean, reflect: true },
    hidden: { type: Boolean, reflect: true },
  };

  constructor() {
    super();
    this.value = "";
    this.label = "";
    this.iconSrc = "";
    this.disabled = false;
    this.hidden = false;
  }

  render() {
    return "";
  }
}
customElements.define("moz-option", MozOption);
