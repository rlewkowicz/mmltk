/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

window.MozXULElement?.insertFTLIfNeeded("toolkit/global/mozSupportLink.ftl");

export default class MozSupportLink extends HTMLAnchorElement {
  static SUPPORT_URL = "https://www.mozilla.org/";
  static get observedAttributes() {
    return ["support-page", "utm-content"];
  }

  #register() {
    if (window.document.nodePrincipal?.isSystemPrincipal) {
      ChromeUtils.defineESModuleGetters(MozSupportLink, {
        BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
      });

      // eslint-disable-next-line no-shadow
      let { XPCOMUtils } = window.XPCOMUtils
        ? window
        : ChromeUtils.importESModule(
            "resource://gre/modules/XPCOMUtils.sys.mjs"
          );
      XPCOMUtils.defineLazyPreferenceGetter(
        MozSupportLink,
        "SUPPORT_URL",
        "app.support.baseURL",
        "",
        null,
        val => Services.urlFormatter.formatURL(val)
      );
    } else if (!window.IS_STORYBOOK) {
      MozSupportLink.SUPPORT_URL = window.RPMGetFormatURLPref(
        "app.support.baseURL"
      );
    }
  }

  connectedCallback() {
    this.#register();
    this.#setHref();
    this.setAttribute("target", "_blank");
    this.addEventListener("click", this);
    if (
      !this.getAttribute("data-l10n-id") &&
      !this.getAttribute("data-l10n-name") &&
      !this.childElementCount
    ) {
      document.l10n.setAttributes(this, "moz-support-link-text");
    }
    document.l10n.translateFragment(this);
  }

  disconnectedCallback() {
    this.removeEventListener("click", this);
  }

  get supportPage() {
    return this.getAttribute("support-page");
  }

  set supportPage(val) {
    this.setAttribute("support-page", val);
  }

  handleEvent(e) {
    if (e.type == "click") {
      if (window.openTrustedLinkIn) {
        let where = MozSupportLink.BrowserUtils.whereToOpenLink(e, false, true);
        if (where == "current") {
          where = "tab";
        }
        e.preventDefault();
        openTrustedLinkIn(this.href, where);
      }
    }
  }

  attributeChangedCallback(attrName) {
    if (attrName === "support-page" || attrName === "utm-content") {
      this.#setHref();
    }
  }

  #setHref() {
    let supportPage = this.getAttribute("support-page") ?? "";
    let base = MozSupportLink.SUPPORT_URL + supportPage;
    this.href = this.hasAttribute("utm-content")
      ? formatUTMParams(this.getAttribute("utm-content"), base)
      : base;
  }
}
customElements.define("moz-support-link", MozSupportLink, { extends: "a" });

export function formatUTMParams(contentAttribute, url) {
  if (!contentAttribute) {
    return url;
  }
  let parsedUrl = new URL(url);
  let domain = `.${parsedUrl.hostname}`;
  if (
    !domain.endsWith(".mozilla.org") &&
    !domain.endsWith(".allizom.org")
  ) {
    return url;
  }

  parsedUrl.searchParams.set("utm_source", "firefox-browser");
  parsedUrl.searchParams.set("utm_medium", "firefox-browser");
  parsedUrl.searchParams.set("utm_content", contentAttribute);
  return parsedUrl.href;
}
