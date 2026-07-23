/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import {
  createRef,
  html,
  ifDefined,
  literal,
  ref,
  repeat,
  staticHtml,
  unsafeStatic,
} from "chrome://global/content/vendor/lit.all.mjs";
import {
  SettingElement,
  spread,
} from "chrome://browser/content/preferences/widgets/setting-element.mjs";
import MozInputFolder from "chrome://global/content/elements/moz-input-folder.mjs";







const KNOWN_OPTIONS = new Map([
  ["moz-radio-group", literal`moz-radio`],
  ["moz-select", literal`moz-option`],
  ["moz-visual-picker", literal`moz-visual-picker-item`],
]);

const ITEM_SLOT_BY_PARENT = new Map([
  ["moz-checkbox", "nested"],
  ["moz-input-email", "nested"],
  ["moz-input-folder", "nested"],
  ["moz-input-number", "nested"],
  ["moz-input-password", "nested"],
  ["moz-input-search", "nested"],
  ["moz-input-tel", "nested"],
  ["moz-input-text", "nested"],
  ["moz-input-url", "nested"],
  ["moz-radio", "nested"],
  ["moz-radio-group", "nested"],
  ["moz-toggle", "nested"],
]);

export class SettingNotDefinedError extends Error {
  constructor(settingId) {
    super(
      `No Setting with id "${settingId}". Did you register it with Preferences.addSetting()?`
    );
    this.name = "SettingNotDefinedError";
    this.settingId = settingId;
  }
}

export class SettingControl extends SettingElement {
  static SettingNotDefinedError = SettingNotDefinedError;
  static properties = {
    setting: { type: Object },
    config: { type: Object },
    value: {},
    parentDisabled: { type: Boolean },
    tabIndex: { type: Number, reflect: true },
  };

  #lastSetting;

  constructor() {
    super();
    this.controlRef = createRef();

    this.getSetting = undefined;

    this.setting = undefined;

    this.config = undefined;

    this.parentDisabled = undefined;

  }

  createRenderRoot() {
    return this;
  }

  focus() {
    this.controlEl.focus();
  }

  get controlEl() {
    return this.controlRef.value;
  }

  get disabled() {
    return this.setting?.disabled || this.setting?.locked;
  }

  async getUpdateComplete() {
    let result = await super.getUpdateComplete();
    await this.controlEl?.updateComplete;
    return result;
  }

  onSettingChange = () => {
    this.setValue();
    this.requestUpdate();
  };

  willUpdate(changedProperties) {
    if (changedProperties.has("setting")) {
      if (this.#lastSetting) {
        this.#lastSetting.off("change", this.onSettingChange);
      }
      this.#lastSetting = this.setting;
      this.setValue();
      this.setting.on("change", this.onSettingChange);
    }
    if (!this.setting) {
      throw new SettingNotDefinedError(this.config.id);
    }
    this.id = `setting-control-${this.config.id}`;
    let prevHidden = this.hidden;
    this.hidden = !this.setting.visible;
    if (prevHidden != this.hidden) {
      this.dispatchEvent(new Event("visibility-change", { bubbles: true }));
    }
  }

  updated() {
    const control = this.controlRef?.value;
    if (!control) {
      return;
    }

    if ("checked" in control) {
      control.checked = this.value;
    } else if ("pressed" in control) {
      control.pressed = this.value;
    } else if ("value" in control) {
      control.value = this.value;
    }

    control.requestUpdate?.();
  }

  getCommonPropertyMapping(config) {
    return {
      ...super.getCommonPropertyMapping(config),
      "data-subcategory": config.subcategory,
      ".setting": this.setting,
      ".control": this,
    };
  }

  getOptionPropertyMapping(config) {
    return {
      ...this.getCommonPropertyMapping(config),
      ".value": config.value,
      ".disabled": config.disabled,
      ".hidden": config.hidden,
    };
  }

  getControlPropertyMapping(config) {
    return {
      ...this.getCommonPropertyMapping(config),
      ".parentDisabled": this.parentDisabled,
      "?disabled": this.disabled,
      ".hidden": config.control == "moz-message-bar" && this.hidden,
    };
  }

  getValue() {
    return this.setting.value;
  }

  setValue = () => {
    this.value = this.setting.value;
  };

  controlValue(el) {
    let Cls = el.constructor;
    if (
      "activatedProperty" in Cls &&
      Cls.activatedProperty &&
      el.localName != "moz-radio"
    ) {
      return el[ (Cls.activatedProperty)];
    }
    if (el instanceof MozInputFolder) {
      return el.folder;
    }
    return "value" in el ? el.value : null;
  }

  onChange(el) {
    this.setting.userChange(this.controlValue(el));
  }

  onClick(event) {
    this.setting.userClick(event);
  }

  onMessageBarDismiss(event) {
    this.setting.messageBarDismiss(event);
  }

  onReorder(event) {
    this.setting.userReorder(event);
  }

  itemsTemplate(config) {
    if (!config.items) {
      return [];
    }

    const itemArgs = config.items.map(i => ({
      config: i,
      setting: this.getSetting(i.id),
    }));
    let control = config.control || "moz-checkbox";
    return repeat(
      itemArgs,
      item => item.config.key || item.config.id,
      item =>
        html`<setting-control
          .config=${item.config}
          .setting=${item.setting}
          .getSetting=${this.getSetting}
          slot=${ifDefined(
            item.config.slot || ITEM_SLOT_BY_PARENT.get(control)
          )}
        ></setting-control>`
    );
  }

  optionsTemplate(config) {
    if (!config.options) {
      return [];
    }
    let control = config.control || "moz-checkbox";
    return repeat(
      config.options,
      opt => opt.key,
      opt => {
        let optionTag = opt.control
          ? unsafeStatic(opt.control)
          : KNOWN_OPTIONS.get(control);
        let spreadValues = spread(this.getOptionPropertyMapping(opt));
        let children =
          "items" in opt ? this.itemsTemplate(opt) : this.optionsTemplate(opt);
        if (opt.control == "a" && opt.controlAttrs?.is == "moz-support-link") {
          return html`<a is="moz-support-link" ${spreadValues}>${children}</a>`;
        }
        return staticHtml`<${optionTag} ${spreadValues}>${children}</${optionTag}>`;
      }
    );
  }

  render() {
    this.config = this.setting.getControlConfig(this.config);
    let { config } = this;
    let control = config.control || "moz-checkbox";

    let nestedSettings =
      "items" in config
        ? this.itemsTemplate(config)
        : this.optionsTemplate(config);

    let controlProps = this.getControlPropertyMapping(config);

    let tag = unsafeStatic(control);
    return staticHtml`
    <${tag}
      ${spread(controlProps)}
      ${ref(this.controlRef)}
      tabindex=${ifDefined(this.tabIndex)}
    >${nestedSettings}</${tag}>`;
  }
}
customElements.define("setting-control", SettingControl);
