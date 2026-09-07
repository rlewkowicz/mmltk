/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

Services.obs.addObserver(
  {
    observe(doc) {
      if (
        doc.nodePrincipal.isSystemPrincipal &&
        (doc.contentType == "application/xhtml+xml" ||
          doc.contentType == "text/html") &&
        doc.URL != "about:blank"
      ) {
        Services.scriptloader.loadSubScript(
          "chrome://global/content/customElements.js",
          doc.documentGlobal
        );
      }
    },
  },
  "document-element-inserted"
);
