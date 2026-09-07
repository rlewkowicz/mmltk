/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { html, ifDefined } from "../vendor/lit.all.mjs";
import { MozLitElement } from "../lit-utils.mjs";

export default class MozInputColor extends MozLitElement {
  static properties = {
    value: { type: String },
    name: { type: String },
    label: { type: String, fluent: true },
  };

  static queries = {
    inputEl: ".swatch",
  };

  static shadowRootOptions = {
    ...MozLitElement.shadowRootOptions,
    delegatesFocus: true,
  };

  constructor() {
    super();

    this.name = "";
    this.label = "";
    this.value = "";
  }

  updateInputFromEvent(e) {
    const input =  (e.target);
    this.value = input.value;
  }

  redispatchEvent(e) {
    this.updateInputFromEvent(e);

    let { bubbles, cancelable, composed, type } = e;
    let newEvent = new Event(type, {
      bubbles,
      cancelable,
      composed,
    });
    this.dispatchEvent(newEvent);
  }

  render() {
    return html`
      <link
        rel="stylesheet"
        href="chrome://global/content/elements/moz-input-color.css"
      />

      <label title=${this.value}>
        <input
          type="color"
          name=${ifDefined(this.name)}
          .value=${this.value}
          class="swatch"
          @input=${this.updateInputFromEvent}
          @change=${this.redispatchEvent}
        />
        <span>${this.label}</span>
        <img
          class="icon"
          alt=""
          src="chrome://global/skin/icons/edit-outline.svg"
        />
      </label>
    `;
  }
}
customElements.define("moz-input-color", MozInputColor);
