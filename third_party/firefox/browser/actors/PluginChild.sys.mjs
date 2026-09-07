/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export class PluginChild extends JSWindowActorChild {
  handleEvent(event) {
    let eventDoc = event.target.ownerDocument || event.target.document;
    if (eventDoc && eventDoc != this.document) {
      return;
    }

    let eventType = event.type;
    if (eventType == "PluginCrashed") {
      this.onPluginCrashed(event);
    }
  }

  isWithinFullScreenElement(fullScreenElement, domElement) {
    let getTrueFullScreenElement = fullScreenIframe => {
      if (
        typeof fullScreenIframe.contentDocument !== "undefined" &&
        fullScreenIframe.contentDocument.mozFullScreenElement
      ) {
        return getTrueFullScreenElement(
          fullScreenIframe.contentDocument.mozFullScreenElement
        );
      }
      return fullScreenIframe;
    };

    if (fullScreenElement.tagName === "IFRAME") {
      fullScreenElement = getTrueFullScreenElement(fullScreenElement);
    }

    if (fullScreenElement.contains(domElement)) {
      return true;
    }
    let parentIframe = domElement.documentGlobal.frameElement;
    if (parentIframe) {
      return this.isWithinFullScreenElement(fullScreenElement, parentIframe);
    }
    return false;
  }

  async onPluginCrashed(aEvent) {
    if (!this.contentWindow.PluginCrashedEvent.isInstance(aEvent)) {
      return;
    }

    let { target, gmpPlugin, pluginID } = aEvent;
    let fullScreenElement =
      this.contentWindow.top.document.mozFullScreenElement;
    if (fullScreenElement) {
      if (this.isWithinFullScreenElement(fullScreenElement, target)) {
        this.contentWindow.top.document.mozCancelFullScreen();
      }
    }

    if (!gmpPlugin || !target.document) {
      return;
    }

    this.sendAsyncMessage("PluginContent:ShowPluginCrashedNotification", {
      pluginCrashID: { pluginID },
    });
  }
}
