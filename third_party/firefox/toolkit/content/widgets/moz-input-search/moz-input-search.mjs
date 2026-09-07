/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { html, ifDefined } from "../vendor/lit.all.mjs";
import MozInputText from "chrome://global/content/elements/moz-input-text.mjs";

export default class MozInputSearch extends MozInputText {
  static #searchDebounceDelayMs = 500;

  #searchTimer = null;

  #clearSearchTimer() {
    if (this.#searchTimer) {
      clearTimeout(this.#searchTimer);
    }
    this.#searchTimer = null;
  }

  #dispatchSearch() {
    this.dispatchEvent(
      new CustomEvent("MozInputSearch:search", {
        bubbles: true,
        composed: true,
        detail: { query: this.value },
      })
    );
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    this.#clearSearchTimer();
  }

  inputStylesTemplate() {
    return html`${super.inputStylesTemplate()}`;
  }

  handleInput(e) {
    super.handleInput(e);
    this.#clearSearchTimer();
    this.#searchTimer = setTimeout(() => {
      this.#dispatchSearch();
    }, MozInputSearch.#searchDebounceDelayMs);
  }

  clear() {
    this.#clearSearchTimer();
    if (this.value) {
      this.value = this.inputEl.value = "";
      this.#dispatchSearch();
    }
  }

  #hasIcon() {
    return this.iconSrc === undefined || !!this.iconSrc;
  }

  inputTemplate() {
    return html`
      <input
        id="input"
        class=${this.#hasIcon() ? "with-icon" : ""}
        type="search"
        name=${this.name}
        .value=${this.value}
        ?disabled=${this.disabled || this.parentDisabled}
        accesskey=${ifDefined(this.accessKey)}
        placeholder=${ifDefined(this.placeholder)}
        aria-label=${ifDefined(this.ariaLabel ?? undefined)}
        aria-describedby="description"
        aria-description=${ifDefined(
          this.hasDescription ? undefined : this.ariaDescription
        )}
        @input=${this.handleInput}
        @change=${this.redispatchEvent}
      />
    `;
  }
}
customElements.define("moz-input-search", MozInputSearch);
