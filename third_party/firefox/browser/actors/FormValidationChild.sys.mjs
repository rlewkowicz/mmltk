/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


export class FormValidationChild extends JSWindowActorChild {
  constructor() {
    super();
    this._validationMessage = "";
    this._element = null;
  }


  handleEvent(aEvent) {
    switch (aEvent.type) {
      case "MozInvalidForm":
        aEvent.preventDefault();
        this.notifyInvalidSubmit(aEvent.detail);
        break;
      case "pageshow":
        if (this._isRootDocumentEvent(aEvent)) {
          this._hidePopup();
        }
        break;
      case "pagehide":
        this._onBlur();
        break;
      case "input":
        this._onInput(aEvent);
        break;
      case "blur":
        this._onBlur(aEvent);
        break;
    }
  }

  notifyInvalidSubmit(aInvalidElements) {
    for (let element of aInvalidElements) {
      if (this.contentWindow != element.documentGlobal) {
        return;
      }

      if (
        !(
          ChromeUtils.getClassName(element) === "HTMLInputElement" ||
          ChromeUtils.getClassName(element) === "HTMLTextAreaElement" ||
          ChromeUtils.getClassName(element) === "HTMLSelectElement" ||
          ChromeUtils.getClassName(element) === "HTMLButtonElement" ||
          element.isFormAssociatedCustomElement
        )
      ) {
        continue;
      }

      let validationMessage = element.isFormAssociatedCustomElement
        ? element.internals.validationMessage
        : element.validationMessage;

      if (element.isFormAssociatedCustomElement) {
        element = element.internals.validationAnchor || element;
      }

      if (!element || !Services.focus.elementIsFocusable(element, 0)) {
        continue;
      }

      this._validationMessage = validationMessage;

      if (this._element == element) {
        this._showPopup(element);
        break;
      }
      this._element = element;

      element.focus();

      element.addEventListener("input", this);

      element.addEventListener("blur", this);

      this._showPopup(element);
      break;
    }
  }


  _onInput(aEvent) {
    let element = aEvent.originalTarget;

    if (element.validity.valid) {
      this._hidePopup();
      return;
    }

    if (this._validationMessage != element.validationMessage) {
      this._validationMessage = element.validationMessage;
      this._showPopup(element);
    }
  }

  _onBlur() {
    if (this._element) {
      this._element.removeEventListener("input", this);
      this._element.removeEventListener("blur", this);
    }
    this._hidePopup();
    this._element = null;
  }

  _showPopup(aElement) {
    let panelData = {};
    let win = aElement.documentGlobal;

    panelData.message = this._validationMessage;

    panelData.screenRect =
      win.windowUtils.getElementBoundingScreenRect(aElement);

    if (
      aElement.tagName == "INPUT" &&
      (aElement.type == "radio" || aElement.type == "checkbox")
    ) {
      panelData.position = "bottomcenter topleft";
    } else {
      panelData.position = "after_start";
    }
    this.sendAsyncMessage("FormValidation:ShowPopup", panelData);

    win.addEventListener("pagehide", this, {
      mozSystemGroup: true,
    });
  }

  _hidePopup() {
    this.sendAsyncMessage("FormValidation:HidePopup", {});
    this._element.documentGlobal.removeEventListener("pagehide", this, {
      mozSystemGroup: true,
    });
  }

  _isRootDocumentEvent(aEvent) {
    if (this.contentWindow == null) {
      return true;
    }
    let target = aEvent.originalTarget;
    return (
      target == this.document ||
      (target.ownerDocument && target.ownerDocument == this.document)
    );
  }
}
