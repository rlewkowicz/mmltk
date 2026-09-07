/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import {
  LitElement,
  html,
  ifDefined,
  nothing,
  classMap,
} from "chrome://global/content/vendor/lit.all.mjs";

function query(el, selector) {
  return () => el.renderRoot.querySelector(selector);
}

function queryAll(el, selector) {
  return () => el.renderRoot.querySelectorAll(selector);
}

export class MozLitElement extends LitElement {
  #l10nObj;
  #l10nRootConnected = false;

  static createProperty(attrName, options) {
    if (options.mapped) {
      let domAttrPropertyName = `${attrName}Attribute`;
      let domAttrName = options.attribute ?? attrName.toLowerCase();
      if (attrName.startsWith("aria")) {
        domAttrName = domAttrName.replace("aria", "aria-");
      }
      this.mappedAttributes ??= [];
      this.mappedAttributes.push([attrName, domAttrPropertyName]);
      options.state = true;
      super.createProperty(domAttrPropertyName, {
        type: String,
        attribute: domAttrName,
        reflect: true,
      });
    }
    if (options.fluent) {
      this.fluentProperties ??= [];
      this.fluentProperties.push(options.attribute || attrName.toLowerCase());
    }
    return super.createProperty(attrName, options);
  }

  constructor() {
    super();
    let { queries } = this.constructor;
    if (queries) {
      for (let [selectorName, selector] of Object.entries(queries)) {
        if (selector.all) {
          Object.defineProperty(this, selectorName, {
            get: queryAll(this, selector.all),
          });
        } else {
          Object.defineProperty(this, selectorName, {
            get: query(this, selector),
          });
        }
      }
    }
  }

  connectedCallback() {
    super.connectedCallback();
    if (
      this.renderRoot == this.shadowRoot &&
      !this.#l10nRootConnected &&
      this.#l10n
    ) {
      this.#l10n.connectRoot(this.renderRoot);
      this.#l10nRootConnected = true;

      if (this.constructor.fluentProperties?.length) {
        let { fluentProperties } = this.constructor;
        if (this.dataset.l10nAttrs) {
          fluentProperties = fluentProperties.concat(this.dataset.l10nAttrs);
        }
        this.dataset.l10nAttrs = fluentProperties.join(",");
        if (this.dataset.l10nId) {
          this.#l10n.translateElements([this]);
        }
      }
    }
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    if (
      this.renderRoot == this.shadowRoot &&
      this.#l10nRootConnected &&
      this.#l10n
    ) {
      this.#l10n.disconnectRoot(this.renderRoot);
      this.#l10nRootConnected = false;
    }
  }

  willUpdate(changes) {
    this.#handleMappedAttributeChange(changes);
  }

  #handleMappedAttributeChange(changes) {
    if (!this.constructor.mappedAttributes) {
      return;
    }
    for (let [attrName, domAttrName] of this.constructor.mappedAttributes) {
      if (changes.has(domAttrName)) {
        this[attrName] = this[domAttrName];
        this[domAttrName] = null;
      }
    }
  }

  get #l10n() {
    if (!this.#l10nObj) {
      this.#l10nObj =
        (false && window.mockL10n) || document.l10n;
    }
    return this.#l10nObj;
  }

  async dispatchOnUpdateComplete(event) {
    await this.updateComplete;
    this.dispatchEvent(event);
  }

  update() {
    super.update();
    if (this.#l10n) {
      this.#l10n.translateFragment(this.renderRoot);
    }
  }
}

export class MozBaseInputElement extends MozLitElement {
  static formAssociated = true;
  #internals;
  #hasSlottedContent = new Map();

  static properties = {
    label: { type: String, fluent: true },
    name: { type: String },
    value: { type: String },
    iconSrc: { type: String },
    disabled: { type: Boolean },
    description: { type: String, fluent: true },
    supportPage: { type: String, attribute: "support-page" },
    accessKey: { type: String, mapped: true, fluent: true },
    parentDisabled: { type: Boolean, state: true },
    ariaLabel: { type: String, mapped: true },
    ariaDescription: { type: String, mapped: true },
    inputLayout: { type: String, reflect: true, attribute: "inputlayout" },
  };
  static inputLayout = "inline";
  static activatedProperty = null;

