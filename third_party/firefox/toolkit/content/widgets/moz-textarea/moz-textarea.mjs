/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { html, ifDefined } from "../vendor/lit.all.mjs";
import MozInputText from "chrome://global/content/elements/moz-input-text.mjs";

export default class MozTextarea extends MozInputText {
  static properties = {
    ...MozInputText.properties,
    rows: { type: Number, reflect: true },
  };

  constructor() {
    super();
    this.rows = 2;
  }

  inputStylesTemplate() {
    return html`
      ${super.inputStylesTemplate()}
      <link
        rel="stylesheet"
        href="chrome://global/content/elements/moz-textarea.css"
      />
    `;
  }

  inputTemplate() {
    return html`
      <textarea
        id="input"
        name=${this.name}
        rows=${this.rows}
        .value=${this.value}
        ?disabled=${this.disabled || this.parentDisabled}
        ?readonly=${this.readonly}
        accesskey=${ifDefined(this.accessKey)}
        placeholder=${ifDefined(this.placeholder)}
        aria-label=${ifDefined(this.ariaLabel ?? undefined)}
        aria-describedby="description"
        aria-description=${ifDefined(
          this.hasDescription ? undefined : this.ariaDescription
        )}
        @input=${this.handleInput}
        @change=${this.redispatchEvent}
      ></textarea>
    `;
  }
}
customElements.define("moz-textarea", MozTextarea);
