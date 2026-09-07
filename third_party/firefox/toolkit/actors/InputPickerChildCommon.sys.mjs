/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export class InputPickerChildCommon extends JSWindowActorChild {
  #inputElement = null;
  #inputType = "";
  #namespace;
  #abortController;

  constructor(namespace) {
    super();
    this.#namespace = namespace;
  }

  close() {
    this.#abortController.abort();
    this.closeImpl(this.#inputElement);
    this.#inputElement = null;
    this.#inputType = "";
  }

  closeImpl(_inputElement) {
    throw new Error("Not implemented");
  }

  addListeners(aElement) {
    this.#abortController = new AbortController();
    aElement.documentGlobal.addEventListener("pagehide", this, {
      signal: this.#abortController.signal,
    });
  }

  getComputedDirection(aElement) {
    return aElement.documentGlobal
      .getComputedStyle(aElement)
      .getPropertyValue("direction");
  }

  getBoundingContentRect(aElement) {
    let win = aElement.documentGlobal;
    return win.windowUtils.getElementBoundingScreenRect(aElement);
  }

  receiveMessage(aMessage) {
    if (!this.#inputElement || this.#inputElement.type !== this.#inputType) {
      return;
    }
    switch (aMessage.name) {
      case "InputPicker:Closed": {
        this.close();
        break;
      }
      case "InputPicker:ValueChanged": {
        this.pickerValueChangedImpl(aMessage, this.#inputElement);
        break;
      }
    }
  }

  pickerValueChangedImpl(_aMessage, _inputElement) {
    throw new Error("Not implemented");
  }

  handleEvent(aEvent) {
    switch (aEvent.type) {
      case `MozOpen${this.#namespace}`: {
        if (
          !aEvent.originalTarget.documentGlobal.HTMLInputElement.isInstance(
            aEvent.originalTarget
          )
        ) {
          return;
        }

        if (this.#inputElement) {
          return;
        }

        const inputElement = aEvent.originalTarget;
        const openPickerDetail = this.openPickerImpl(inputElement);
        if (!openPickerDetail) {
          return;
        }

        this.#inputElement = inputElement;
        this.#inputType = inputElement.type;
        this.addListeners(inputElement);

        this.sendAsyncMessage(`InputPicker:Open`, {
          rect: this.getBoundingContentRect(inputElement),
          dir: this.getComputedDirection(inputElement),
          type: inputElement.type,
          detail: openPickerDetail,
        });
        break;
      }
      case `MozClose${this.#namespace}`: {
        this.sendAsyncMessage(`InputPicker:Close`, {});
        this.close();
        break;
      }
      case "pagehide": {
        if (this.#inputElement?.ownerDocument == aEvent.target) {
          this.sendAsyncMessage(`InputPicker:Close`, {});
          this.close();
        }
        break;
      }
      default:
        break;
    }
  }

  openPickerImpl(_inputElement) {
    throw new Error("Not implemented");
  }

  updatePickerImpl(_inputElement) {
    throw new Error("Not implemented");
  }
}
