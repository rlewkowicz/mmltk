/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "console", () => {
  return console.createInstance({
    prefix: "CaptchaDetectionChild",
    maxLogLevelPref: "captchadetection.loglevel",
  });
});

class CaptchaHandler {
  static type = "abstract";

  constructor(actor, _event, skipConstructedNotif = false) {
    this.actor = actor;
    if (!skipConstructedNotif) {
      this.notifyConstructed();
    }
  }

  notifyConstructed() {
    lazy.console.debug(`CaptchaHandler constructed: ${this.constructor.type}`);
    this.actor.sendAsyncMessage("CaptchaHandler:Constructed", {
      type: this.constructor.type,
    });
  }

  static matches(_document) {
    throw new Error("abstract method");
  }

  updateState(state) {
    this.actor.sendAsyncMessage("CaptchaState:Update", state);
  }

  onActorDestroy() {
    lazy.console.debug("CaptchaHandler destroyed");
  }

  handleEvent(event) {
    lazy.console.debug("CaptchaHandler got event:", event);
  }

  receiveMessage(message) {
    lazy.console.debug("CaptchaHandler got message:", message);
  }
}

class GoogleRecaptchaV2Handler extends CaptchaHandler {
  #enabled;
  #mutationObserver;

  static type = "g-recaptcha-v2";

  constructor(actor, event) {
    super(
      actor,
      event,
      actor.document.location.pathname.endsWith("/bframe") ||
        (false &&
          actor.document.location.pathname.endsWith(
            "g_recaptcha_v2_checkbox.html"
          ))
    );
    this.#enabled = true;
    this.#mutationObserver = new this.actor.contentWindow.MutationObserver(
      this.#mutationHandler.bind(this)
    );
    this.#mutationObserver.observe(this.actor.document, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ["style"],
    });
  }

  static matches(document) {
    if (false) {
      return (
        document
          .getElementById("captchaType")
          ?.getAttribute("data-captcha-type") === GoogleRecaptchaV2Handler.type
      );
    }

    return (
      [
        "https://www.google.com/recaptcha/api2/",
        "https://www.google.com/recaptcha/enterprise/",
      ].some(match => document.location.href.startsWith(match)) &&
      !document.location.search.includes("size=invisible")
    );
  }

  #mutationHandler(_mutations, observer) {
    if (!this.#enabled) {
      return;
    }

    const token = this.actor.document.getElementById("recaptcha-token");
    const initialized = token && token.value !== "";
    if (!initialized) {
      return;
    }

    const checkmark = this.actor.document.getElementById("recaptcha-anchor");
    if (checkmark && checkmark.ariaChecked === "true") {
      this.updateState({
        type: GoogleRecaptchaV2Handler.type,
        changes: "GotCheckmark",
      });
      this.#enabled = false;
      observer.disconnect();
      return;
    }

    const images = this.actor.document.getElementById("rc-imageselect");
    if (images) {
      this.updateState({
        type: GoogleRecaptchaV2Handler.type,
        changes: "ImagesShown",
      });
      this.#enabled = false;
      observer.disconnect();
    }
  }

  onActorDestroy() {
    super.onActorDestroy();
    this.#mutationObserver.disconnect();
  }
}

class CFTurnstileHandler extends CaptchaHandler {
  #observingShadowRoot;
  #mutationObserver;

  static type = "cf-turnstile";

  constructor(actor, event) {
    super(actor, event);
    this.#observingShadowRoot = false;
    if (this.actor.document.body?.openOrClosedShadowRoot) {
      this.#observeShadowRoot(this.actor.document.body.openOrClosedShadowRoot);
      return;
    }
    this.#mutationObserver = new this.actor.contentWindow.MutationObserver(
      this.#mutationHandler.bind(this)
    );
    this.#mutationObserver.observe(this.actor.document.documentElement, {
      attributes: true,
    });
  }

  static matchesRegex = new RegExp(
    "https://challenges.cloudflare.com/cdn-cgi/challenge-platform/.+?/turnstile/if/ov2/av0/rcv/"
  );

  static matches(document) {
    if (false) {
      return (
        document
          .getElementById("captchaType")
          ?.getAttribute("data-captcha-type") === CFTurnstileHandler.type
      );
    }

    return CFTurnstileHandler.matchesRegex.test(document.location.href);
  }

  #mutationHandler(_mutations, observer) {
    lazy.console.debug(_mutations);
    if (this.#observingShadowRoot) {
      return;
    }

    const shadowRoot = this.actor.document.body?.openOrClosedShadowRoot;
    if (!shadowRoot) {
      return;
    }
    observer.disconnect();
    lazy.console.debug("Found shadowRoot", shadowRoot);

    this.#observeShadowRoot(shadowRoot);
  }

  #observeShadowRoot(shadowRoot) {
    if (this.#observingShadowRoot) {
      return;
    }
    this.#observingShadowRoot = true;

    this.#mutationObserver = new this.actor.contentWindow.MutationObserver(
      (_mutations, observer) => {
        const fail = shadowRoot.getElementById("fail");
        const success = shadowRoot.getElementById("success");
        if (!fail || !success) {
          return;
        }

        if (fail.style.display !== "none") {
          lazy.console.debug("Captcha failed");
          this.updateState({
            type: CFTurnstileHandler.type,
            result: "Failed",
          });
          observer.disconnect();
          return;
        }

        if (success.style.display !== "none") {
          lazy.console.debug("Captcha succeeded");
          this.updateState({
            type: CFTurnstileHandler.type,
            result: "Succeeded",
          });
          observer.disconnect();
        }
      }
    ).observe(shadowRoot, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ["style"],
    });
  }

  onActorDestroy() {
    super.onActorDestroy();
    this.#mutationObserver.disconnect();
  }
}

