/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const Cm = Components.manager;
Cm.QueryInterface(Ci.nsIServiceManager);

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "BROWSER_STARTUP_RECORD",
  "browser.startup.record",
  false
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "BROWSER_STARTUP_RECORD_IMAGES",
  "browser.startup.recordImages",
  false
);

let firstPaintNotification = "xul-window-visible";
if (AppConstants.platform == "linux") {
  firstPaintNotification = "document-shown";
}

let win, canvas;
let paints = [];
let afterPaintListener = () => {
  let width, height;
  canvas.width = width = win.innerWidth;
  canvas.height = height = win.innerHeight;
  if (width < 1 || height < 1) {
    return;
  }
  let ctx = canvas.getContext("2d", { alpha: false, willReadFrequently: true });

  ctx.drawWindow(
    win,
    0,
    0,
    width,
    height,
    "white",
    ctx.DRAWWINDOW_DO_NOT_FLUSH |
      ctx.DRAWWINDOW_DRAW_VIEW |
      ctx.DRAWWINDOW_ASYNC_DECODE_IMAGES |
      ctx.DRAWWINDOW_USE_WIDGET_LAYERS
  );
  paints.push({
    data: ctx.getImageData(0, 0, width, height).data,
    width,
    height,
  });
};

export function StartupRecorder() {
  this.wrappedJSObject = this;
  this.data = {
    images: {
      "image-drawing": new Set(),
      "image-loading": new Set(),
    },
    code: {},
    prefStats: {},
  };
  this.done = new Promise(resolve => {
    this._resolve = resolve;
  });
}

StartupRecorder.prototype = {
  QueryInterface: ChromeUtils.generateQI(["nsIObserver"]),

  record(name) {
    this.data.code[name] = {
      modules: Cu.loadedESModules,
      services: Object.keys(Cc).filter(c => {
        try {
          return Cm.isServiceInstantiatedByContractID(c, Ci.nsISupports);
        } catch (e) {
          return false;
        }
      }),
    };
  },

  observe(subject, topic, data) {
    if (topic == "app-startup" || topic == "content-process-ready-for-script") {
      if (Services.appinfo.ID != "{ec8030f7-c20a-464f-9b0e-13a3a9e97384}") {
        return;
      }

      if (!lazy.BROWSER_STARTUP_RECORD && !lazy.BROWSER_STARTUP_RECORD_IMAGES) {
        this._resolve();
        this._resolve = null;
        return;
      }

      let topics = [
        "profile-do-change", 
        "toplevel-window-ready", 
        firstPaintNotification,
        "sessionstore-windows-restored",
        "browser-startup-idle-tasks-finished",
      ];

      if (lazy.BROWSER_STARTUP_RECORD_IMAGES) {
        topics = [
          "image-loading",
          "image-drawing",
          "browser-startup-idle-tasks-finished",
        ];
      }
      for (let t of topics) {
        Services.obs.addObserver(this, t);
      }
      return;
    }

    if (topic == firstPaintNotification) {
      if (subject instanceof Ci.nsIAppWindow) {
        subject = subject
          .QueryInterface(Ci.nsIInterfaceRequestor)
          .getInterface(Ci.nsIDOMWindow);
      }

      let doc = topic == "document-shown" ? subject : subject.document;

      if (
        doc.documentElement.getAttribute("windowtype") != "navigator:browser"
      ) {
        return;
      }
    }

    if (topic == "image-drawing" || topic == "image-loading") {
      this.data.images[topic].add(data);
      return;
    }

    Services.obs.removeObserver(this, topic);

    if (topic == firstPaintNotification) {
      win = topic == "document-shown" ? subject.defaultView : subject;
      canvas = win.document.createElementNS(
        "http://www.w3.org/1999/xhtml",
        "canvas"
      );
      canvas.mozOpaque = true;
      afterPaintListener();
      win.addEventListener("MozAfterPaint", afterPaintListener);
    }

    if (topic == "sessionstore-windows-restored") {
      Services.tm.dispatchToMainThread(
        this.record.bind(this, "before handling user events")
      );
    } else if (topic == "browser-startup-idle-tasks-finished") {
      if (lazy.BROWSER_STARTUP_RECORD_IMAGES) {
        Services.obs.removeObserver(this, "image-drawing");
        Services.obs.removeObserver(this, "image-loading");
        this._resolve();
        this._resolve = null;
        return;
      }

      this.record("before becoming idle");
      win.removeEventListener("MozAfterPaint", afterPaintListener);
      win = null;
      this.data.frames = paints;
      this.data.prefStats = {};
      if (AppConstants.DEBUG) {
        Services.prefs.readStats(
          (key, value) => (this.data.prefStats[key] = value)
        );
      }
      paints = null;

      this._resolve();
      this._resolve = null;
    } else {
      const topicsToNames = {
        "profile-do-change": "before profile selection",
        "toplevel-window-ready": "before opening first browser window",
      };
      topicsToNames[firstPaintNotification] = "before first paint";
      this.record(topicsToNames[topic]);
    }
  },
};
