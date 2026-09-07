/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { InputPickerChildCommon } from "./InputPickerChildCommon.sys.mjs";

export class DateTimePickerChild extends InputPickerChildCommon {
  constructor() {
    super("DateTimePicker");
  }

  closeImpl(inputElement) {
    let dateTimeBoxElement = inputElement.dateTimeBoxElement;
    if (!dateTimeBoxElement) {
      return;
    }

    let win = inputElement.documentGlobal;
    dateTimeBoxElement.dispatchEvent(
      new win.CustomEvent("MozSetDateTimePickerState", { detail: false })
    );
  }

  getTimePickerPref() {
    return Services.prefs.getBoolPref("dom.forms.datetime.timepicker");
  }

  pickerValueChangedImpl(aMessage, inputElement) {
    let dateTimeBoxElement = inputElement.dateTimeBoxElement;
    if (!dateTimeBoxElement) {
      return;
    }

    let win = inputElement.documentGlobal;

    dateTimeBoxElement.dispatchEvent(
      new win.CustomEvent("MozPickerValueChanged", {
        detail: Cu.cloneInto(aMessage.data, win),
      })
    );
  }

  openPickerImpl(inputElement) {
    if (inputElement.type == "time" && !this.getTimePickerPref()) {
      return undefined;
    }

    let dateTimeBoxElement = inputElement.dateTimeBoxElement;
    if (!dateTimeBoxElement) {
      throw new Error("How do we get this event without a UA Widget?");
    }

    let win = inputElement.documentGlobal;
    dateTimeBoxElement.dispatchEvent(
      new win.CustomEvent("MozSetDateTimePickerState", { detail: true })
    );

    let value = inputElement.getDateTimeInputBoxValue();
    return {
      value: Object.keys(value).length ? value : inputElement.value,
      min: inputElement.getMinimum(),
      max: inputElement.getMaximum(),
      step: inputElement.getStep(),
      stepBase: inputElement.getStepBase(),
    };
  }
}
