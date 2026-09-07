/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { Preferences } from "chrome://global/content/preferences/Preferences.mjs";

const { EventEmitter } = ChromeUtils.importESModule(
  "resource://gre/modules/EventEmitter.sys.mjs"
);



function getElementsByAttribute(name, value) {
  return document.querySelectorAll(`[${name}="${value}"]`);
}

export class Preference extends EventEmitter {
  _value;

  constructor({ id, type, inverted }) {
    super();
    this.on("change", this.onChange.bind(this));

    this._value = null;
    this.readonly = false;
    this._useDefault = false;
    this.batching = false;

    this.id = id;
    this.type = type;
    this.inverted = !!inverted;


    if (
      Preferences.type == "child" &&
      window.opener &&
      window.opener.Preferences &&
      window.opener.document.nodePrincipal.isSystemPrincipal
    ) {
      const preference = window.opener.Preferences.get(this.id);

      this._value = preference ? preference.value : this.valueFromPreferences;
    } else {
      this._value = this.valueFromPreferences;
    }
  }

  reset() {
    this.value = undefined;
  }

  _reportUnknownType() {
    const msg = `Preference with id=${this.id} has unknown type ${this.type}.`;
    Services.console.logStringMessage(msg);
  }

  setElementValue(aElement) {
    if (this.locked) {
      aElement.disabled = true;
    }
    if (aElement.labels?.length) {
      for (let label of aElement.labels) {
         (label).toggleAttribute("disabled", this.locked);
      }
    }

    if (!this.isElementEditable(aElement)) {
      return;
    }

    let rv = undefined;

    if (Preferences._syncFromPrefListeners.has(aElement)) {
      rv = Preferences._syncFromPrefListeners.get(aElement)(aElement);
    }
    let val = rv;
    if (val === undefined) {
      val = Preferences.instantApply ? this.valueFromPreferences : this.value;
    }
    if (val === undefined) {
      val = this.defaultValue;
    }

    function setValue(element, attribute, value) {
      if (attribute in element) {
        // @ts-expect-error The property might not be writable...
        element[attribute] = value;
      } else if (attribute === "checked" || attribute === "pressed") {
        if (value) {
          element.setAttribute(attribute, "true");
        } else {
          element.removeAttribute(attribute);
        }
      } else {
        element.setAttribute(attribute, value);
      }
    }
    if (
      aElement.localName == "checkbox" ||
      aElement.localName == "moz-checkbox" ||
      (aElement.localName == "input" && aElement.type == "checkbox")
    ) {
      setValue(aElement, "checked", val);
    } else if (aElement.localName == "moz-toggle") {
      setValue(aElement, "pressed", val);
    } else {
      setValue(aElement, "value", val);
    }
  }

  getElementValue(aElement) {
    if (Preferences._syncToPrefListeners.has(aElement)) {
      try {
        const rv = Preferences._syncToPrefListeners.get(aElement)(aElement);
        if (rv !== undefined) {
          return rv;
        }
      } catch (e) {
        console.error(e);
      }
    }

    function getValue(element, attribute) {
      if (attribute in element) {
        return element[ (attribute)];
      }
      return element.getAttribute(attribute);
    }
    let value;
    if (
      aElement.localName == "checkbox" ||
      aElement.localName == "moz-checkbox" ||
      (aElement.localName == "input" &&
         (aElement).type == "checkbox")
    ) {
      value = getValue(aElement, "checked");
    } else if (aElement.localName == "moz-toggle") {
      value = getValue(aElement, "pressed");
    } else {
      value = getValue(aElement, "value");
    }

    switch (this.type) {
      case "int":
        return parseInt(value, 10) || 0;
      case "bool":
        return typeof value == "boolean" ? value : value == "true";
    }
    return value;
  }

