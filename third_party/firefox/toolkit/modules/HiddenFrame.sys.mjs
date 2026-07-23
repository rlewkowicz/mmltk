/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


const XUL_PAGE = Services.io.newURI("chrome://global/content/win.xhtml");

const gAllHiddenFrames = new Set();

const BACKGROUND_WIDTH = 1024;
const BACKGROUND_HEIGHT = 768;

let cleanupRegistered = false;
function ensureCleanupRegistered() {
  if (!cleanupRegistered) {
    cleanupRegistered = true;
    Services.obs.addObserver(function () {
      for (let hiddenFrame of gAllHiddenFrames) {
        hiddenFrame.destroy();
      }
    }, "xpcom-shutdown");
  }
}

export class HiddenFrame {
  #frame = null;
  #browser = null;
  #listener = null;
  #webProgress = null;
  #deferred = null;

  get() {
    if (!this.#deferred) {
      this.#deferred = Promise.withResolvers();
      this.#create();
    }

    return this.#deferred.promise;
  }

  getWindow() {
    this.get();
    return this.#browser.document.documentGlobal;
  }

  destroy() {
    if (this.#browser) {
      if (this.#listener) {
        this.#webProgress.removeProgressListener(this.#listener);
        this.#listener = null;
        this.#webProgress = null;
      }
      this.#frame = null;
      this.#deferred = null;

      gAllHiddenFrames.delete(this);
      this.#browser.close();
      this.#browser = null;
    }
  }

  #create() {
    ensureCleanupRegistered();
    let chromeFlags = Ci.nsIWebBrowserChrome.CHROME_REMOTE_WINDOW;
    if (Services.appinfo.fissionAutostart) {
      chromeFlags |= Ci.nsIWebBrowserChrome.CHROME_FISSION_WINDOW;
    }
    this.#browser = Services.appShell.createWindowlessBrowser(
      true,
      chromeFlags
    );
    gAllHiddenFrames.add(this);
    this.#webProgress = this.#browser
      .QueryInterface(Ci.nsIInterfaceRequestor)
      .getInterface(Ci.nsIWebProgress);
    this.#listener = {
      QueryInterface: ChromeUtils.generateQI([
        "nsIWebProgressListener",
        "nsISupportsWeakReference",
      ]),
    };
    this.#listener.onStateChange = (wbp, request, stateFlags) => {
      if (!request) {
        return;
      }
      if (stateFlags & Ci.nsIWebProgressListener.STATE_STOP) {
        this.#webProgress.removeProgressListener(this.#listener);
        this.#listener = null;
        this.#webProgress = null;
        this.#frame = this.#browser.document.documentGlobal;
        this.#deferred.resolve(this.#frame);
      }
    };
    this.#webProgress.addProgressListener(
      this.#listener,
      Ci.nsIWebProgress.NOTIFY_STATE_DOCUMENT
    );
    let systemPrincipal = Services.scriptSecurityManager.getSystemPrincipal();
    let browsingContext = this.#browser.browsingContext;
    browsingContext.useGlobalHistory = false;
    let loadURIOptions = {
      triggeringPrincipal: systemPrincipal,
    };
    this.#browser.loadURI(XUL_PAGE, loadURIOptions);
  }
}

export const HiddenBrowserManager = new (class HiddenBrowserManager {
  #frame = null;
  #browsers = 0;

  async #acquireBrowser(messageManagerGroup) {
    this.#browsers++;
    if (!this.#frame) {
      this.#frame = new HiddenFrame();
    }

    let frame = await this.#frame.get();
    let doc = frame.document;
    let browser = doc.createXULElement("browser");
    browser.setAttribute("remote", "true");
    browser.setAttribute("type", "content");
    browser.style.width = `${BACKGROUND_WIDTH}px`;
    browser.style.minWidth = `${BACKGROUND_WIDTH}px`;
    browser.style.height = `${BACKGROUND_HEIGHT}px`;
    browser.style.minHeight = `${BACKGROUND_HEIGHT}px`;
    browser.setAttribute("maychangeremoteness", "true");
    browser.setAttribute("nodefaultsrc", "true");
    if (messageManagerGroup) {
      browser.setAttribute("messagemanagergroup", messageManagerGroup);
    }
    doc.documentElement.appendChild(browser);

    return browser;
  }

  #releaseBrowser(browser) {
    browser.remove();

    this.#browsers--;
    if (this.#browsers == 0) {
      this.#frame.destroy();
      this.#frame = null;
    }
  }

  async withHiddenBrowser(callback, { messageManagerGroup = undefined } = {}) {
    let browser = await this.#acquireBrowser(messageManagerGroup);
    try {
      return await callback(browser);
    } finally {
      this.#releaseBrowser(browser);
    }
  }
})();
