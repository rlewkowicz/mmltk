/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export class AutoScrollParent extends JSWindowActorParent {
  receiveMessage(msg) {
    let browser = this.manager.browsingContext.top.embedderElement;
    if (!browser) {
      return null;
    }

    const requestedInForegroundTab = browser.isRemoteBrowser
      ? Services.focus.focusedElement == browser
      : true;

    let data = msg.data;
    switch (msg.name) {
      case "Autoscroll:Start":
        if (!requestedInForegroundTab) {
          return Promise.resolve({ autoscrollEnabled: false, usingAPZ: false });
        }
        return Promise.resolve(browser.startScroll(data));
      case "Autoscroll:MaybeStartInParent": {
        if (!requestedInForegroundTab) {
          return Promise.resolve({ autoscrollEnabled: false, usingAPZ: false });
        }
        let parent = this.browsingContext.parent;
        if (parent) {
          let actor = parent.currentWindowGlobal.getActor("AutoScroll");
          actor.sendAsyncMessage("Autoscroll:MaybeStart", data);
        }
        break;
      }
      case "Autoscroll:Cancel":
        browser.cancelScroll();
        break;
    }
    return null;
  }
}
