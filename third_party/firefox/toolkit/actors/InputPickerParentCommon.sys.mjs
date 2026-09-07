/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const DEBUG = false;
function debug(aStr) {
  if (DEBUG) {
    dump("-*- InputPickerParent: " + aStr + "\n");
  }
}

export class InputPickerParentCommon extends JSWindowActorParent {
  #namespace;
  #picker;
  #oldFocus;
  #abortController;

  constructor(namespace) {
    super();
    this.#namespace = namespace;
  }

  receiveMessage(aMessage) {
    debug("receiveMessage: " + aMessage.name);
    switch (aMessage.name) {
      case `InputPicker:Open`: {
        this.showPicker(aMessage.data);
        break;
      }
      case `InputPicker:Close`: {
        if (!this.#picker) {
          return;
        }
        this.close();
        break;
      }
      default:
        break;
    }
  }

  handleEvent(aEvent) {
    debug("handleEvent: " + aEvent.type);
    switch (aEvent.type) {
      case "InputPickerValueCleared": {
        this.sendAsyncMessage("InputPicker:ValueChanged", null);
        break;
      }
      case "InputPickerValueChanged": {
        this.sendAsyncMessage("InputPicker:ValueChanged", aEvent.detail);
        break;
      }
      case "popuphidden": {
        this.sendAsyncMessage(`InputPicker:Closed`, {});
        this.close();
        break;
      }
      default:
        break;
    }
  }

  createPickerImpl(_panel) {
    throw new Error("Not implemented");
  }

  showPicker(aData) {
    let rect = aData.rect;
    let type = aData.type;
    let detail = aData.detail;

    debug("Opening picker with details: " + JSON.stringify(detail));
    if (!this.browsingContext.canOpenModalPicker) {
      debug("Not allowed to open picker");
      return;
    }

    this.#cleanupPicker();
    let window = this.browsingContext.top.topChromeWindow;
    let doc = window.document;
    const id = `${this.#namespace}Panel`;
    let panel = doc.getElementById(id);
    if (!panel) {
      panel = doc.createXULElement("panel");
      panel.id = id;
      panel.setAttribute("type", "arrow");
      panel.setAttribute("orient", "vertical");
      panel.setAttribute("ignorekeys", "true");
      panel.setAttribute("noautofocus", "true");
      panel.setAttribute("consumeoutsideclicks", "never");
      panel.setAttribute("level", "parent");
      panel.setAttribute("tabspecific", "true");
      let container =
        doc.getElementById("mainPopupSet") ||
        doc.querySelector("popupset") ||
        doc.documentElement.appendChild(doc.createXULElement("popupset"));
      container.appendChild(panel);
    }
    this.#oldFocus = doc.activeElement;
    this.#picker = this.createPickerImpl(panel);
    this.#picker.openPicker(type, rect, detail);
    this.addPickerListeners(panel);
  }

  #cleanupPicker() {
    if (!this.#picker) {
      return;
    }
    this.#picker.closePicker();
    this.#abortController.abort();
    this.#picker = null;
  }

  close() {
    this.#cleanupPicker();
    this.#oldFocus?.focus();
    this.#oldFocus = null;
  }

  addPickerListeners(panel) {
    if (!this.#picker) {
      return;
    }
    this.#abortController = new AbortController();
    const { signal } = this.#abortController;
    panel.addEventListener("popuphidden", this, { signal });
    panel.addEventListener("InputPickerValueChanged", this, {
      signal,
    });
    panel.addEventListener("InputPickerValueCleared", this, {
      signal,
    });
  }
}
