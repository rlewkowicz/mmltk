/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export class PageStyleParent extends JSWindowActorParent {
  #styleSheetInfo = null;

  receiveMessage(msg) {
    let browser = this.browsingContext.top.embedderElement;
    if (!browser || browser.documentGlobal.closed) {
      return;
    }

    let actor =
      this.browsingContext.top.currentWindowGlobal.getActor("PageStyle");
    switch (msg.name) {
      case "PageStyle:Add":
        actor.addSheetInfo(msg.data);
        break;
      case "PageStyle:Clear":
        if (actor == this) {
          this.#styleSheetInfo = null;
        }
        break;
    }
  }

  addSheetInfo(newSheetData) {
    let info = this.getSheetInfo();
    info.filteredStyleSheets.push(...newSheetData.filteredStyleSheets);
    info.preferredStyleSheetSet ||= newSheetData.preferredStyleSheetSet;
  }

  getSheetInfo() {
    if (!this.#styleSheetInfo) {
      this.#styleSheetInfo = {
        filteredStyleSheets: [],
        preferredStyleSheetSet: true,
      };
    }
    return this.#styleSheetInfo;
  }
}
