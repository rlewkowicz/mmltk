/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import {
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AboutPagesUtils: "resource://gre/modules/AboutPagesUtils.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

export class UrlbarProviderAboutPages extends UrlbarProvider {
  get type() {
    return UrlbarUtils.PROVIDER_TYPE.PROFILE;
  }

  async isActive(queryContext) {
    return queryContext.trimmedLowerCaseSearchString.startsWith("about:");
  }

  startQuery(queryContext, addCallback) {
    let searchString = queryContext.trimmedLowerCaseSearchString;
    for (const aboutUrl of lazy.AboutPagesUtils.visibleAboutUrls) {
      if (aboutUrl.startsWith(searchString)) {
        let result = new lazy.UrlbarResult({
          type: lazy.UrlbarShared.RESULT_TYPE.URL,
          source: lazy.UrlbarShared.RESULT_SOURCE.OTHER_LOCAL,
          payload: {
            title: aboutUrl,
            url: aboutUrl,
            icon: UrlbarUtils.getIconForUrl(aboutUrl),
          },
          highlights: {
            title: UrlbarUtils.HIGHLIGHT.TYPED,
            url: UrlbarUtils.HIGHLIGHT.TYPED,
          },
        });
        addCallback(this, result);
      }
    }
  }
}
