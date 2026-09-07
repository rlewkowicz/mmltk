/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export class GeolocationUIUtils {
  dismissPrompts(aBrowsingContext) {
    let embedder = aBrowsingContext?.top.embedderElement;
    let owner = embedder?.documentGlobal;
    if (owner) {
      let dialogBox = owner.gBrowser.getTabDialogBox(embedder);
      dialogBox.getTabDialogManager().abortDialogs();
    }
  }
}

GeolocationUIUtils.prototype.QueryInterface = ChromeUtils.generateQI([
  "nsIGeolocationUIUtils",
]);
