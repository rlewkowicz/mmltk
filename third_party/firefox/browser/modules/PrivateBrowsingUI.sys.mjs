/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

import { PrivateBrowsingUtils } from "resource://gre/modules/PrivateBrowsingUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { PanelMultiView } from "moz-src:///browser/components/customizableui/PanelMultiView.sys.mjs";

export const PrivateBrowsingUI = {
  init: function PBUI_init(window) {
    if (!PrivateBrowsingUtils.isWindowPrivate(window)) {
      return;
    }
    const document = window.document;
    const gBrowser = window.gBrowser;

    document.getElementById("Tools:Sanitize").setAttribute("disabled", "true");

    if (window.location.href != AppConstants.BROWSER_CHROME_URL) {
      return;
    }

    let docElement = document.documentElement;
    docElement.setAttribute(
      "privatebrowsingmode",
      PrivateBrowsingUtils.permanentPrivateBrowsing ? "permanent" : "temporary"
    );

    gBrowser.updateTitlebar();

    if (PrivateBrowsingUtils.permanentPrivateBrowsing) {
      let hideNewWindowItem = (windowItem, privateWindowItem) => {
        privateWindowItem.hidden = true;
        windowItem.setAttribute(
          "data-l10n-id",
          privateWindowItem.getAttribute("data-l10n-id")
        );
      };

      hideNewWindowItem(
        document.getElementById("menu_newNavigator"),
        document.getElementById("menu_newPrivateWindow")
      );
      hideNewWindowItem(
        PanelMultiView.getViewNode(document, "appMenu-new-window-button2"),
        PanelMultiView.getViewNode(
          document,
          "appMenu-new-private-window-button2"
        )
      );
    }
  },
};
