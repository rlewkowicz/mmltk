/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export class BrowserElementParent extends JSWindowActorParent {
  receiveMessage(message) {
    switch (message.name) {
      case "DOMWindowClose": {
        if (!this.manager.browsingContext.parent) {
          let browser = this.manager.browsingContext.embedderElement;
          let win = browser.documentGlobal;
          if (browser.isRemoteBrowser) {
            browser.dispatchEvent(
              new win.CustomEvent("DOMWindowClose", {
                bubbles: true,
              })
            );
          }
        }
        break;
      }
    }
  }
}
