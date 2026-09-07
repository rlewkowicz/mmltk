/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  Interactions: "moz-src:///browser/components/places/Interactions.sys.mjs",
});

export class InteractionsParent extends JSWindowActorParent {
  receiveMessage(msg) {
    switch (msg.name) {
      case "Interactions:PageLoaded":
        lazy.Interactions.registerNewInteraction(
          this.browsingContext.embedderElement,
          msg.data
        );
        break;
      case "Interactions:PageHide":
        lazy.Interactions.registerEndOfInteraction(
          this.browsingContext?.embedderElement
        );
        break;
    }
  }
}