  constructor() {
    super();
    this.disabled = false;
    this.inputLayout =  (
      this.constructor
    ).inputLayout;
    this.#internals = this.attachInternals();
  }

  get form() {
    return this.#internals.form;
  }

  setFormValue(value) {
    this.#internals.setFormValue(value);
  }

  formResetCallback() {
    this.value = this.defaultValue;
  }

  connectedCallback() {
    super.connectedCallback();
    let val = this.getAttribute("value") || this.value;
    this.defaultValue = val;
    this.value = val;
    this.#internals.setFormValue(this.value || null);
  }

  willUpdate(changedProperties) {
    super.willUpdate(changedProperties);
    this.#updateInternalState(this.description, "description");
    this.#updateInternalState(this.supportPage, "support-link");
    this.#updateInternalState(this.label, "label");

    if (changedProperties.has("value")) {
      this.setFormValue(this.value);
    }
    let activatedProperty =  (
      this.constructor
    ).activatedProperty;
    if (
      (activatedProperty && changedProperties.has(activatedProperty)) ||
      changedProperties.has("disabled") ||
      changedProperties.has("parentDisabled")
    ) {
      this.updateNestedElements();
    }
  }

  #updateInternalState(propVal, stateKey) {
    let internalStateKey = `has-${stateKey}`;
    let hasValue = !!(propVal || this.#hasSlottedContent.get(stateKey));

    if (this.#internals.states?.has(internalStateKey) == hasValue) {
      return;
    }

    if (hasValue) {
      this.#internals.states.add(internalStateKey);
    } else {
      this.#internals.states.delete(internalStateKey);
    }
  }

  updateNestedElements() {
    if (this.isDisabled) {
      this.#internals.states.add("disabled");
    } else {
      this.#internals.states.delete("disabled");
    }
    for (let el of this.nestedEls) {
      if ("parentDisabled" in el) {
        el.parentDisabled =
          this.parentDisabled ||
          !this[this.constructor.activatedProperty] ||
          this.disabled;
      }
    }
  }

  get inputEl() {
    return this.renderRoot.getElementById("input");
  }

  get labelEl() {
    return this.renderRoot.querySelector("label");
  }

  get icon() {
    return this.renderRoot.querySelector(".icon");
  }

  get descriptionEl() {
    return this.renderRoot.getElementById("description");
  }

  get nestedEls() {
    return this.renderRoot.querySelector(".nested")?.assignedElements() ?? [];
  }

  get hasDescription() {
    return this.#internals.states.has("has-description");
  }

  get hasSupportLink() {
    return this.#internals.states.has("has-support-link");
  }

  get hasLabel() {
    return this.#internals.states.has("has-label");
  }

  get isDisabled() {
    return !!(this.disabled || this.parentDisabled);
  }

  click() {
    this.inputEl.click();
  }

  focus() {
    this.inputEl.focus();
  }

  select() {
    this.inputEl.select();
  }

  blur() {
    this.inputEl.blur();
  }

  redispatchEvent(event) {
    let { bubbles, cancelable, composed, type } = event;
    let newEvent = new Event(type, {
      bubbles,
      cancelable,
      composed,
    });
    this.dispatchEvent(newEvent);
  }

  inputTemplate() {
    throw new Error(
      "inputTemplate() must be implemented and provide the input element"
    );
  }

  inputStylesTemplate() {
    return nothing;
  }

  render() {
    return html`
      <link
        rel="stylesheet"
        href="chrome://global/content/elements/moz-input-common.css"
      />
      ${this.inputStylesTemplate()}
      <div class="content-wrapper">
        <span class="label-wrapper">
          <label
            is="moz-label"
            id="label"
            part="label"
            for="input"
            shownaccesskey=${ifDefined(this.accessKey)}
            >${this.inputLayout === "inline"
              ? this.inputTemplate()
              : ""}${this.labelTemplate()}</label
          >${this.hasDescription ? "" : this.supportLinkTemplate()}
          ${this.descriptionTemplate()}
        </span>
        ${this.inputLayout !== "inline" ? this.inputTemplate() : ""}
      </div>
      ${this.nestedFieldsTemplate()}
    `;
  }

