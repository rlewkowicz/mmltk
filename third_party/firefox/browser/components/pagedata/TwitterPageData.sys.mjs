/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export const TwitterPageData = {
  collect(document) {
    let pageData = {};

    let twitterTags = document.querySelectorAll("meta[name^='twitter:'");

    for (let tag of twitterTags) {
      let propertyName = tag.getAttribute("name").substring(8);

      switch (propertyName) {
        case "site":
          pageData.siteName = tag.getAttribute("content");
          break;
        case "description":
          pageData.description = tag.getAttribute("content");
          break;
        case "image":
          pageData.image = tag.getAttribute("content");
          break;
      }
    }

    return pageData;
  },
};
