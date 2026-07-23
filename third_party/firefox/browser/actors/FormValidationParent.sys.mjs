/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
});

class PopupShownObserver {
  _weakContext = null;

  constructor(browsingContext) {
    this._weakContext = Cu.getWeakReference(browsingContext);
  }

  observe(subject, topic) {
    let ctxt = this._weakContext.get();
    let actor = ctxt.currentWindowGlobal?.getExistingActor("FormValidation");
    if (!actor) {
      Services.obs.removeObserver(this, "popup-shown");
      return;
    }
    if (topic == "popup-shown" && subject != actor._panel) {
      actor._hidePopup();
    }
  }

  QueryInterface = ChromeUtils.generateQI([
    Ci.nsIObserver,
    Ci.nsISupportsWeakReference,
  ]);
}

export class FormValidationParent extends JSWindowActorParent {
  constructor() {
    super();

    this._panel = null;
    this._obs = null;
  }

  static hasOpenPopups(ownPanel = null) {
    for (let win of lazy.BrowserWindowTracker.orderedWindows) {
      let popups = win.document.querySelectorAll("panel,menupopup");
      for (let popup of popups) {
        if (popup == ownPanel) {
          continue; 
        }
        let { state } = popup;
        if (state == "open" || state == "showing") {
          return true;
        }
      }
    }
    return false;
  }


  uninit() {
    this._panel = null;
    this._obs = null;
  }

  hidePopup() {
    this._hidePopup();
  }


  receiveMessage(aMessage) {
    switch (aMessage.name) {
      case "FormValidation:ShowPopup": {
        let browser = this.browsingContext.top.embedderElement;
        let window = browser.documentGlobal;
        let data = aMessage.data;
        let tabBrowser = window.gBrowser;

        if (tabBrowser && browser != tabBrowser.selectedBrowser) {
          return;
        }

        // popup. We have to fall through for our own popup to make sure the
        if (FormValidationParent.hasOpenPopups(this._panel)) {
          return;
        }

        this._showPopup(browser, data);
        break;
      }
      case "FormValidation:HidePopup":
        this._hidePopup();
        break;
    }
  }

  handleEvent(aEvent) {
    switch (aEvent.type) {
      case "FullZoomChange":
      case "TextZoomChange":
      case "scroll":
        this._hidePopup();
        break;
      case "popuphidden":
        this._onPopupHidden(aEvent);
        break;
    }
  }


  _onPopupHidden(aEvent) {
    aEvent.originalTarget.removeEventListener("popuphidden", this, true);
    Services.obs.removeObserver(this._obs, "popup-shown");
    let tabBrowser = aEvent.originalTarget.documentGlobal.gBrowser;
    tabBrowser.selectedBrowser.removeEventListener("scroll", this, true);
    tabBrowser.selectedBrowser.removeEventListener("FullZoomChange", this);
    tabBrowser.selectedBrowser.removeEventListener("TextZoomChange", this);

    this._obs = null;
    this._panel = null;
  }

  _showPopup(aBrowser, aPanelData) {
    let previouslyShown = !!this._panel;
    this._panel = this._getAndMaybeCreatePanel();
    this._panel.firstChild.textContent = aPanelData.message;

    if (previouslyShown) {
      return;
    }
    this._panel.addEventListener("popuphidden", this, true);
    this._obs = new PopupShownObserver(this.browsingContext);
    Services.obs.addObserver(this._obs, "popup-shown", true);

    aBrowser.addEventListener("scroll", this, true);
    aBrowser.addEventListener("FullZoomChange", this);
    aBrowser.addEventListener("TextZoomChange", this);

    aBrowser.constrainPopup(this._panel);

    let rect = aPanelData.screenRect;
    this._panel.openPopupAtScreenRect(
      aPanelData.position,
      rect.left,
      rect.top,
      rect.width,
      rect.height,
      false,
      false
    );
  }

  _hidePopup() {
    this._panel?.hidePopup();
  }

  _getAndMaybeCreatePanel() {
    if (!this._panel) {
      let browser = this.browsingContext.top.embedderElement;
      let window = browser.documentGlobal;
      let template = window.document.getElementById("invalidFormTemplate");
      if (template) {
        template.replaceWith(template.content);
      }
      this._panel = window.document.getElementById("invalid-form-popup");
    }

    return this._panel;
  }
}
