/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  FaviconLoader: "resource:///modules/FaviconLoader.sys.mjs",
});

export class LinkHandlerChild extends JSWindowActorChild {
  constructor() {
    super();

    this.seenTabIcon = false;
    this._iconLoader = null;
  }

  get iconLoader() {
    if (!this._iconLoader) {
      this._iconLoader = new lazy.FaviconLoader(this);
    }
    return this._iconLoader;
  }

  addRootIcon() {
    if (
      !this.seenTabIcon &&
      Services.prefs.getBoolPref("browser.chrome.guess_favicon", true) &&
      Services.prefs.getBoolPref("browser.chrome.site_icons", true)
    ) {
      let pageURI = this.document.documentURIObject;
      if (["http", "https"].includes(pageURI.scheme)) {
        this.seenTabIcon = true;
        this.iconLoader.addDefaultIcon(pageURI);
      }
    }
  }

  onHeadParsed(event) {
    if (event.target.ownerDocument != this.document) {
      return;
    }

    this.addRootIcon();

    if (this._iconLoader) {
      this._iconLoader.onPageShow();
    }
  }

  onPageShow(event) {
    if (event.target != this.document) {
      return;
    }

    this.addRootIcon();

    if (this._iconLoader) {
      this._iconLoader.onPageShow();
    }
  }

  onPageHide(event) {
    if (event.target != this.document) {
      return;
    }

    if (this._iconLoader) {
      this._iconLoader.onPageHide();
    }

    this.seenTabIcon = false;
  }

  onLinkEvent(event) {
    let link = event.target;
    if (link.documentGlobal != this.contentWindow) {
      return;
    }

    let rel = link.rel && link.rel.toLowerCase();
    if (!rel || !link.href || !link.getAttribute("href")) {
      return;
    }

    let iconAdded = false;
    let searchAdded = false;
    let rels = {};
    for (let relString of rel.split(/\s+/)) {
      rels[relString] = true;
    }

    for (let relVal in rels) {
      let isRichIcon = false;

      switch (relVal) {
        case "apple-touch-icon":
        case "apple-touch-icon-precomposed":
        case "fluid-icon":
          isRichIcon = true;
        // fall through
        case "icon":
          if (iconAdded || link.hasAttribute("color") || rel.includes("mask")) {
            break;
          }

          if (!Services.prefs.getBoolPref("browser.chrome.site_icons", true)) {
            return;
          }

          if (this.iconLoader.addIconFromLink(link, isRichIcon)) {
            iconAdded = true;
            if (!isRichIcon) {
              this.seenTabIcon = true;
            }
          }
          break;
        case "search":
          if (!searchAdded && event.type == "DOMLinkAdded") {
            let type = link.type && link.type.toLowerCase();
            type = type.replace(/^\s+|\s*(?:;.*)?$/g, "");

            let re = /^https?:/i;
            if (
              type == "application/opensearchdescription+xml" &&
              link.title &&
              re.test(link.href)
            ) {
              let engine = { title: link.title, href: link.href };
              this.sendAsyncMessage("Link:AddSearch", {
                engine,
              });
              searchAdded = true;
            }
          }
          break;
      }
    }
  }

  handleEvent(event) {
    switch (event.type) {
      case "pageshow":
        return this.onPageShow(event);
      case "pagehide":
        return this.onPageHide(event);
      case "DOMHeadElementParsed":
        return this.onHeadParsed(event);
      default:
        return this.onLinkEvent(event);
    }
  }
}
