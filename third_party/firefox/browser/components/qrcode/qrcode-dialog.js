/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { XPCOMUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
);

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "IDNService",
  "@mozilla.org/network/idn-service;1",
  Ci.nsIIDNService
);

const QRCodeDialog = {
  _url: null,
  _qrCodeDataURI: null,

  init() {
    if (window.arguments && window.arguments[0]) {
      const params = window.arguments[0];
      this._url = params.url;
      this._qrCodeDataURI = params.qrCodeDataURI;
    }

    document.mozSubdialogReady = this.setupDialog();

    const copyButton = document.getElementById("copy-button");

    document.subDialogSetDefaultFocus = () => copyButton.focus();

    copyButton.addEventListener("click", () => this.copyImage());
    document
      .getElementById("save-button")
      .addEventListener("click", () => this.saveImage());
    document
      .getElementById("close-button")
      .addEventListener("click", () => window.close());

    document.addEventListener("keydown", event => {
      if (
        event.key === "Enter" &&
        !event.defaultPrevented &&
        !event.target.closest("moz-button")
      ) {
        event.preventDefault();
        this.copyImage();
      }
    });
  },

  async setupDialog() {
    const successContainer = document.getElementById("success-container");

    if (!this._qrCodeDataURI) {
      this.showFeedback("error", "qrcode-panel-error");
      return;
    }

    const imageElement = document.getElementById("qrcode-image");
    imageElement.src = this._qrCodeDataURI;

    successContainer.hidden = false;

    const urlElement = document.getElementById("qrcode-url");
    urlElement.textContent = this._url;
    urlElement.title = this._url;
  },

  async showFeedback(type, l10nId) {
    let bar = document.getElementById("feedback-bar");
    if (!bar) {
      bar = document.createElement("moz-message-bar");
      bar.id = "feedback-bar";
      bar.setAttribute("role", "alert");
      bar.setAttribute("data-l10n-attrs", "message");
      bar.setAttribute("dismissable", "");
      bar.addEventListener("message-bar:user-dismissed", () => {
        requestAnimationFrame(() => window.resizeDialog?.());
      });
      let content = document.getElementById("qrcode-dialog-content");
      content.appendChild(bar);
    }
    bar.type = type;
    document.l10n.setAttributes(bar, l10nId);
    await bar.updateComplete;
    window.resizeDialog?.();
  },

  decodeDataURI() {
    const dataPrefix = "data:image/png;base64,";
    if (!this._qrCodeDataURI?.startsWith(dataPrefix)) {
      throw new Error("Invalid QR code image data");
    }

    return Uint8Array.fromBase64(this._qrCodeDataURI.slice(dataPrefix.length));
  },

  async copyImage() {
    try {
      const qrCodeBytes = this.decodeDataURI();
      const item = new ClipboardItem({
        "image/png": new Blob([qrCodeBytes], { type: "image/png" }),
      });
      await navigator.clipboard.write([item]);
      this.showFeedback("success", "qrcode-copy-success");
    } catch (error) {
      console.error("Failed to copy QR code:", error);
      this.showFeedback("error", "qrcode-copy-error");
    }
  },

  async saveImage() {
    let domain = "";
    let uri;
    try {
      uri = Services.io.newURI(this._url);
      domain = lazy.IDNService.domainToDisplay(
        Services.eTLD.getSchemelessSite(uri)
      );
    } catch (e) {
      if (uri) {
        domain = uri.host;
      }
    }

    const filenameMessage = domain
      ? { id: "qrcode-save-filename-with-domain-base", args: { domain } }
      : "qrcode-save-filename-base";
    const [defaultFilename] = await document.l10n.formatValues([
      filenameMessage,
    ]);
    const filename = `${defaultFilename}.png`;

    const chromeWindow = window.browsingContext.topChromeWindow;
    chromeWindow.internalSave(
      this._qrCodeDataURI,
      null, 
      null, 
      filename,
      null, 
      "image/png",
      true, 
      "SaveImageTitle",
      null, 
      null, 
      null, 
      null, 
      false, 
      null, 
      lazy.PrivateBrowsingUtils.isWindowPrivate(chromeWindow),
      Services.scriptSecurityManager.getSystemPrincipal()
    );
  },
};

window.QRCodeDialog = QRCodeDialog;

window.addEventListener("DOMContentLoaded", () => {
  QRCodeDialog.init();
});
