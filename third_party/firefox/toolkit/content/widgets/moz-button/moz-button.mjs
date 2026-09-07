/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { html, ifDefined, classMap } from "../vendor/lit.all.mjs";
import { MozLitElement } from "../lit-utils.mjs";

window.MozXULElement?.insertFTLIfNeeded("toolkit/global/mozButton.ftl");

// eslint-disable-next-line import/no-unassigned-import
import "chrome://global/content/elements/moz-label.mjs";

class MenuController {
  host;

  #menuId;

  #menuEl;

  #hostIsSplitButton;

  constructor(host) {
    this.host = host;
    host.addController(this);
  }

  hostConnected() {
    this.hostUpdated();
  }

  hostDisconnected() {
    this.removePanelListListeners();
    this.#menuId = null;
    this.#menuEl = null;
  }

  hostUpdated() {
    let hostMenuId = this.host.menuId;
    let hostIsSplitButton = this.host.isSplitButton;

    if (
      this.#menuId === hostMenuId &&
      this.#hostIsSplitButton === hostIsSplitButton
    ) {
      return;
    }
    if (this.#menuEl?.localName == "panel-list") {
      this.panelListCleanUp();
    }

    this.#menuId = hostMenuId;
    this.#hostIsSplitButton = hostIsSplitButton;

    if (this.#menuId) {
      this.#menuEl = this.getPanelList();

      if (this.#menuEl?.localName == "panel-list") {
        this.panelListSetUp();
      }
    }

    if (!this.#menuId) {
      this.#menuEl = null;
      this.host.removeController(this);
    }
  }

  getPanelList() {
    let root = this.host.getRootNode();
    let menuEl = null;

    while (root) {
      menuEl = root.querySelector?.(`#${this.#menuId}`);
      if (menuEl) {
        break;
      }

      if (root instanceof ShadowRoot) {
        root = root.host?.getRootNode();
      } else {
        break;
      }
    }

    return menuEl;
  }

  openPanelList = event => {
    if (event.type == "click") {
      event.preventDefault();
    }
    if (
      (event.type == "mousedown" && event.button == 0) ||
      event.inputSource == MouseEvent.MOZ_SOURCE_KEYBOARD ||
      !event.detail
    ) {
      if (this.#hostIsSplitButton) {
        this.#menuEl?.toggle(event, this.host.chevronButtonEl);
      } else {
        this.#menuEl?.toggle(event, this.host);
      }
    }
  };

  #updateOpenAttr = event => {
    if (event.type == "shown") {
      this.host.toggleAttribute("open", true);
    } else if (event.type == "hidden") {
      this.host.removeAttribute("open");
    }
  };

  removePanelListListeners() {
    if (this.#hostIsSplitButton) {
      this.host.chevronButtonEl?.removeEventListener(
        "click",
        this.openPanelList
      );
      this.host.chevronButtonEl?.removeEventListener(
        "mousedown",
        this.openPanelList
      );
    } else {
      this.host.removeEventListener("click", this.openPanelList);
      this.host.removeEventListener("mousedown", this.openPanelList);
    }
    this.#menuEl?.removeEventListener("shown", this.#updateOpenAttr);
    this.#menuEl?.removeEventListener("hidden", this.#updateOpenAttr);
  }

  panelListSetUp() {
    if (this.#hostIsSplitButton) {
      this.host.chevronButtonEl?.addEventListener("click", this.openPanelList);
      this.host.chevronButtonEl?.addEventListener(
        "mousedown",
        this.openPanelList
      );
    } else {
      this.host.addEventListener("click", this.openPanelList);
      this.host.addEventListener("mousedown", this.openPanelList);
    }
    this.#menuEl.addEventListener("shown", this.#updateOpenAttr);
    this.#menuEl.addEventListener("hidden", this.#updateOpenAttr);
    this.host.ariaHasPopup = "menu";
    this.host.ariaExpanded = this.#menuEl.open ? "true" : "false";
    this.host.toggleAttribute("open", this.#menuEl.open);
    let triggerEl = this.#hostIsSplitButton
      ? this.host.chevronButtonEl
      : this.host.buttonEl;
    if (triggerEl) {
      triggerEl.popoverTargetElement = this.#menuEl;
    }
  }

  panelListCleanUp() {
    this.removePanelListListeners();
    this.host.ariaHasPopup = null;
    this.host.ariaExpanded = null;
    this.host.removeAttribute("open");
    let triggerEl = this.#hostIsSplitButton
      ? this.host.chevronButtonEl
      : this.host.buttonEl;
    if (triggerEl) {
      triggerEl.popoverTargetElement = null;
    }
  }
}

