/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// tools/lint/eslint/eslint-plugin-mozilla/lib/environments/browser-window.js

// prettier-ignore
// eslint-disable-next-line no-lone-blocks
{
  Services.scriptloader.loadSubScript("chrome://browser/content/browser.js", this);
  if (AppConstants.MOZ_PLACES) {
    Services.scriptloader.loadSubScript("chrome://browser/content/places/browserPlacesViews.js", this);
  }
  Services.scriptloader.loadSubScript("chrome://global/content/globalOverlay.js", this);
  Services.scriptloader.loadSubScript("chrome://global/content/editMenuOverlay.js", this);
  Services.scriptloader.loadSubScript("chrome://browser/content/utilityOverlay.js", this);
  Services.scriptloader.loadSubScript("chrome://browser/content/browser-sets.js", this);
  if (AppConstants.platform == "macosx") {
    Services.scriptloader.loadSubScript("chrome://global/content/macWindowMenu.js", this);
  }
}