class DatadomeHandler extends CaptchaHandler {
  static type = "datadome";

  constructor(actor, event) {
    super(actor, event);

    event.stopImmediatePropagation();

    this.actor
      .sendQuery("CaptchaDetection:Init", { type: DatadomeHandler.type })
      .then(() => {
        event.target.dispatchEvent(event);
      });
  }

  static matches(document) {
    if (false) {
      return (
        document
          .getElementById("captchaType")
          ?.getAttribute("data-captcha-type") === DatadomeHandler.type
      );
    }

    return document.location.href.startsWith(
      "https://geo.captcha-delivery.com/captcha/"
    );
  }
}

class HCaptchaHandler extends CaptchaHandler {
  #shown;
  #checked;
  #mutationObserver;

  static type = "hCaptcha";

  constructor(actor, event) {
    let params = null;
    try {
      params = new URLSearchParams(actor.document.location.hash.slice(1));
    } catch {
      super(actor, event, true);
      return;
    }

    const frameType = params.get("frame");

    super(actor, event, frameType === "challenge");

    if (frameType === "challenge") {
      this.#initChallengeHandler();
    } else if (frameType === "checkbox") {
      this.#initCheckboxHandler();
    }
  }

  static matches(document) {
    if (false) {
      return (
        document
          .getElementById("captchaType")
          ?.getAttribute("data-captcha-type") === HCaptchaHandler.type
      );
    }

    return (
      document.location.href.startsWith(
        "https://newassets.hcaptcha.com/captcha/v1/"
      ) &&
      document.location.pathname.endsWith("/static/hcaptcha.html") &&
      !document.location.hash.includes("size=invisible") &&
      !document.location.hash.includes("frame=checkbox-invisible")
    );
  }

  #initChallengeHandler() {
    this.#shown = false;
    this.#mutationObserver = new this.actor.contentWindow.MutationObserver(
      this.#challengeMutationHandler.bind(this)
    );
    this.#mutationObserver.observe(this.actor.document.body, {
      attributes: true,
      attributeFilter: ["aria-hidden"],
    });
  }

  #challengeMutationHandler(_mutations, observer) {
    if (this.#shown) {
      return;
    }

    this.#shown = this.actor.document.body.ariaHidden !== "true";
    if (!this.#shown) {
      return;
    }

    this.updateState({
      type: HCaptchaHandler.type,
      changes: "shown",
    });
    observer.disconnect();
  }

  #initCheckboxHandler() {
    this.#checked = false;
    this.#mutationObserver = new this.actor.contentWindow.MutationObserver(
      this.#checkboxMutationHandler.bind(this)
    );
    this.#mutationObserver.observe(this.actor.document, {
      subtree: true,
      attributes: true,
      attributeFilter: ["aria-checked"],
    });
  }

  #checkboxMutationHandler(_mutations, observer) {
    if (this.#checked) {
      return;
    }

    const checkbox = this.actor.document.getElementById("checkbox");
    if (checkbox?.ariaChecked === "true") {
      this.#checked = true;
      this.updateState({
        type: HCaptchaHandler.type,
        changes: "passed",
      });
      observer.disconnect();
    }
  }

  onActorDestroy() {
    super.onActorDestroy();
    this.#mutationObserver.disconnect();
  }
}

class ArkoseLabsHandler extends CaptchaHandler {
  static type = "arkoseLabs";

  constructor(actor) {
    super(actor);
    this.actor.sendQuery("CaptchaDetection:Init", {
      type: ArkoseLabsHandler.type,
    });
  }

  static matches(document) {
    if (false) {
      return (
        document
          .getElementById("captchaType")
          ?.getAttribute("data-captcha-type") === ArkoseLabsHandler.type
      );
    }

    return document.location.href.startsWith(
      "https://client-api.arkoselabs.com/fc/assets/ec-game-core/game-core/"
    );
  }
}

export class CaptchaDetectionChild extends JSWindowActorChild {
  actorCreated() {
    lazy.console.debug("actorCreated");
  }

  static #handlers = [
    GoogleRecaptchaV2Handler,
    CFTurnstileHandler,
    DatadomeHandler,
    HCaptchaHandler,
    ArkoseLabsHandler,
  ];

  #initCaptchaHandler(event) {
    for (const handler of CaptchaDetectionChild.#handlers) {
      if (handler.matches(this.document)) {
        this.handler = new handler(this, event);
        return;
      }
    }
  }

  actorDestroy() {
    lazy.console.debug("actorDestroy()");
    this.handler?.onActorDestroy();
  }

  handleEvent(event) {
    if (
      !this.handler &&
      (event.type === "DOMContentLoaded" || event.type === "pageshow")
    ) {
      this.#initCaptchaHandler(event);
      return;
    }

    if (event.type === "pagehide") {
      this.sendAsyncMessage("Page:Hide");
    }

    this.handler?.handleEvent(event);
  }

  receiveMessage(message) {
    if (this.handler) {
      this.handler.receiveMessage(message);
    }
  }
}

