/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import {
  html,
  nothing,
  classMap,
} from "chrome://global/content/vendor/lit.all.mjs";
import {
  SelectControlItemMixin,
  SelectControlBaseElement,
} from "../lit-select-control.mjs";
import { MozLitElement } from "../lit-utils.mjs";
import { ifDefined } from "../vendor/lit.all.mjs";

export class MozVisualPicker extends SelectControlBaseElement {
  static childElementName = "moz-visual-picker-item";
}
customElements.define("moz-visual-picker", MozVisualPicker);

export class MozVisualPickerItem extends SelectControlItemMixin(MozLitElement) {
  static properties = {
    label: { type: String, fluent: true },
    description: { type: String, fluent: true },
    ariaLabel: { type: String, fluent: true, mapped: true },
    imageSrc: { type: String },
  };

  static queries = {
    itemEl: ".picker-item",
    labelEl: ".label",
    descriptionEl: ".description",
  };

  click() {
    this.itemEl.click();
  }

  focus() {
    this.itemEl.focus();
  }

  blur() {
    this.itemEl.blur();
  }

  handleKeydown(event) {
    if (event.code == "Space" || event.code == "Enter") {
      this.handleClick(event);
    }
  }

  handleClick(event) {
    event.stopPropagation();
    this.dispatchEvent(
      new Event("click", {
        bubbles: true,
        composed: true,
      })
    );

    super.handleClick();

    this.dispatchEvent(
      new Event("input", {
        bubbles: true,
        composed: true,
      })
    );
    this.dispatchEvent(
      new Event("change", {
        bubbles: true,
        composed: true,
      })
    );
  }

  handleSlotchange(event) {
    if (!this.label && !this.ariaLabel) {
      let elements = event.target.assignedElements();
      this.itemEl.ariaLabelledByElements = elements;
    }
  }

  contentTemplate() {
    if (!this.imageSrc && !this.label && !this.description) {
      return html`<slot></slot>`;
    }

    return html`
      ${this.imageSrc
        ? html`<img src=${this.imageSrc} role="presentation" part="image" />`
        : nothing}
      <div class="text-content">
        ${this.label ? html`<p class="label">${this.label}</p>` : nothing}
        ${this.description
          ? html`<p class="description">${this.description}</p>`
          : nothing}
      </div>
    `;
  }

  render() {
    return html`
      <link
        rel="stylesheet"
        href="chrome://global/content/elements/moz-visual-picker-item.css"
      />
      <div
        class=${classMap({
          "picker-item": true,
          "image-item": this.imageSrc && this.label,
        })}
        role=${this.role}
        value=${this.value}
        aria-label=${ifDefined(this.ariaLabel)}
        aria-checked=${this.role == "radio" ? this.checked : nothing}
        aria-selected=${this.role == "option" ? this.checked : nothing}
        tabindex=${this.itemTabIndex}
        ?checked=${this.checked}
        ?disabled=${this.isDisabled}
        @click=${this.handleClick}
        @keydown=${this.handleKeydown}
        @slotchange=${this.handleSlotchange}
      >
        ${this.contentTemplate()}
      </div>
    `;
  }
}
customElements.define("moz-visual-picker-item", MozVisualPickerItem);