  isElementEditable(aElement) {
    switch (aElement.localName) {
      case "checkbox":
      case "input":
      case "radiogroup":
      case "textarea":
      case "menulist":
      case "moz-toggle":
      case "moz-checkbox":
        return true;
    }
    return false;
  }

  updateElements() {
    if (!this.id) {
      return;
    }

    const elements = getElementsByAttribute("preference", this.id);
    for (const element of elements) {
      this.setElementValue(element);
    }
  }

  onChange() {
    this.updateElements();
  }

  get value() {
    return this._value;
  }

  set value(val) {
    if (this.value !== val) {
      this._value = val;
      if (Preferences.instantApply) {
        this.valueFromPreferences = val;
      }
      this.emit("change");
    }
  }

  get locked() {
    return Services.prefs.prefIsLocked(this.id);
  }

  updateControlDisabledState(val) {
    if (!this.id) {
      return;
    }

    val = val || this.locked;

    const elements = getElementsByAttribute("preference", this.id);
    for (const element of elements) {
      element.disabled = val;

      const labels = getElementsByAttribute("control", element.id);
      for (const label of labels) {
        label.disabled = val;
      }
    }
  }

  get hasUserValue() {
    return Services.prefs.prefHasUserValue(this.id) && this.value !== undefined;
  }

  get defaultValue() {
    this._useDefault = true;
    const val = this.valueFromPreferences;
    this._useDefault = false;
    return val;
  }

  get _branch() {
    return this._useDefault ? Preferences.defaultBranch : Services.prefs;
  }

  get valueFromPreferences() {
    try {
      switch (this.type) {
        case "int":
          return this._branch.getIntPref(this.id);
        case "bool": {
          const val = this._branch.getBoolPref(this.id);
          return this.inverted ? !val : val;
        }
        case "wstring":
          return this._branch.getComplexValue(
            this.id,
            Ci.nsIPrefLocalizedString
          ).data;
        case "string":
        case "unichar":
          return this._branch.getStringPref(this.id);
        case "fontname": {
          const family = this._branch.getStringPref(this.id);
          if (!family) {
            return family;
          }
          const fontEnumerator = Cc[
            "@mozilla.org/gfx/fontenumerator;1"
          ].createInstance(Ci.nsIFontEnumerator);
          return fontEnumerator.getStandardFamilyName(family);
        }
        case "file": {
          const f = this._branch.getComplexValue(this.id, Ci.nsIFile);
          return f;
        }
        default:
          this._reportUnknownType();
      }
    } catch (e) {}
    return null;
  }

  set valueFromPreferences(val) {
    if (this.readonly || this.valueFromPreferences == val) {
      return;
    }

    if (val === undefined) {
      Services.prefs.clearUserPref(this.id);
      return;
    }

    switch (this.type) {
      case "int":
        Services.prefs.setIntPref(this.id,  (val));
        break;
      case "bool":
        Services.prefs.setBoolPref(this.id, this.inverted ? !val : !!val);
        break;
      case "wstring": {
        const pls = Cc["@mozilla.org/pref-localizedstring;1"].createInstance(
          Ci.nsIPrefLocalizedString
        );
        pls.data =  (val);
        Services.prefs.setComplexValue(this.id, Ci.nsIPrefLocalizedString, pls);
        break;
      }
      case "string":
      case "unichar":
      case "fontname":
        Services.prefs.setStringPref(this.id,  (val));
        break;
      case "file": {
        let lf;
        if (typeof val == "string") {
          lf = Cc["@mozilla.org/file/local;1"].createInstance(Ci.nsIFile);
          lf.persistentDescriptor = val;
          if (!lf.exists()) {
            lf.initWithPath(val);
          }
        } else {
          lf =  (val).QueryInterface(Ci.nsIFile);
        }
        Services.prefs.setComplexValue(this.id, Ci.nsIFile, lf);
        break;
      }
      default:
        this._reportUnknownType();
    }
    if (!this.batching) {
      Services.prefs.savePrefFile(null);
    }
  }
}
