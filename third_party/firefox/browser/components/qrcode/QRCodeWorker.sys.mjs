/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { BasePromiseWorker } from "resource://gre/modules/PromiseWorker.sys.mjs";

const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "logConsole", function () {
  return console.createInstance({
    prefix: "QRCodeWorker",
    maxLogLevel: Services.prefs.getBoolPref("browser.qrcode.log", false)
      ? "Debug"
      : "Warn",
  });
});

export class QRCodeWorker extends BasePromiseWorker {
  constructor() {
    super("chrome://browser/content/qrcode/QRCodeWorker.worker.mjs", {
      type: "module",
    });

    this.log = (...args) => lazy.logConsole.debug(...args);
  }

  async generateFullQRCode(url, showLogo = true) {
    return this.post("generateFullQRCode", [url, showLogo]);
  }

  async generateQRMatrix(url) {
    return this.post("generateQRMatrix", [url]);
  }

  async getLogoPlacement(dotCount, margin) {
    return this.post("getLogoPlacement", [dotCount, margin]);
  }

  async terminate() {
    super.terminate();
  }
}
