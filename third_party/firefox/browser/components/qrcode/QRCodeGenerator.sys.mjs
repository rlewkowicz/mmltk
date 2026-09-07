/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "logConsole", function () {
  return console.createInstance({
    prefix: "QRCodeGenerator",
    maxLogLevel: Services.prefs.getBoolPref("browser.qrcode.log", false)
      ? "Debug"
      : "Warn",
  });
});

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

ChromeUtils.defineESModuleGetters(lazy, {
  QRCodeWorker: "moz-src:///browser/components/qrcode/QRCodeWorker.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "embedLogo",
  "browser.shareqrcode.embed_logo",
  true
);

export const QRCodeGenerator = {
  async generateQRCode(url) {
    const worker = new lazy.QRCodeWorker();
    try {
      const dataURI = await worker.generateFullQRCode(url, lazy.embedLogo);
      lazy.logConsole.debug("QRCode worker generated full QR code");
      return dataURI;
    } finally {
      try {
        await worker.terminate();
        lazy.logConsole.debug("QRCode worker terminated successfully");
      } catch (e) {
        lazy.logConsole.warn("Failed to terminate QRCode worker:", e);
      }
    }
  },
};
