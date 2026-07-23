/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import MozInputText from "chrome://global/content/elements/moz-input-text.mjs";

export default class MozInputNumber extends MozInputText {
  inputTemplate() {
    return super.inputTemplate({ type: "number" });
  }
}
customElements.define("moz-input-number", MozInputNumber);
