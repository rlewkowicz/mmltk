/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "console", () => {
  return console.createInstance({
    prefix: "CaptchaDetectionParent",
    maxLogLevelPref: "captchadetection.loglevel",
  });
});

ChromeUtils.defineESModuleGetters(lazy, {
  CaptchaDetectionPingUtils:
    "resource://gre/modules/CaptchaDetectionPingUtils.sys.mjs",
  CaptchaResponseObserver:
    "resource://gre/modules/CaptchaResponseObserver.sys.mjs",
});

class DocCaptchaState {
  #state;

  constructor() {
    this.#state = new Map();
  }

  get(topId) {
    return this.#state.get(topId);
  }

  static #defaultValue() {
    return new Map();
  }

  update(topId, updateFunction) {
    if (!this.#state.has(topId)) {
      this.#state.set(topId, DocCaptchaState.#defaultValue());
    }
    updateFunction(this.#state.get(topId));
  }

  clear(topId) {
    this.#state.delete(topId);
  }
}

const docState = new DocCaptchaState();

class CaptchaDetectionParent extends JSWindowActorParent {
  #responseObserver;

  actorCreated() {
    lazy.console.debug("actorCreated");
  }

  actorDestroy() {
    lazy.console.debug("actorDestroy()");

    this.#onPageHidden();
  }

  #updateGRecaptchaV2State({ changes, type }) {
    lazy.console.debug("updateGRecaptchaV2State", changes);

    const topId = this.#topInnerWindowId;
    const isPBM = this.browsingContext.usePrivateBrowsing;

