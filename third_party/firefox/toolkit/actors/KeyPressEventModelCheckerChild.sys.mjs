/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

export class KeyPressEventModelCheckerChild extends JSWindowActorChild {
  handleEvent(aEvent) {
    if (!AppConstants.DEBUG) {
      aEvent.stopImmediatePropagation();
    }

    let model = Document.KEYPRESS_EVENT_MODEL_DEFAULT;
    if (
      this._isOldOfficeOnlineServer(aEvent.target) ||
      this._isOldConfluence(aEvent.target.documentGlobal)
    ) {
      model = Document.KEYPRESS_EVENT_MODEL_SPLIT;
    }
    aEvent.target.setKeyPressEventModel(model);
  }

  _isOldOfficeOnlineServer(aDocument) {
    let editingElement = aDocument.getElementById(
      "WACViewPanel_EditingElement"
    );
    if (!editingElement) {
      return false;
    }
    let isOldVersion = !editingElement.classList.contains(
      "WACViewPanel_DisableLegacyKeyCodeAndCharCode"
    );
    return isOldVersion;
  }

  _isOldConfluence(aWindow) {
    if (!aWindow) {
      return false;
    }
    let tinyMCEObject;
    try {
      tinyMCEObject = ChromeUtils.waiveXrays(aWindow.parent).tinyMCE;
    } catch (e) {
    }
    if (!tinyMCEObject) {
      try {
        tinyMCEObject = ChromeUtils.waiveXrays(aWindow).tinyMCE;
      } catch (e) {
      }
      if (!tinyMCEObject) {
        return false;
      }
    }
    try {
      let { author, version } =
        new tinyMCEObject.plugins.CursorTargetPlugin().getInfo();
      if (author !== "Atlassian") {
        return false;
      }
      let isOldVersion = version === "1.0";
      return isOldVersion;
    } catch (e) {
      return false;
    }
  }
}
