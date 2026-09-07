/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { html, ifDefined } from "../vendor/lit.all.mjs";
import {
  SelectControlBaseElement,
  SelectControlItemMixin,
} from "../lit-select-control.mjs";
import { MozBaseInputElement } from "../lit-utils.mjs";

export class MozRadioGroup extends SelectControlBaseElement {
  static childElementName = "moz-radio";

  static properties = {
    parentDisabled: { type: Boolean, state: true },
  };

  constructor() {
    super();
    this.orientation = "vertical";
  }
}
customElements.define("moz-radio-group", MozRadioGroup);

export class MozRadio extends SelectControlItemMixin(MozBaseInputElement) {
  static activatedProperty = "checked";

  get isDisabled() {
    return (
      super.isDisabled || this.parentDisabled || this.controller.parentDisabled
    );
  }

  inputTemplate() {
    return html`<input
      type="radio"
      id="input"
      .value=${this.value}
      name=${this.name}
      .checked=${this.checked}
      aria-checked=${this.checked}
      tabindex=${this.itemTabIndex}
      ?disabled=${this.isDisabled}
      accesskey=${ifDefined(this.accessKey)}
      aria-label=${ifDefined(this.ariaLabel ?? undefined)}
      aria-describedby="description"
      aria-description=${ifDefined(
        this.hasDescription ? undefined : this.ariaDescription
      )}
      @click=${this.handleClick}
      @change=${this.handleChange}
    />`;
  }
}
customElements.define("moz-radio", MozRadio);