export default class MozButton extends MozLitElement {
  static shadowRootOptions = {
    ...MozLitElement.shadowRootOptions,
    delegatesFocus: true,
  };

  static properties = {
    label: { type: String, reflect: true, fluent: true },
    type: { type: String, reflect: true },
    size: { type: String, reflect: true },
    disabled: { type: Boolean, reflect: true },
    title: { type: String, mapped: true },
    tooltipText: { type: String, fluent: true },
    ariaLabel: { type: String, mapped: true },
    ariaHasPopup: { type: String, mapped: true },
    ariaExpanded: { type: String, mapped: true },
    ariaPressed: { type: String, mapped: true },
    iconSrc: { type: String },
    hasVisibleLabel: { type: Boolean, state: true },
    accessKey: { type: String, mapped: true },
    attention: { type: Boolean },
    iconPosition: { type: String, reflect: true },
    menuId: { type: String, reflect: true },
    parentDisabled: { type: Boolean },
  };

  static queries = {
    buttonEl: "#main-button",
    chevronButtonEl: "#chevron-button",
    slotEl: "slot",
    backgroundEl: "#main-button .button-background",
  };

  constructor() {
    super();
    this.type = "default";
    this.size = "default";
    this.disabled = false;
    this.hasVisibleLabel = !!this.label;
    this.attention = false;
    this.iconPosition = "start";
    this.menuId = "";
    this.parentDisabled = undefined;
  }

  updated(changedProperties) {
    super.updated(changedProperties);

    if (changedProperties.has("menuId")) {
      if (this.menuId && !this._menuController) {
        this._menuController = new MenuController(this);
      }
      if (!this.menuId && this._menuController) {
        this._menuController = null;
      }
    }
  }

  get isSplitButton() {
    return this.type === "split";
  }

  click() {
    this.performUpdate();
    this.buttonEl?.click();
  }

  checkForLabelText() {
    this.hasVisibleLabel = this.slotEl
      ?.assignedNodes()
      .some(node => node.textContent.trim());
  }

  labelTemplate() {
    if (this.label) {
      return html`<span class="text" .textContent=${this.label}></span>`;
    }
    return html`<slot @slotchange=${this.checkForLabelText}></slot>`;
  }

  iconTemplate(position) {
    if (this.iconSrc && position == this.iconPosition) {
      return html`<img src=${this.iconSrc} role="presentation" />`;
    }
    return null;
  }

  chevronButtonTemplate() {
    if (this.isSplitButton) {
      return html`<button
        id="chevron-button"
        size=${this.size}
        ?disabled=${this.disabled || this.parentDisabled}
        data-l10n-id="moz-button-more-options"
        aria-labelledby="main-button chevron-button"
        aria-expanded=${ifDefined(this.ariaExpanded)}
        aria-haspopup=${ifDefined(this.ariaHasPopup)}
        @click=${e => e.stopPropagation()}
        @mousedown=${e => e.stopPropagation()}
      >
        <span
          class="button-background"
          part="chevron-button"
          type=${this.type}
          size=${this.size}
        >
          <img
            src="chrome://global/skin/icons/arrow-down.svg"
            role="presentation"
          />
        </span>
      </button>`;
    }
    return null;
  }

  render() {
    return html`
      <link
        rel="stylesheet"
        href="chrome://global/content/elements/moz-button.css"
      />
      <button
        id="main-button"
        ?disabled=${this.disabled || this.parentDisabled}
        title=${ifDefined(this.title || this.tooltipText)}
        aria-label=${ifDefined(this.ariaLabel)}
        aria-expanded=${ifDefined(
          this.isSplitButton ? undefined : this.ariaExpanded
        )}
        aria-haspopup=${ifDefined(
          this.isSplitButton ? undefined : this.ariaHasPopup
        )}
        aria-pressed=${ifDefined(this.ariaPressed)}
        accesskey=${ifDefined(this.accessKey)}
      >
        <span
          class=${classMap({
            labelled: this.label || this.hasVisibleLabel,
            "button-background": true,
            badged:
              (this.iconSrc || this.type.includes("icon")) && this.attention,
          })}
          part="button"
          type=${this.type}
          size=${this.size}
        >
          ${this.iconTemplate("start")}
          <label
            is="moz-label"
            shownaccesskey=${ifDefined(this.accessKey)}
            part="moz-button-label"
          >
            ${this.labelTemplate()}
          </label>
          ${this.iconTemplate("end")}
        </span>
      </button>
      ${this.chevronButtonTemplate()}
    `;
  }
}
customElements.define("moz-button", MozButton);