    if (changes === "ImagesShown") {
      docState.update(topId, state => {
        state.set(type + changes, true);
      });

      const shownMetric = "googleRecaptchaV2Ps" + (isPBM ? "Pbm" : "");
    } else if (changes === "GotCheckmark") {
      const autoCompleted = !docState.get(topId)?.has(type + "ImagesShown");
      const resultMetric =
        "googleRecaptchaV2" +
        (autoCompleted ? "Ac" : "Pc") +
        (isPBM ? "Pbm" : "");
      lazy.console.debug("Incremented metric", resultMetric);
      docState.clear(topId);
      this.#onMetricSet();
    }
  }

  #recordCFTurnstileResult({ result }) {
    lazy.console.debug("recordCFTurnstileResult", result);

    const isPBM = this.browsingContext.usePrivateBrowsing;
    const resultMetric =
      "cloudflareTurnstile" +
      (result === "Succeeded" ? "Cc" : "Cf") +
      (isPBM ? "Pbm" : "");
    lazy.console.debug("Incremented metric", resultMetric);
    this.#onMetricSet();
  }

  async #datadomeInit() {
    const parent = this.browsingContext.parentWindowContext;
    if (!parent) {
      lazy.console.error("Datadome captcha loaded in a top-level window?");
      return;
    }

    let actor = null;
    try {
      actor = parent.getActor("CaptchaDetectionCommunication");
      if (!actor) {
        lazy.console.error("CaptchaDetection actor not found in parent window");
        return;
      }
    } catch (e) {
      lazy.console.error("Error getting actor", e);
      return;
    }

    await actor.sendQuery("Datadome:AddMessageListener");
  }

  #recordDatadomeEvent({ event, ...payload }) {
    lazy.console.debug("recordDatadomeEvent", { event, payload });

    const suffix = this.browsingContext.usePrivateBrowsing ? "Pbm" : "";
    let metricName = "datadome";
    if (event === "load") {
      if (payload.captchaShown) {
        metricName += "Ps";
      } else if (payload.blocked) {
        metricName += "Bl";
      }
    } else if (event === "passed") {
      metricName += "Pc";
    } else {
      lazy.console.error("Unknown Datadome event", event);
      return;
    }

    metricName += suffix;
    lazy.console.debug("Incremented metric", metricName);

    this.#onMetricSet(0);
  }

  #recordHCaptchaState({ changes, type }) {
    lazy.console.debug("recordHCaptchaEvent", changes);

    const topId = this.#topInnerWindowId;
    const isPBM = this.browsingContext.usePrivateBrowsing;

    if (changes === "shown") {
      docState.update(topId, state => {
        state.set(type + changes, true);
      });

      const shownMetric = "hcaptchaPs" + (isPBM ? "Pbm" : "");
      lazy.console.debug("Incremented metric", shownMetric);
    } else if (changes === "passed") {
      const autoCompleted = !docState.get(topId)?.has(type + "shown");
      const resultMetric =
        "hcaptcha" + (autoCompleted ? "Ac" : "Pc") + (isPBM ? "Pbm" : "");
      lazy.console.debug("Incremented metric", resultMetric);
      docState.clear(topId);
      this.#onMetricSet();
    }
  }

  #recordArkoseLabsEvent({ event, solved, solutionsSubmitted }) {
    lazy.console.debug("recordArkoseLabsEvent", {
      event,
      solved,
      solutionsSubmitted,
    });

    const isPBM = this.browsingContext.usePrivateBrowsing;

    const suffix = isPBM ? "Pbm" : "";
    const resultMetric = "arkoselabs" + (solved ? "Pc" : "Pf") + suffix;
    lazy.console.debug("Incremented metric", resultMetric);

    const metricName = "arkoselabsSolutionsRequired" + suffix;
    lazy.console.debug("Sampled", metricName, "with", solutionsSubmitted);

    this.#onMetricSet();
  }

  async #arkoseLabsInit() {
    let solutionsSubmitted = 0;
    this.#responseObserver = new lazy.CaptchaResponseObserver(
      channel =>
        channel.loadInfo?.browsingContextID === this.browsingContext.id &&
        channel.URI &&
        (false
          ? channel.URI.filePath.endsWith("arkose_labs_api.sjs")
          : channel.URI.spec === "https://client-api.arkoselabs.com/fc/ca/"),
      (_channel, statusCode, responseBody) => {
        if (statusCode !== Cr.NS_OK) {
          return;
        }

        let body;
        try {
          body = JSON.parse(responseBody);
          if (!body) {
            lazy.console.debug(
              "ResponseObserver:ResponseBody",
              "Failed to parse JSON"
            );
            return;
          }
        } catch (e) {
          lazy.console.debug(
            "ResponseObserver:ResponseBody",
            "Failed to parse JSON",
            e,
            responseBody
          );
          return;
        }

        if (["response", "solved"].some(key => !body.hasOwnProperty(key))) {
          lazy.console.debug(
            "ResponseObserver:ResponseBody",
            "Missing keys",
            body
          );
          return;
        }

        solutionsSubmitted++;
        if (typeof body.solved !== "boolean") {
          return;
        }

        this.#recordArkoseLabsEvent({
          event: "completed",
          solved: body.solved,
          solutionsSubmitted,
        });

        solutionsSubmitted = 0;
      }
    );
    this.#responseObserver.register();
  }

  get #topInnerWindowId() {
    return this.browsingContext.topWindowContext.innerWindowId;
  }

  #onPageHidden() {
    docState.clear(this.#topInnerWindowId);

    if (this.#responseObserver) {
      this.#responseObserver.unregister();
    }
  }

  async #onMetricSet(parentDepth = 1) {
    lazy.CaptchaDetectionPingUtils.maybeSubmitPing();
    if (false) {
      await this.#notifyTestMetricIsSet(parentDepth);
    }
  }

  async #notifyTestMetricIsSet(parentDepth = 1) {
    if (!false) {
      throw new Error("This method should only be called in automation");
    }

    let parent = this.browsingContext.currentWindowContext;
    for (let i = 0; i < parentDepth; i++) {
      parent = parent.parentWindowContext;
      if (!parent) {
        lazy.console.error("No parent window context");
        return;
      }
    }

    let actor = null;
    try {
      actor = parent.getActor("CaptchaDetectionCommunication");
      if (!actor) {
        lazy.console.error("CaptchaDetection actor not found in parent window");
        return;
      }
    } catch (e) {
      lazy.console.error("Error getting actor", e);
      return;
    }

    await actor.sendQuery("Testing:MetricIsSet");
  }

  recordCaptchaHandlerConstructed({ type }) {
    lazy.console.debug("recordCaptchaHandlerConstructed", type);

    let metric = "";
    switch (type) {
      case "g-recaptcha-v2":
        metric = "googleRecaptchaV2Oc";
        break;
      case "cf-turnstile":
        metric = "cloudflareTurnstileOc";
        break;
      case "datadome":
        metric = "datadomeOc";
        break;
      case "hCaptcha":
        metric = "hcaptchaOc";
        break;
      case "arkoseLabs":
        metric = "arkoselabsOc";
        break;
    }
    metric += this.browsingContext.usePrivateBrowsing ? "Pbm" : "";
    lazy.console.debug("Incremented metric", metric);
  }

  async receiveMessage(message) {
    lazy.console.debug("receiveMessage", message);

    switch (message.name) {
      case "CaptchaState:Update":
        switch (message.data.type) {
          case "g-recaptcha-v2":
            this.#updateGRecaptchaV2State(message.data);
            break;
          case "cf-turnstile":
            this.#recordCFTurnstileResult(message.data);
            break;
          case "datadome":
            this.#recordDatadomeEvent(message.data);
            break;
          case "hCaptcha":
            this.#recordHCaptchaState(message.data);
            break;
        }
        break;
      case "CaptchaHandler:Constructed":
        this.recordCaptchaHandlerConstructed(message.data);
        break;
      case "Page:Hide":
        this.#onPageHidden();
        break;
      case "CaptchaDetection:Init":
        switch (message.data.type) {
          case "datadome":
            return this.#datadomeInit();
          case "arkoseLabs":
            return this.#arkoseLabsInit();
        }
        break;
      default:
        lazy.console.error("Unknown message", message);
    }
    return null;
  }
}

export {
  CaptchaDetectionParent,
  CaptchaDetectionParent as CaptchaDetectionCommunicationParent,
};


