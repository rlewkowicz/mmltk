/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  ASRouter: "resource:///modules/asrouter/ASRouter.sys.mjs",
  BookmarksBarButton: "resource:///modules/asrouter/BookmarksBarButton.sys.mjs",
  CFRPageActions: "resource:///modules/asrouter/CFRPageActions.sys.mjs",
  CustomizableUI:
    "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs",
  FeatureCalloutBroker:
    "resource:///modules/asrouter/FeatureCalloutBroker.sys.mjs",
  InfoBar: "resource:///modules/asrouter/InfoBar.sys.mjs",
  JsonSchema: "resource://gre/modules/JsonSchema.sys.mjs",
  MessageLoaderUtils: "resource:///modules/asrouter/ASRouter.sys.mjs",
  SpecialMessageActions:
    "resource://messaging-system/lib/SpecialMessageActions.sys.mjs",
  Spotlight: "resource:///modules/asrouter/Spotlight.sys.mjs",

  log: () => {
    const { Logger } = ChromeUtils.importESModule(
      "resource://messaging-system/lib/Logger.sys.mjs"
    );
    return new Logger("AboutMessagePreviewParent");
  },
});

function dispatchCFRAction({ type, data }, browser) {
  if (type === "USER_ACTION") {
    lazy.SpecialMessageActions.handleAction(data, browser);
  }
}



const MESSAGE_HANDLERS = Object.freeze({
  infobar: (message, browser) =>
    lazy.InfoBar.showInfoBarMessage(browser, message, dispatchCFRAction),

  spotlight: (message, browser) =>
    lazy.Spotlight.showSpotlightDialog(browser, message, () => {}),

  cfr_doorhanger: (message, browser) =>
    lazy.CFRPageActions.forceRecommendation(
      browser,
      message,
      dispatchCFRAction
    ),

  feature_callout: async (message, browser) => {
    const tourPref = message.content.tour_pref_name;
    if (tourPref) {
      Services.prefs.clearUserPref(tourPref);
    }
    message.trigger = { id: "nthTabClosed" };
    message.targeting = "true";
    const showing = await lazy.FeatureCalloutBroker.showFeatureCallout(
      browser,
      message
    );
    if (!showing) {
      for (const screen of message.content.screens) {
        const existingAnchors = screen.anchors;
        const fallbackAnchor = { selector: "#star-button-box" };

        if (existingAnchors[0].hasOwnProperty("arrow_position")) {
          fallbackAnchor.arrow_position = "top-center-arrow-end";
        } else {
          fallbackAnchor.panel_position = {
            anchor_attachment: "bottomcenter",
            callout_attachment: "topright",
          };
        }

        screen.anchors = [...existingAnchors, fallbackAnchor];
        lazy.log.debug("ANCHORS: ", screen.anchors);
      }
      await lazy.FeatureCalloutBroker.showFeatureCallout(browser, message);
    }
  },

  bookmarks_bar_button: (message, browser) => {
    lazy.CustomizableUI.setToolbarVisibility(
      lazy.CustomizableUI.AREA_BOOKMARKS,
      true
    );
    lazy.BookmarksBarButton.showBookmarksBarButton(browser, message);
  },

  pb_newtab: (message, browser) =>
    lazy.ASRouter.forcePBWindow(browser, message),
});

export class AboutMessagePreviewParent extends JSWindowActorParent {
  static getSupportedTemplates() {
    return Object.keys(MESSAGE_HANDLERS);
  }

  async showMessage(data, validationEnabled = true) {
    let message;
    try {
      message = JSON.parse(data);
    } catch (e) {
      lazy.log.error("Could not parse message", e);
      return;
    }

    if (validationEnabled) {
      const schema = await fetch(
        "chrome://browser/content/asrouter/schemas/MessagingExperiment.schema.json",
        { credentials: "omit" }
      ).then(rsp => rsp.json());
      const result = lazy.JsonSchema.validate(message, schema);
      if (!result.valid) {
        lazy.log.error(
          `Invalid message: ${JSON.stringify(result.errors, undefined, 2)}`
        );
      }
    }

    message = lazy.MessageLoaderUtils._delocalizeValues(message);

    const browser =
      this.browsingContext.topChromeWindow.gBrowser.selectedBrowser;

    const handler = MESSAGE_HANDLERS[message.template];

    if (handler) {
      void handler(message, browser);
    } else {
      lazy.log.error(`Unsupported message template ${message.template}`);
    }
  }

  async receiveMessage(message) {
    const { name, data, validationEnabled } = message;

    switch (name) {
      case "MessagePreview:SHOW_MESSAGE":
        await this.showMessage(data, validationEnabled);
        return;
      default:
        lazy.log.debug(`Unexpected event ${name} was not handled.`);
    }
  }
}
