/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export const OpenGraphPageData = {
  collect(document) {
    let pageData = {};

    let openGraphTags = document.querySelectorAll("meta[property^='og:'");

    for (let tag of openGraphTags) {
      let propertyName = tag.getAttribute("property").substring(3);

      switch (propertyName) {
        case "description":
          pageData.description = tag.getAttribute("content");
          break;
        case "site_name":
          pageData.siteName = tag.getAttribute("content");
          break;
        case "image":
          pageData.image = tag.getAttribute("content");
          break;
      }
    }

    return pageData;
  },
};
