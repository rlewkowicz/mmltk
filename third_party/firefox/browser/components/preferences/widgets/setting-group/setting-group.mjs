/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { html } from "chrome://global/content/vendor/lit.all.mjs";
import {
  SettingElement,
  bumpHeadingLevelForSrd,
  spread,
} from "chrome://browser/content/preferences/widgets/setting-element.mjs";
import { SettingControl } from "chrome://browser/content/preferences/widgets/setting-control.mjs";
import { SettingGroupManager } from "chrome://browser/content/preferences/config/SettingGroupManager.mjs";



const CLICK_HANDLERS = new Set([
  "dialog-button",
  "moz-box-button",
  "moz-box-item",
  "moz-box-link",
  "moz-button",
  "moz-box-group",
  "moz-message-bar",
  "a",
]);
const DISMISS_HANDLERS = new Set(["moz-message-bar"]);
const REORDER_HANDLERS = new Set(["moz-box-group"]);

const HiddenAttr = Object.freeze({
  Self: "data-hidden-by-setting-group",
  Search: "data-hidden-from-search",
});

export class SettingGroup extends SettingElement {
  static properties = {
    config: { type: Object },
    groupId: { type: String },
    getSetting: { type: Function },
    srdEnabled: { type: Boolean },
    inSubPane: { type: Boolean },
  };

  static queries = {
    allControlEls: { all: "setting-control" },
    fieldsetEl: "moz-fieldset",
  };

  get childControlEls() {
    if (!this.config) {
      return [];
    }
    // @ts-expect-error bug 1997478
    return [...this.fieldsetEl.children].filter(
      child => child instanceof SettingControl
    );
  }

  constructor() {
    super();

    this.getSetting = undefined;

    this.config = undefined;

    this.srdEnabled = false;
    this.inSubPane = false;
  }

  createRenderRoot() {
    return this;
  }

  #unsubscribeGroupRegister = null;

  connectedCallback() {
    super.connectedCallback();
    this.#unsubscribeGroupRegister = SettingGroupManager.onRegister(id => {
      if (id === this.groupId && !this.config) {
        window.initSettingGroup(id);
      }
    });
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    this.#unsubscribeGroupRegister?.();
    this.#unsubscribeGroupRegister = null;
  }

  willUpdate() {
    if (!this.srdEnabled) {
      this.classList.toggle("subcategory", this.config?.headingLevel == 1);
    }
    if (this.config?.hiddenFromSearch !== undefined) {
      if (this.config.hiddenFromSearch) {
        this.setAttribute(HiddenAttr.Search, "true");
      } else {
        this.removeAttribute(HiddenAttr.Search);
      }
    }
    if (this.config?.hidden !== undefined) {
      this.toggleAttribute(HiddenAttr.Self, this.config.hidden);
    }
    if (this.config?.subcategory) {
      this.setAttribute("data-subcategory", this.config.subcategory);
    }
  }

  async handleVisibilityChange() {
    await this.updateComplete;

    if (this.config?.hidden) {
      return;
    }

    let hasVisibleControls =
      !!this.childControlEls?.length &&
      this.childControlEls?.some(el => !el.hidden);
    let groupbox =  (this.closest("groupbox"));

    if (hasVisibleControls) {
      if (this.hasAttribute(HiddenAttr.Self)) {
        this.removeAttribute(HiddenAttr.Self);
        this.removeAttribute(HiddenAttr.Search);
      }
      if (groupbox && groupbox.hasAttribute(HiddenAttr.Self)) {
        groupbox.removeAttribute(HiddenAttr.Search);
        groupbox.removeAttribute(HiddenAttr.Self);
      }
    } else {
      this.setAttribute(HiddenAttr.Self, "");
      this.setAttribute(HiddenAttr.Search, "true");
      if (groupbox && !groupbox.hasAttribute(HiddenAttr.Search)) {
        groupbox.setAttribute(HiddenAttr.Search, "true");
        groupbox.setAttribute(HiddenAttr.Self, "");
      }
    }
  }

  async getUpdateComplete() {
    let result = await super.getUpdateComplete();
    // @ts-expect-error bug 1997478
    await Promise.all([...this.allControlEls].map(el => el.updateComplete));
    return result;
  }

  onChange(e) {
    let inputEl = e.target;
    inputEl.control?.onChange(inputEl);
  }

  onClick(e) {
    let inputEl = e.target;
    if (!CLICK_HANDLERS.has(inputEl.localName)) {
      return;
    }
    inputEl.control?.onClick(e);
  }

  onMessageBarDismiss(e) {
    let inputEl = e.target;
    if (!DISMISS_HANDLERS.has(inputEl.localName)) {
      return;
    }
    inputEl.control?.onMessageBarDismiss(e);
  }

  onReorder(e) {
    let inputEl = e.target;
    if (!REORDER_HANDLERS.has(inputEl.localName)) {
      return;
    }
    inputEl.control?.onReorder(e);
  }

  itemTemplate(item) {
    let setting = this.getSetting(item.id);
    return html`<setting-control
      .setting=${setting}
      .config=${item}
      .getSetting=${this.getSetting}
    ></setting-control>`;
  }

  containerTemplate(content) {
    if (
      (this.srdEnabled || this.inSubPane || this.config.card == "always") &&
      this.config.card != "never"
    ) {
      return html`<moz-card role="presentation">${content}</moz-card>`;
    }
    return content;
  }

  render() {
    if (!this.config) {
      return "";
    }
    let headingLevel = this.config.headingLevel;
    if (this.srdEnabled) {
      headingLevel = bumpHeadingLevelForSrd(headingLevel ?? 2, true);
    }
    return this.containerTemplate(
      html`<moz-fieldset
        .headingLevel=${headingLevel}
        @change=${this.onChange}
        @toggle=${this.onChange}
        @click=${this.onClick}
        @message-bar:user-dismissed=${this.onMessageBarDismiss}
        @reorder=${this.onReorder}
        @visibility-change=${this.handleVisibilityChange}
        ${spread(this.getCommonPropertyMapping(this.config))}
        >${this.config.items.map(item => this.itemTemplate(item))}</moz-fieldset
      >`
    );
  }
}
customElements.define("setting-group", SettingGroup);
