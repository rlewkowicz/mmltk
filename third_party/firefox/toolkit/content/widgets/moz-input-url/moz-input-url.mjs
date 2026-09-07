/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import MozInputText from "chrome://global/content/elements/moz-input-text.mjs";

export default class MozInputUrl extends MozInputText {
  inputTemplate() {
    return super.inputTemplate({ type: "url" });
  }
}
customElements.define("moz-input-url", MozInputUrl);