  labelTemplate() {
    if (!this.label) {
      return "";
    }
    let labelEl;
    let headingLevel = this.getAttribute("headinglevel");
    if (headingLevel == "3" || headingLevel == "4") {
      labelEl = html`<h3
        class="text text-box-trim-start"
        .textContent=${this.label}
      ></h3>`;
    } else {
      labelEl = html`<span class="text" .textContent=${this.label}></span>`;
    }
    return html`<span class="text-container"
      >${this.iconTemplate()}${labelEl}</span
    >`;
  }

  descriptionTemplate() {
    return html`
      <div class="description text-deemphasized">
        <span id="description" class="description-text">
          ${this.description ??
          html`<slot
            name="description"
            @slotchange=${this.onSlotchange}
          ></slot>`}</span
        >${this.hasDescription ? this.supportLinkTemplate() : ""}
      </div>
    `;
  }

  iconTemplate() {
    if (this.iconSrc) {
      return html`<img src=${this.iconSrc} role="presentation" class="icon" />`;
    }
    return "";
  }

  supportLinkTemplate() {
    if (this.supportPage) {
      return html`<a
        is="moz-support-link"
        support-page=${this.supportPage}
        part="support-link"
        aria-describedby="label description"
      ></a>`;
    }
    return html`<slot
      name="support-link"
      @slotchange=${this.onSlotchange}
    ></slot>`;
  }

  nestedFieldsTemplate() {
    if (this.constructor.activatedProperty) {
      return html`<slot
        name="nested"
        class="nested"
        @slotchange=${this.updateNestedElements}
      ></slot>`;
    }
    return "";
  }

  onSlotchange(e) {
    let propName = e.target.name;
    let hasSlottedContent = e.target
      .assignedNodes()
      .some(
        node => node.textContent.trim() || node.getAttribute("data-l10n-id")
      );

    if (hasSlottedContent == this.#hasSlottedContent.get(propName)) {
      return;
    }

    this.#hasSlottedContent.set(propName, hasSlottedContent);
    this.requestUpdate();
  }
}

export class MozBoxBase extends MozLitElement {
  static properties = {
    label: { type: String, fluent: true },
    description: { type: String, fluent: true },
    iconSrc: { type: String },
  };

  constructor() {
    super();
    this.label = "";
    this.description = "";
    this.iconSrc = "";
  }

  get labelEl() {
    return this.renderRoot.querySelector(".label");
  }

  get descriptionEl() {
    return this.renderRoot.querySelector(".description");
  }

  get iconEl() {
    return this.renderRoot.querySelector(".icon");
  }

  stylesTemplate() {
    return html`
      <link
        rel="stylesheet"
        href="chrome://global/content/elements/moz-box-common.css"
      />
      <link
        rel="stylesheet"
        href="chrome://global/skin/design-system/text-and-typography.css"
      />
    `;
  }

  textTemplate() {
    return html`<div
      class=${classMap({
        "text-content": true,
        "has-icon": this.iconSrc,
        "has-description": this.description,
      })}
    >
      ${this.iconTemplate()}${this.labelTemplate()}${this.descriptionTemplate()}
    </div>`;
  }

  labelTemplate() {
    if (!this.label) {
      return "";
    }
    return html`<span class="label" id="label">${this.label}</span>`;
  }

  iconTemplate() {
    if (!this.iconSrc) {
      return "";
    }
    return html`<img src=${this.iconSrc} role="presentation" class="icon" />`;
  }

  descriptionTemplate() {
    if (!this.description) {
      return "";
    }
    return html`<span class="description text-deemphasized" id="description">
      ${this.description}
    </span>`;
  }
}
