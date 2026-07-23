/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "webauthnService",
  "@mozilla.org/webauthn/service;1",
  Ci.nsIWebAuthnService
);

export let WebAuthnPromptHelper = {
  _icon: "webauthn-notification-icon",
  _topic: "webauthn-prompt",

  _current: null,

  _tid: 0,

  _l10n: new Localization(
    ["branding/brand.ftl", "toolkit/webauthnDialog.ftl"],
    true
  ),

  observe(aSubject, aTopic, aData) {
    switch (aTopic) {
      case "fullscreen-nav-toolbox":
        if (aData == "hidden" && this._tid != 0) {
          aSubject.documentGlobal.FullScreen.showNavToolbox();
        }
        return;
      case "fullscreen-painted":
        if (this._tid != 0) {
          aSubject.FullScreen.exitDomFullScreen();
        }
        return;
      case this._topic:
        break;
      default:
        return;
    }

    let data = JSON.parse(aData);

    if (data.prompt.type == "cancel") {
      this.cancel(data);
      return;
    }

    let browsingContext = BrowsingContext.get(data.browsingContextId);

    if (data.prompt.type == "presence") {
      this.presence_required(browsingContext, data);
    } else if (data.prompt.type == "attestation-consent") {
      this.attestation_consent(browsingContext, data);
    } else if (data.prompt.type == "pin-required") {
      this.pin_required(browsingContext, false, data);
    } else if (data.prompt.type == "pin-invalid") {
      this.pin_required(browsingContext, true, data);
    } else if (data.prompt.type == "select-sign-result") {
      this.select_sign_result(browsingContext, data);
    } else if (data.prompt.type == "already-registered") {
      this.show_info(
        browsingContext,
        data.origin,
        data.tid,
        "alreadyRegistered",
        "webauthn-already-registered-prompt"
      );
    } else if (data.prompt.type == "select-device") {
      this.show_info(
        browsingContext,
        data.origin,
        data.tid,
        "selectDevice",
        "webauthn-select-device-prompt"
      );
    } else if (data.prompt.type == "pin-auth-blocked") {
      this.show_info(
        browsingContext,
        data.origin,
        data.tid,
        "pinAuthBlocked",
        "webauthn-pin-auth-blocked-prompt"
      );
    } else if (data.prompt.type == "uv-blocked") {
      this.show_info(
        browsingContext,
        data.origin,
        data.tid,
        "uvBlocked",
        "webauthn-uv-blocked-prompt"
      );
    } else if (data.prompt.type == "uv-invalid") {
      let retriesLeft = data.prompt.retries;
      let dialogText;
      if (retriesLeft === 0) {
        return;
      } else if (retriesLeft == null || retriesLeft < 0) {
        dialogText = this._l10n.formatValueSync(
          "webauthn-uv-invalid-short-prompt"
        );
      } else {
        dialogText = this._l10n.formatValueSync(
          "webauthn-uv-invalid-long-prompt",
          { retriesLeft }
        );
      }
      let mainAction = this.buildCancelAction(data.tid);
      this.show_formatted_msg(
        browsingContext,
        data.tid,
        "uvInvalid",
        dialogText,
        mainAction
      );
    } else if (data.prompt.type == "device-blocked") {
      this.show_info(
        browsingContext,
        data.origin,
        data.tid,
        "deviceBlocked",
        "webauthn-device-blocked-prompt"
      );
    } else if (data.prompt.type == "pin-not-set") {
      this.show_info(
        browsingContext,
        data.origin,
        data.tid,
        "pinNotSet",
        "webauthn-pin-not-set-prompt"
      );
    }
  },

  prompt_for_password(
    browsingContext,
    origin,
    wasInvalid,
    retriesLeft,
    aPassword
  ) {
    this.reset();
    let dialogText;
    if (!wasInvalid) {
      dialogText = this._l10n.formatValueSync("webauthn-pin-required-prompt");
    } else if (retriesLeft == null || retriesLeft < 0 || retriesLeft > 3) {
      dialogText = this._l10n.formatValueSync(
        "webauthn-pin-invalid-short-prompt"
      );
    } else {
      dialogText = this._l10n.formatValueSync(
        "webauthn-pin-invalid-long-prompt",
        { retriesLeft }
      );
    }

    let res = Services.prompt.promptPasswordBC(
      browsingContext,
      Services.prompt.MODAL_TYPE_TAB,
      origin,
      dialogText,
      aPassword
    );
    return res;
  },

  select_sign_result(browsingContext, { origin, tid, prompt: { entities } }) {
    let unknownAccount = this._l10n.formatValueSync(
      "webauthn-select-sign-result-unknown-account"
    );
    let secondaryActions = [];
    for (let i = 0; i < entities.length; i++) {
      let label = entities[i].name ?? unknownAccount;
      secondaryActions.push({
        label,
        accessKey: i.toString(),
        callback() {
          lazy.webauthnService.selectionCallback(tid, i);
        },
      });
    }
    let mainAction = this.buildCancelAction(tid);
    let options = { escAction: "buttoncommand" };
    this.show(
      browsingContext,
      tid,
      "select-sign-result",
      "webauthn-select-sign-result-prompt",
      origin,
      mainAction,
      secondaryActions,
      options
    );
  },

  pin_required(
    browsingContext,
    wasInvalid,
    { origin, tid, prompt: { retries } }
  ) {
    let aPassword = Object.create(null); 
    let res = this.prompt_for_password(
      browsingContext,
      origin,
      wasInvalid,
      retries,
      aPassword
    );
    if (res) {
      lazy.webauthnService.pinCallback(tid, aPassword.value);
    } else {
      lazy.webauthnService.cancel(tid);
    }
  },

  presence_required(browsingContext, { origin, tid }) {
    let mainAction = this.buildCancelAction(tid);
    let options = { escAction: "buttoncommand" };
    let secondaryActions = [];
    let message = "webauthn-user-presence-prompt";
    this.show(
      browsingContext,
      tid,
      "presence",
      message,
      origin,
      mainAction,
      secondaryActions,
      options
    );
  },

  attestation_consent(browsingContext, { origin, tid }) {
    let [allowMsg, blockMsg] = this._l10n.formatMessagesSync([
      { id: "webauthn-allow" },
      { id: "webauthn-block" },
    ]);
    let mainAction = {
      label: allowMsg.value,
      accessKey: allowMsg.attributes.find(a => a.name == "accesskey").value,
      callback(_state) {
        lazy.webauthnService.setHasAttestationConsent(tid, true);
      },
    };
    let secondaryActions = [
      {
        label: blockMsg.value,
        accessKey: blockMsg.attributes.find(a => a.name == "accesskey").value,
        callback(_state) {
          lazy.webauthnService.setHasAttestationConsent(tid, false);
        },
      },
    ];

    let learnMoreURL =
      Services.urlFormatter.formatURLPref("app.support.baseURL") +
      "webauthn-direct-attestation";

    let options = {
      learnMoreURL,
      hintText: this._l10n.formatValueSync(
        "webauthn-register-direct-prompt-hint"
      ),
    };
    this.show(
      browsingContext,
      tid,
      "register-direct",
      "webauthn-register-direct-prompt",
      origin,
      mainAction,
      secondaryActions,
      options
    );
  },

  show_info(browsingContext, origin, tid, id, stringId) {
    let mainAction = this.buildCancelAction(tid);
    this.show(browsingContext, tid, id, stringId, origin, mainAction);
  },

  show(
    browsingContext,
    tid,
    id,
    stringId,
    origin,
    mainAction,
    secondaryActions = [],
    options = {}
  ) {
    let message = this._l10n.formatValueSync(stringId, { hostname: "<>" });

    try {
      origin = Services.io.newURI(origin).asciiHost;
    } catch (e) {
    }
    options.name = origin;
    this.show_formatted_msg(
      browsingContext,
      tid,
      id,
      message,
      mainAction,
      secondaryActions,
      options
    );
  },

  show_formatted_msg(
    browsingContext,
    tid,
    id,
    message,
    mainAction,
    secondaryActions = [],
    options = {}
  ) {
    this.reset();
    this._tid = tid;

    Services.obs.addObserver(this, "fullscreen-painted");

    Services.obs.addObserver(this, "fullscreen-nav-toolbox");

    let chromeWin = browsingContext.topChromeWindow;

    chromeWin.FullScreen.exitDomFullScreen();

    if (chromeWin.fullScreen) {
      chromeWin.FullScreen.showNavToolbox();
    }

    options.hideClose = true;
    options.persistent = true;
    options.eventCallback = event => {
      if (event == "removed") {
        Services.obs.removeObserver(this, "fullscreen-painted");
        Services.obs.removeObserver(this, "fullscreen-nav-toolbox");
        this._current = null;
        this._tid = 0;
      }
    };

    this._current = chromeWin.PopupNotifications.show(
      browsingContext.top.embedderElement,
      `webauthn-prompt-${id}`,
      message,
      this._icon,
      mainAction,
      secondaryActions,
      options
    );
  },

  cancel({ tid }) {
    if (this._tid == tid) {
      this.reset();
    }
  },

  reset() {
    if (this._current) {
      this._current.remove();
    }
  },

  buildCancelAction(tid) {
    let [cancelMsg] = this._l10n.formatMessagesSync([
      { id: "webauthn-cancel" },
    ]);
    return {
      label: cancelMsg.value,
      accessKey: cancelMsg.attributes.find(a => a.name == "accesskey").value,
      callback() {
        lazy.webauthnService.cancel(tid);
      },
    };
  },
};
