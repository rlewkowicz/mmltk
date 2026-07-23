/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ContentDOMReference: "resource://gre/modules/ContentDOMReference.sys.mjs",
  DeferredTask: "resource://gre/modules/DeferredTask.sys.mjs",
  KEYBOARD_CONTROLS: "resource://gre/modules/PictureInPictureControls.sys.mjs",
  NimbusFeatures: "resource://nimbus/ExperimentAPI.sys.mjs",
  Rect: "resource://gre/modules/Geometry.sys.mjs",
  TOGGLE_POLICIES: "resource://gre/modules/PictureInPictureControls.sys.mjs",
  TOGGLE_POLICY_STRINGS:
    "resource://gre/modules/PictureInPictureControls.sys.mjs",
});

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { WebVTT } from "resource://gre/modules/vtt.sys.mjs";
import { setTimeout, clearTimeout } from "resource://gre/modules/Timer.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "DISPLAY_TEXT_TRACKS_PREF",
  "media.videocontrols.picture-in-picture.display-text-tracks.enabled",
  false
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "IMPROVED_CONTROLS_ENABLED_PREF",
  "media.videocontrols.picture-in-picture.improved-video-controls.enabled",
  false
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "MIN_VIDEO_LENGTH",
  "media.videocontrols.picture-in-picture.video-toggle.min-video-secs",
  45
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "PIP_TOGGLE_ALWAYS_SHOW",
  "media.videocontrols.picture-in-picture.video-toggle.always-show",
  false
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "PIP_URLBAR_BUTTON",
  "media.videocontrols.picture-in-picture.urlbar-button.enabled",
  false
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "PIP_AUTO_CLOSE",
  "media.videocontrols.picture-in-picture.auto-close.enabled",
  true
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "EMPTIED_TIMEOUT_MS",
  "media.videocontrols.picture-in-picture.auto-close.timeoutMs",
  1000
);

const PIP_ENABLED_PREF = "media.videocontrols.picture-in-picture.enabled";
const TOGGLE_ENABLED_PREF =
  "media.videocontrols.picture-in-picture.video-toggle.enabled";
const TOGGLE_FIRST_SEEN_PREF =
  "media.videocontrols.picture-in-picture.video-toggle.first-seen-secs";
const TOGGLE_FIRST_TIME_DURATION_DAYS = 28;
const TOGGLE_HAS_USED_PREF =
  "media.videocontrols.picture-in-picture.video-toggle.has-used";
const TOGGLE_TESTING_PREF =
  "media.videocontrols.picture-in-picture.video-toggle.testing";
const TOGGLE_VISIBILITY_THRESHOLD_PREF =
  "media.videocontrols.picture-in-picture.video-toggle.visibility-threshold";
const TEXT_TRACK_FONT_SIZE =
  "media.videocontrols.picture-in-picture.display-text-tracks.size";

const MOUSEMOVE_PROCESSING_DELAY_MS = 50;
const TOGGLE_HIDING_TIMEOUT_MS = 3000;
const SEEK_TIME_SECS = 5;

var gPlayerContents = new WeakSet();

let gOriginatingVideoMap = new WeakMap();

let gVideoToPipWindow = new WeakMap();

var gWeakIntersectingVideosForTesting = new WeakSet();

ChromeUtils.defineLazyGetter(lazy, "gSiteOverrides", () => {
  return PictureInPictureToggleChild.getSiteOverrides();
});

ChromeUtils.defineLazyGetter(lazy, "logConsole", () => {
  return console.createInstance({
    prefix: "PictureInPictureChild",
    maxLogLevel: Services.prefs.getBoolPref(
      "media.videocontrols.picture-in-picture.log",
      false
    )
      ? "Debug"
      : "Error",
  });
});

function applyWrapper(pipChild, originatingVideo) {
  let override = getSiteOverrideForDocument(
    originatingVideo.ownerDocument.documentURI
  );
  let wrapperPath = override?.videoWrapperScriptPath;
  return new PictureInPictureChildVideoWrapper(
    wrapperPath,
    originatingVideo,
    pipChild
  );
}

function getSiteOverrideForDocument(documentURI) {
  let overrides = lazy.gSiteOverrides.find(([matcher]) =>
    matcher.matches(documentURI)
  );
  return overrides?.[1] ?? null;
}

export class PictureInPictureLauncherChild extends JSWindowActorChild {
  handleEvent(event) {
    switch (event.type) {
      case "MozTogglePictureInPicture": {
        if (event.isTrusted) {
          this.togglePictureInPicture({
            video: event.target,
            reason: event.detail?.reason,
            eventExtraKeys: event.detail?.eventExtraKeys,
          });
        }
        break;
      }
    }
  }

  receiveMessage(message) {
    switch (message.name) {
      case "PictureInPicture:KeyToggle": {
        this.keyToggle();
        break;
      }
      case "PictureInPicture:AutoToggle": {
        this.autoToggle();
        break;
      }
    }
  }

  async togglePictureInPicture(pipObject, autoFocus = true) {
    let {
      video,
      reason,
      pictureInPictureWindow,
      eventExtraKeys = {},
    } = pipObject;
    if (video.isCloningElementVisually) {
      const stopPipEvent = new this.contentWindow.CustomEvent(
        "MozStopPictureInPicture",
        {
          bubbles: true,
          detail: { reason },
        }
      );
      this.contentWindow.windowUtils.dispatchEventToChromeOnly(
        video,
        stopPipEvent
      );
      return;
    }

    if (!PictureInPictureChild.videoWrapper) {
      PictureInPictureChild.videoWrapper = applyWrapper(
        PictureInPictureChild,
        video
      );
    }

    let timestamp = undefined;
    let scrubberPosition = undefined;

    if (lazy.IMPROVED_CONTROLS_ENABLED_PREF) {
      timestamp = PictureInPictureChild.videoWrapper.formatTimestamp(
        PictureInPictureChild.videoWrapper.getCurrentTime(video),
        PictureInPictureChild.videoWrapper.getDuration(video)
      );

      scrubberPosition =
        timestamp === undefined
          ? undefined
          : PictureInPictureChild.videoWrapper.getCurrentTime(video) /
            PictureInPictureChild.videoWrapper.getDuration(video);
    }

    const videoRef = lazy.ContentDOMReference.get(video);
    if (pictureInPictureWindow) {
      gVideoToPipWindow.set(video, pictureInPictureWindow);
    }

    const res = this.sendQuery("PictureInPicture:Request", {
      isMuted: PictureInPictureChild.videoIsMuted(video),
      playing: PictureInPictureChild.videoIsPlaying(video),
      videoHeight: video.videoHeight,
      videoWidth: video.videoWidth,
      videoRef,
      isPipApiRequest: !!pictureInPictureWindow,
      ccEnabled: lazy.DISPLAY_TEXT_TRACKS_PREF,
      webVTTSubtitles: !!video.textTracks?.length,
      scrubberPosition,
      timestamp,
      volume: PictureInPictureChild.videoWrapper.getVolume(video),
      autoFocus,
    });

    if (reason) {
    }
    await res;
  }

  keyToggle() {
    let doc = this.document;
    if (doc) {
      let video = this.findVideoToPiP(doc);
      if (video) {
        this.togglePictureInPicture({ video, reason: "Shortcut" });
      }
    }
  }

  findVideoToPiP(doc) {
    let video = doc.activeElement;
    if (!HTMLVideoElement.isInstance(video)) {
      let listOfVideos = [...doc.querySelectorAll("video")].filter(
        v => !isNaN(v.duration)
      );
      video =
        listOfVideos.filter(v => !v.paused)[0] ||
        listOfVideos.sort((a, b) => b.duration - a.duration)[0];
    }
    return video;
  }

  autoToggle() {
    let doc = this.document;
    if (doc) {
      let video = this.findVideoToPiP(doc);
      if (
        video &&
        PictureInPictureChild.videoIsPlaying(video) &&
        PictureInPictureChild.videoIsPiPEligible(video)
      ) {
        this.togglePictureInPicture({ video, reason: "AutoPip" }, false);
      }
    }
  }
}

export class PictureInPictureToggleChild extends JSWindowActorChild {
  constructor() {
    super();
    this.weakDocStates = new WeakMap();
    this.toggleEnabled =
      Services.prefs.getBoolPref(TOGGLE_ENABLED_PREF) &&
      Services.prefs.getBoolPref(PIP_ENABLED_PREF);
    this.toggleTesting = Services.prefs.getBoolPref(TOGGLE_TESTING_PREF, false);

    this.observerFunction = (subject, topic, data) => {
      this.observe(subject, topic, data);
    };
    Services.prefs.addObserver(TOGGLE_ENABLED_PREF, this.observerFunction);
    Services.prefs.addObserver(PIP_ENABLED_PREF, this.observerFunction);
    Services.prefs.addObserver(TOGGLE_FIRST_SEEN_PREF, this.observerFunction);
    Services.cpmm.sharedData.addEventListener("change", this);

    this.urlbarToggleEligiblePipVideos = new WeakSet();
    this.trackingVideos = new WeakSet();
  }

  receiveMessage(message) {
    switch (message.name) {
      case "PictureInPicture:UrlbarToggle": {
        this.urlbarToggle(message.data);
        break;
      }
    }
    return null;
  }

  didDestroy() {
    this.stopTrackingMouseOverVideos();
    Services.prefs.removeObserver(TOGGLE_ENABLED_PREF, this.observerFunction);
    Services.prefs.removeObserver(PIP_ENABLED_PREF, this.observerFunction);
    Services.prefs.removeObserver(
      TOGGLE_FIRST_SEEN_PREF,
      this.observerFunction
    );
    Services.cpmm.sharedData.removeEventListener("change", this);

    let state = this.docState;
    if (state?.intersectionObserver) {
      state.intersectionObserver.disconnect();
    }

    this.videoWrapper?.destroy();
    this.videoWrapper = null;

    for (let video of ChromeUtils.nondeterministicGetWeakSetKeys(
      this.urlbarToggleEligiblePipVideos
    )) {
      video.removeEventListener("emptied", this);
      video.removeEventListener("loadedmetadata", this);
      video.removeEventListener("durationchange", this);
    }

    for (let video of ChromeUtils.nondeterministicGetWeakSetKeys(
      this.trackingVideos
    )) {
      video.removeEventListener("emptied", this);
      video.removeEventListener("loadedmetadata", this);
      video.removeEventListener("durationchange", this);
    }

    this.isDestroyed = true;
  }

  observe(subject, topic, data) {
    if (topic != "nsPref:changed") {
      return;
    }

    switch (data) {
      case TOGGLE_FIRST_SEEN_PREF: {
        const firstSeenSeconds = Services.prefs.getIntPref(
          TOGGLE_FIRST_SEEN_PREF
        );
        if (!firstSeenSeconds || firstSeenSeconds < 0) {
          return;
        }
        this.changeToIconIfDurationEnd(firstSeenSeconds);
        break;
      }
    }
  }

  get docState() {
    if (this.isDestroyed || !this.document) {
      return false;
    }

    let state = this.weakDocStates.get(this.document);

    let visibilityThresholdPref = Services.prefs.getFloatPref(
      TOGGLE_VISIBILITY_THRESHOLD_PREF,
      "1.0"
    );

    if (!state) {
      state = {
        intersectionObserver: null,
        weakVisibleVideos: new WeakSet(),
        visibleVideosCount: 0,
        mousemoveDeferredTask: null,
        weakOverVideo: null,
        isClickingToggle: false,
        clickedElement: null,
        hideToggleDeferredTask: null,
        isTrackingVideos: false,
        togglePolicy: lazy.TOGGLE_POLICIES.DEFAULT,
        toggleVisibilityThreshold: visibilityThresholdPref,
        checkedPolicyDocumentURI: null,
        isUnloaded: false,
      };
      this.weakDocStates.set(this.document, state);
    }

    return state;
  }

  getWeakOverVideo() {
    let { weakOverVideo } = this.docState;
    if (weakOverVideo) {
      try {
        return weakOverVideo.get();
      } catch (e) {
        return null;
      }
    }
    return null;
  }

  handleEvent(event) {
    if (!event.isTrusted) {
      return;
    }

    if (gPlayerContents.has(this.contentWindow)) {
      return;
    }

    switch (event.type) {
      case "touchstart": {
        if (this.docState.isClickingToggle) {
          event.stopImmediatePropagation();
          event.preventDefault();
        }
        break;
      }
      case "change": {
        const { changedKeys } = event;
        if (changedKeys.includes("PictureInPicture:SiteOverrides")) {
          try {
            lazy.gSiteOverrides =
              PictureInPictureToggleChild.getSiteOverrides();
          } catch (e) {
            if (!(e instanceof TypeError)) {
              throw e;
            }
          }
        }
        break;
      }
      case "UAWidgetSetupOrChange": {
        if (
          this.contentWindow.HTMLVideoElement.isInstance(event.target) &&
          event.target.ownerDocument == this.document
        ) {
          this.registerVideo(event.target);
        }
        break;
      }
      case "contextmenu": {
        if (this.toggleEnabled) {
          this.checkContextMenu(event);
        }
        break;
      }
      case "mouseout": {
        this.onMouseOut(event);
        break;
      }
      case "click":
        if (event.detail == 0) {
          let shadowRoot = event.originalTarget.containingShadowRoot;
          let toggle = this.getToggleElement(shadowRoot);
          if (event.originalTarget == toggle) {
            this.startPictureInPicture(event, shadowRoot.host, toggle);
            return;
          }
        }
      // fall through
      case "mousedown":
      case "pointerup":
      case "mouseup": {
        this.onMouseButtonEvent(event);
        break;
      }
      case "pointerdown": {
        this.onPointerDown(event);
        break;
      }
      case "mousemove": {
        this.onMouseMove(event);
        break;
      }
      case "pageshow": {
        this.onPageShow(event);
        break;
      }
      case "pagehide": {
        this.onPageHide(event);
        break;
      }
      case "visibilitychange": {
        this.onVisibilityChange(event);
        break;
      }
      case "durationchange":
      case "emptied":
      case "loadedmetadata": {
        this.updateUrlbarPipVideoEligibility(event.target);
        break;
      }
    }
  }

  registerVideo(video) {
    let state = this.docState;
    if (!state.intersectionObserver) {
      let fn = this.onIntersection.bind(this);
      state.intersectionObserver = new this.contentWindow.IntersectionObserver(
        fn,
        {
          threshold: [0.0, 0.5],
        }
      );
    }

    state.intersectionObserver.observe(video);

    if (!lazy.PIP_URLBAR_BUTTON) {
      return;
    }

    video.addEventListener("emptied", this);
    video.addEventListener("loadedmetadata", this);
    video.addEventListener("durationchange", this);

    this.trackingVideos.add(video);

    this.updateUrlbarPipVideoEligibility(video);
  }

  updateUrlbarPipVideoEligibility(video) {
    let override = getSiteOverrideForDocument(this.document.documentURI);
    if (!this.videoWrapper && override?.hasUrlbarEligibilityOverride) {
      this.videoWrapper = applyWrapper(this, video);
    }
    let isWrapperEligible =
      this.videoWrapper?.isUrlbarToggleEligible(video) ?? true;
    let isEligible =
      isWrapperEligible && PictureInPictureChild.videoIsPiPEligible(video);
    if (isEligible) {
      if (!this.urlbarToggleEligiblePipVideos.has(video)) {
        this.urlbarToggleEligiblePipVideos.add(video);

        let mutationObserver = new this.contentWindow.MutationObserver(
          mutationList => {
            this.handleEligiblePipVideoMutation(mutationList);
          }
        );
        mutationObserver.observe(video.parentElement, { childList: true });
      }
    } else if (this.urlbarToggleEligiblePipVideos.has(video)) {
      this.urlbarToggleEligiblePipVideos.delete(video);
    }

    let videos = ChromeUtils.nondeterministicGetWeakSetKeys(
      this.urlbarToggleEligiblePipVideos
    );

    this.sendAsyncMessage("PictureInPicture:UpdateEligiblePipVideoCount", {
      pipCount: videos.length,
      pipDisabledCount: videos.reduce(
        (accumulator, currentVal) =>
          accumulator + (currentVal.disablePictureInPicture ? 1 : 0),
        0
      ),
    });
  }

  handleEligiblePipVideoMutation(mutationList) {
    for (let mutationRecord of mutationList) {
      let video = mutationRecord.removedNodes[0];
      this.urlbarToggleEligiblePipVideos.delete(video);
    }

    let videos = ChromeUtils.nondeterministicGetWeakSetKeys(
      this.urlbarToggleEligiblePipVideos
    );

    this.sendAsyncMessage("PictureInPicture:UpdateEligiblePipVideoCount", {
      pipCount: videos.length,
      pipDisabledCount: videos.reduce(
        (accumulator, currentVal) =>
          accumulator + (currentVal.disablePictureInPicture ? 1 : 0),
        0
      ),
    });
  }

  urlbarToggle(eventExtraKeys) {
    let video = ChromeUtils.nondeterministicGetWeakSetKeys(
      this.urlbarToggleEligiblePipVideos
    )[0];
    if (video) {
      let pipEvent = new this.contentWindow.CustomEvent(
        "MozTogglePictureInPicture",
        {
          bubbles: true,
          detail: { reason: "UrlBar", eventExtraKeys },
        }
      );
      this.contentWindow.windowUtils.dispatchEventToChromeOnly(video, pipEvent);
    }
  }

  changeToIconIfDurationEnd(firstSeenStartSeconds) {
    const { displayDuration } =
      lazy.NimbusFeatures.pictureinpicture.getAllVariables({
        defaultValues: {
          displayDuration: TOGGLE_FIRST_TIME_DURATION_DAYS,
        },
      });
    if (!displayDuration || displayDuration < 0) {
      return;
    }

    let daysInSeconds = displayDuration * 24 * 60 * 60;
    let firstSeenEndSeconds = daysInSeconds + firstSeenStartSeconds;
    let currentDateSeconds = Math.round(Date.now() / 1000);

    lazy.logConsole.debug(
      "Toggle duration experiment - first time toggle seen on:",
      new Date(firstSeenStartSeconds * 1000).toLocaleDateString()
    );
    lazy.logConsole.debug(
      "Toggle duration experiment - first time toggle will change on:",
      new Date(firstSeenEndSeconds * 1000).toLocaleDateString()
    );
    lazy.logConsole.debug(
      "Toggle duration experiment - current date:",
      new Date(currentDateSeconds * 1000).toLocaleDateString()
    );

    if (currentDateSeconds >= firstSeenEndSeconds) {
      this.sendAsyncMessage("PictureInPicture:SetHasUsed", {
        hasUsed: true,
      });
    }
  }

  worthTracking(intersectionEntry) {
    return intersectionEntry.isIntersecting;
  }

  onIntersection(entries) {
    let state = this.docState;
    if (!state) {
      return;
    }
    let oldVisibleVideosCount = state.visibleVideosCount;
    for (let entry of entries) {
      let video = entry.target;
      if (this.worthTracking(entry)) {
        if (!state.weakVisibleVideos.has(video)) {
          state.weakVisibleVideos.add(video);
          state.visibleVideosCount++;
          if (this.toggleTesting) {
            gWeakIntersectingVideosForTesting.add(video);
          }
        }
      } else if (state.weakVisibleVideos.has(video)) {
        state.weakVisibleVideos.delete(video);
        state.visibleVideosCount--;
        if (this.toggleTesting) {
          gWeakIntersectingVideosForTesting.delete(video);
        }
      }
    }

    if (!oldVisibleVideosCount && state.visibleVideosCount) {
      if (this.toggleTesting || !this.contentWindow) {
        this.beginTrackingMouseOverVideos();
      } else {
        this.contentWindow.requestIdleCallback(() => {
          this.beginTrackingMouseOverVideos();
        });
      }
    } else if (oldVisibleVideosCount && !state.visibleVideosCount) {
      if (this.toggleTesting || !this.contentWindow) {
        this.stopTrackingMouseOverVideos();
      } else {
        this.contentWindow.requestIdleCallback(() => {
          this.stopTrackingMouseOverVideos();
        });
      }
    }
  }

  addMouseButtonListeners() {
    this.contentWindow.windowRoot.addEventListener("pointerdown", this, {
      capture: true,
    });
    this.contentWindow.windowRoot.addEventListener("mousedown", this, {
      capture: true,
    });
    this.contentWindow.windowRoot.addEventListener("mouseup", this, {
      capture: true,
    });
    this.contentWindow.windowRoot.addEventListener("pointerup", this, {
      capture: true,
    });
    this.contentWindow.windowRoot.addEventListener("click", this, {
      capture: true,
    });
    this.contentWindow.windowRoot.addEventListener("mouseout", this, {
      capture: true,
    });
    this.contentWindow.windowRoot.addEventListener("touchstart", this, {
      capture: true,
    });
  }

  removeMouseButtonListeners() {
    if (!this.contentWindow || !this.contentWindow.windowRoot) {
      return;
    }

    this.contentWindow.windowRoot.removeEventListener("pointerdown", this, {
      capture: true,
    });
    this.contentWindow.windowRoot.removeEventListener("mousedown", this, {
      capture: true,
    });
    this.contentWindow.windowRoot.removeEventListener("mouseup", this, {
      capture: true,
    });
    this.contentWindow.windowRoot.removeEventListener("pointerup", this, {
      capture: true,
    });
    this.contentWindow.windowRoot.removeEventListener("click", this, {
      capture: true,
    });
    this.contentWindow.windowRoot.removeEventListener("mouseout", this, {
      capture: true,
    });
    this.contentWindow.windowRoot.removeEventListener("touchstart", this, {
      capture: true,
    });
  }

  beginTrackingMouseOverVideos() {
    let state = this.docState;
    if (!state.mousemoveDeferredTask) {
      state.mousemoveDeferredTask = new lazy.DeferredTask(() => {
        this.checkLastMouseMove();
      }, MOUSEMOVE_PROCESSING_DELAY_MS);
    }
    this.document.addEventListener("mousemove", this, {
      mozSystemGroup: true,
      capture: true,
    });
    this.contentWindow.addEventListener("pageshow", this, {
      mozSystemGroup: true,
    });
    this.contentWindow.addEventListener("pagehide", this, {
      mozSystemGroup: true,
    });
    lazy.logConsole.debug("Adding visibilitychange event handler");
    this.contentWindow.addEventListener("visibilitychange", this, {
      mozSystemGroup: true,
    });
    this.addMouseButtonListeners();
    state.isTrackingVideos = true;
  }

  stopTrackingMouseOverVideos() {
    let state = this.docState;
    if (!state.mousemoveDeferredTask) {
      return;
    }
    state.mousemoveDeferredTask.disarm();
    this.document.removeEventListener("mousemove", this, {
      mozSystemGroup: true,
      capture: true,
    });
    if (this.contentWindow) {
      this.contentWindow.removeEventListener("pageshow", this, {
        mozSystemGroup: true,
      });
      this.contentWindow.removeEventListener("pagehide", this, {
        mozSystemGroup: true,
      });
      lazy.logConsole.debug("Removing visibilitychange event handler");
      this.contentWindow.removeEventListener("visibilitychange", this, {
        mozSystemGroup: true,
      });
    }
    this.removeMouseButtonListeners();
    let oldOverVideo = this.getWeakOverVideo();
    if (oldOverVideo) {
      this.onMouseLeaveVideo(oldOverVideo);
    }
    state.isTrackingVideos = false;
  }

  onPageShow() {
    let state = this.docState;
    state.isUnloaded = false;
    if (state.isTrackingVideos) {
      this.addMouseButtonListeners();
    }
  }

  onPageHide() {
    let state = this.docState;
    state.isUnloaded = true;
    if (state.isTrackingVideos) {
      this.removeMouseButtonListeners();
    }
  }

  onVisibilityChange() {
    let state = this.docState;
    if (state.isUnloaded) {
      return;
    }

    if (this.document.visibilityState == "hidden") {
      this.sendAsyncMessage("PictureInPicture:VideoTabHidden");
    }
  }

  onPointerDown(event) {
    if (
      event.button != 0 ||
      (AppConstants.platform == "macosx" && event.button == 0 && event.ctrlKey)
    ) {
      return;
    }

    let video = this.getWeakOverVideo();
    if (!video) {
      return;
    }

    let shadowRoot = video.openOrClosedShadowRoot;
    if (!shadowRoot) {
      return;
    }

    let state = this.docState;

    let overVideo = (() => {
      let { clientX, clientY } = event;
      let winUtils = this.contentWindow.windowUtils;
      let elements = winUtils.nodesFromRect(
        clientX,
        clientY,
        1,
        1,
        1,
        1,
        true,
        false,
         true,
        state.toggleVisibilityThreshold
      );

      for (let element of elements) {
        if (element == video || element.containingShadowRoot == shadowRoot) {
          return true;
        }
      }

      return false;
    })();

    if (!overVideo) {
      return;
    }

    let toggle = this.getToggleElement(shadowRoot);
    if (this.isMouseOverToggle(toggle, event)) {
      state.isClickingToggle = true;
      state.clickedElement = Cu.getWeakReference(event.originalTarget);
      event.stopImmediatePropagation();

      this.startPictureInPicture(event, video, toggle);
    }
  }

  startPictureInPicture(event, video) {
    let pipEvent = new this.contentWindow.CustomEvent(
      "MozTogglePictureInPicture",
      {
        bubbles: true,
        detail: { reason: "Toggle" },
      }
    );
    this.contentWindow.windowUtils.dispatchEventToChromeOnly(video, pipEvent);

    this.onMouseLeaveVideo(video);
  }

  onMouseButtonEvent(event) {
    if (
      event.button != 0 ||
      (AppConstants.platform == "macosx" && event.button == 0 && event.ctrlKey)
    ) {
      return;
    }

    let state = this.docState;
    if (state.isClickingToggle) {
      event.stopImmediatePropagation();

      let isMouseUpOnOtherElement =
        event.type == "mouseup" &&
        (!state.clickedElement ||
          state.clickedElement.get() != event.originalTarget);

      if (
        isMouseUpOnOtherElement ||
        event.type == "click" ||
        event.pointerType == "touch"
      ) {
        state.isClickingToggle = false;
        state.clickedElement = null;
      }
    }
  }

  onMouseOut(event) {
    if (!event.relatedTarget) {
      let video = this.getWeakOverVideo();
      if (!video) {
        return;
      }

      this.onMouseLeaveVideo(video);
    }
  }

  onMouseMove(event) {
    let state = this.docState;

    if (state.hideToggleDeferredTask) {
      state.hideToggleDeferredTask.disarm();
      state.hideToggleDeferredTask.arm();
    }

    state.lastMouseMoveEvent = event;
    state.mousemoveDeferredTask.arm();
  }

  checkLastMouseMove() {
    let state = this.docState;
    let event = state.lastMouseMoveEvent;
    let { clientX, clientY } = event;
    lazy.logConsole.debug("Visible videos count:", state.visibleVideosCount);
    lazy.logConsole.debug("Tracking videos:", state.isTrackingVideos);
    let winUtils = this.contentWindow.windowUtils;
    let elements = winUtils.nodesFromRect(
      clientX,
      clientY,
      1,
      1,
      1,
      1,
      true,
      false,
       true
    );

    for (let element of elements) {
      lazy.logConsole.debug("Element id under cursor:", element.id);
      lazy.logConsole.debug(
        "Node name of an element under cursor:",
        element.nodeName
      );
      lazy.logConsole.debug(
        "Supported <video> element:",
        state.weakVisibleVideos.has(element)
      );
      lazy.logConsole.debug(
        "PiP window is open:",
        element.isCloningElementVisually
      );

      for (let el = element; el; el = el.containingShadowRoot?.host) {
        if (state.weakVisibleVideos.has(el) && !el.isCloningElementVisually) {
          lazy.logConsole.debug("Found supported element");
          this.onMouseOverVideo(el, event);
          return;
        }
      }
    }

    let oldOverVideo = this.getWeakOverVideo();
    if (oldOverVideo) {
      this.onMouseLeaveVideo(oldOverVideo);
    }
  }

  onMouseOverVideo(video, event) {
    let oldOverVideo = this.getWeakOverVideo();
    let shadowRoot = video.openOrClosedShadowRoot;

    if (shadowRoot.firstChild && video != oldOverVideo) {
      shadowRoot.firstChild.toggleAttribute(
        "flipped",
        video.getTransformToViewport().a == -1
      );
    }

    if (!shadowRoot) {
      if (oldOverVideo) {
        this.onMouseLeaveVideo(oldOverVideo);
      }

      return;
    }

    let state = this.docState;
    let toggle = this.getToggleElement(shadowRoot);
    let controlsOverlay = shadowRoot.querySelector(".controlsOverlay");

    if (state.checkedPolicyDocumentURI != this.document.documentURI) {
      state.togglePolicy = lazy.TOGGLE_POLICIES.DEFAULT;
      let siteOverrides = this.toggleTesting
        ? PictureInPictureToggleChild.getSiteOverrides()
        : lazy.gSiteOverrides;

      let visibilityThresholdPref = Services.prefs.getFloatPref(
        TOGGLE_VISIBILITY_THRESHOLD_PREF,
        "1.0"
      );

      if (!this.videoWrapper) {
        this.videoWrapper = applyWrapper(this, video);
      }

      for (let [override, { policy, visibilityThreshold }] of siteOverrides) {
        if (
          (policy || visibilityThreshold) &&
          override.matches(this.document.documentURI)
        ) {
          state.togglePolicy = this.videoWrapper?.shouldHideToggle(video)
            ? lazy.TOGGLE_POLICIES.HIDDEN
            : policy || lazy.TOGGLE_POLICIES.DEFAULT;
          state.toggleVisibilityThreshold =
            visibilityThreshold || visibilityThresholdPref;
          break;
        }
      }

      state.checkedPolicyDocumentURI = this.document.documentURI;
    }

    if (
      state.togglePolicy != lazy.TOGGLE_POLICIES.DEFAULT &&
      !(state.togglePolicy == lazy.TOGGLE_POLICIES.BOTTOM && video.controls)
    ) {
      toggle.setAttribute(
        "policy",
        lazy.TOGGLE_POLICY_STRINGS[state.togglePolicy]
      );
    } else {
      toggle.removeAttribute("policy");
    }

    const nimbusExperimentVariables =
      lazy.NimbusFeatures.pictureinpicture.getAllVariables({
        defaultValues: {
          oldToggle: true,
          title: null,
          message: false,
          showIconOnly: false,
          displayDuration: TOGGLE_FIRST_TIME_DURATION_DAYS,
        },
      });

    if (!nimbusExperimentVariables.oldToggle) {
      let controlsContainer = shadowRoot.querySelector(".controlsContainer");
      let pipWrapper = shadowRoot.querySelector(".pip-wrapper");

      controlsContainer.classList.add("experiment");
      pipWrapper.classList.add("experiment");
    } else {
      let controlsContainer = shadowRoot.querySelector(".controlsContainer");
      let pipWrapper = shadowRoot.querySelector(".pip-wrapper");

      controlsContainer.classList.remove("experiment");
      pipWrapper.classList.remove("experiment");
    }

    if (nimbusExperimentVariables.title) {
      let pipExplainer = shadowRoot.querySelector(".pip-explainer");
      let pipLabel = shadowRoot.querySelector(".pip-label");

      if (pipExplainer && nimbusExperimentVariables.message) {
        pipExplainer.innerText = nimbusExperimentVariables.message;
      }
      pipLabel.innerText = nimbusExperimentVariables.title;
    } else if (nimbusExperimentVariables.showIconOnly) {
      let pipExpanded = shadowRoot.querySelector(".pip-expanded");
      pipExpanded.style.display = "none";
      let pipSmall = shadowRoot.querySelector(".pip-small");
      pipSmall.style.opacity = "1";

      let pipIcon = shadowRoot.querySelectorAll(".pip-icon")[1];
      pipIcon.style.display = "block";
    }

    controlsOverlay.removeAttribute("hidetoggle");

    if (!state.hideToggleDeferredTask && !this.toggleTesting) {
      state.hideToggleDeferredTask = new lazy.DeferredTask(() => {
        controlsOverlay.setAttribute("hidetoggle", true);
      }, TOGGLE_HIDING_TIMEOUT_MS);
    }

    if (oldOverVideo) {
      if (oldOverVideo == video) {
        this.checkHoverToggle(toggle, event);
        return;
      }

      this.onMouseLeaveVideo(oldOverVideo);
    }

    state.weakOverVideo = Cu.getWeakReference(video);
    controlsOverlay.classList.add("hovering");

    if (
      state.togglePolicy != lazy.TOGGLE_POLICIES.HIDDEN &&
      !toggle.hasAttribute("hidden")
    ) {
      const hasUsedPiP = Services.prefs.getBoolPref(TOGGLE_HAS_USED_PREF);
      if (!hasUsedPiP) {
        lazy.NimbusFeatures.pictureinpicture.recordExposureEvent();

        const firstSeenSeconds = Services.prefs.getIntPref(
          TOGGLE_FIRST_SEEN_PREF,
          0
        );

        if (!firstSeenSeconds || firstSeenSeconds < 0) {
          let firstTimePiPStartDate = Math.round(Date.now() / 1000);
          this.sendAsyncMessage("PictureInPicture:SetFirstSeen", {
            dateSeconds: firstTimePiPStartDate,
          });
        } else if (nimbusExperimentVariables.displayDuration) {
          this.changeToIconIfDurationEnd(firstSeenSeconds);
        }
      }
    }

    this.checkHoverToggle(toggle, event);
  }

  checkHoverToggle(toggle, event) {
    toggle.classList.toggle("hovering", this.isMouseOverToggle(toggle, event));
  }

  onMouseLeaveVideo(video) {
    let state = this.docState;
    let shadowRoot = video.openOrClosedShadowRoot;

    if (shadowRoot) {
      let controlsOverlay = shadowRoot.querySelector(".controlsOverlay");
      let toggle = this.getToggleElement(shadowRoot);
      controlsOverlay?.classList.remove("hovering");
      toggle?.classList.remove("hovering");
    }

    state.weakOverVideo = null;

    if (!this.toggleTesting) {
      state.hideToggleDeferredTask.disarm();
      state.mousemoveDeferredTask.disarm();
    }

    state.hideToggleDeferredTask = null;
  }

  isMouseOverToggle(toggle, event) {
    let toggleRect =
      toggle.documentGlobal.windowUtils.getBoundsWithoutFlushing(toggle);

    toggleRect = lazy.Rect.fromRect(toggleRect);
    let clickableChildren = toggle.querySelectorAll(".clickable");
    for (let child of clickableChildren) {
      let childRect = lazy.Rect.fromRect(
        child.documentGlobal.windowUtils.getBoundsWithoutFlushing(child)
      );
      toggleRect.expandToContain(childRect);
    }

    if (!toggleRect.width || !toggleRect.height) {
      return false;
    }

    let { clientX, clientY } = event;

    return (
      clientX >= toggleRect.left &&
      clientX <= toggleRect.right &&
      clientY >= toggleRect.top &&
      clientY <= toggleRect.bottom
    );
  }

  checkContextMenu(event) {
    let video = this.getWeakOverVideo();
    if (!video) {
      return;
    }

    let shadowRoot = video.openOrClosedShadowRoot;
    if (!shadowRoot) {
      return;
    }

    let toggle = this.getToggleElement(shadowRoot);
    if (this.isMouseOverToggle(toggle, event)) {
      let devicePixelRatio = toggle.documentGlobal.devicePixelRatio;
      this.sendAsyncMessage("PictureInPicture:OpenToggleContextMenu", {
        screenXDevPx: event.screenX * devicePixelRatio,
        screenYDevPx: event.screenY * devicePixelRatio,
        inputSource: event.inputSource,
      });
      event.stopImmediatePropagation();
      event.preventDefault();
    }
  }

  getToggleElement(shadowRoot) {
    return shadowRoot.getElementById("pictureInPictureToggle");
  }

  static isTracking(video) {
    return gWeakIntersectingVideosForTesting.has(video);
  }

  static getSiteOverrides() {
    let result = [];
    let patterns = Services.cpmm.sharedData.get(
      "PictureInPicture:SiteOverrides"
    );
    for (let pattern in patterns) {
      let matcher = new MatchPattern(pattern);
      result.push([matcher, patterns[pattern]]);
    }
    return result;
  }
}

export class PictureInPictureChild extends JSWindowActorChild {
  #subtitlesEnabled = false;
  weakVideo = null;

  weakPlayerContent = null;

  _currentWebVTTTrack = null;

  #weakPictureInPictureWindow;

  observerFunction = null;

  observe(subject, topic, data) {
    if (topic != "nsPref:changed") {
      return;
    }

    switch (data) {
      case "media.videocontrols.picture-in-picture.display-text-tracks.enabled": {
        const originatingVideo = this.getWeakVideo();
        let isTextTrackPrefEnabled = Services.prefs.getBoolPref(
          "media.videocontrols.picture-in-picture.display-text-tracks.enabled"
        );

        if (isTextTrackPrefEnabled) {
          this.setupTextTracks(originatingVideo);
        } else {
          this.removeTextTracks(originatingVideo);
        }
        break;
      }
    }
  }

  createTextTracksStyleSheet() {
    let headStyleElement = this.document.createElement("link");
    headStyleElement.setAttribute("rel", "stylesheet");
    headStyleElement.setAttribute(
      "href",
      "chrome://global/skin/pictureinpicture/texttracks.css"
    );
    headStyleElement.setAttribute("type", "text/css");
    return headStyleElement;
  }

  setupTextTracks(originatingVideo) {
    const isWebVTTSupported = !!originatingVideo.textTracks?.length;

    if (!isWebVTTSupported) {
      this.setUpCaptionChangeListener(originatingVideo);
      return;
    }

    this.setActiveTextTrack(originatingVideo.textTracks);

    if (!this._currentWebVTTTrack) {
      this.setUpCaptionChangeListener(originatingVideo);
      return;
    }

    originatingVideo.textTracks.addEventListener("change", this);
    this._currentWebVTTTrack.addEventListener("cuechange", this.onCueChange);

    const cues = this._currentWebVTTTrack.activeCues;
    this.updateWebVTTTextTracksDisplay(cues);
  }

  toggleTextTracks() {
    let textTracks = this.document.getElementById("texttracks");
    textTracks.style.display =
      textTracks.style.display === "none" ? "" : "none";
  }

  removeTextTracks(originatingVideo) {
    const isWebVTTSupported = !!originatingVideo.textTracks;

    this.removeCaptionChangeListener(originatingVideo);

    if (!isWebVTTSupported) {
      return;
    }

    originatingVideo.textTracks.removeEventListener("change", this);
    this._currentWebVTTTrack?.removeEventListener(
      "cuechange",
      this.onCueChange
    );
    this._currentWebVTTTrack = null;
    this.updateWebVTTTextTracksDisplay(null);
  }

  moveTextTracks(data) {
    const {
      isFullscreen,
      isVideoControlsShowing,
      playerBottomControlsDOMRect,
      isScrubberShowing,
    } = data;
    let textTracks = this.document.getElementById("texttracks");
    const originatingWindow = this.getWeakVideo().documentGlobal;
    const isReducedMotionEnabled = originatingWindow.matchMedia(
      "(prefers-reduced-motion: reduce)"
    ).matches;
    const textTracksFontScale = this.document
      .querySelector(":root")
      .style.getPropertyValue("--font-scale");

    if (isFullscreen || isReducedMotionEnabled) {
      textTracks.removeAttribute("overlap-video-controls");
      return;
    }

    if (isVideoControlsShowing) {
      let playerVideoRect = textTracks.parentElement.getBoundingClientRect();
      let isOverlap =
        playerVideoRect.bottom - textTracksFontScale * playerVideoRect.height >
        playerBottomControlsDOMRect.top;

      if (isOverlap) {
        const root = this.document.querySelector(":root");
        if (isScrubberShowing) {
          root.style.setProperty("--player-controls-scrubber-height", "30px");
        } else {
          root.style.setProperty("--player-controls-scrubber-height", "0px");
        }
        textTracks.setAttribute("overlap-video-controls", true);
      } else {
        textTracks.removeAttribute("overlap-video-controls");
      }
    } else {
      textTracks.removeAttribute("overlap-video-controls");
    }
  }

  updateWebVTTTextTracksDisplay(textTrackCues) {
    let pipWindowTracksContainer = this.document.getElementById("texttracks");
    let playerVideo = this.document.getElementById("playervideo");
    let playerVideoWindow = playerVideo.documentGlobal;

    pipWindowTracksContainer.replaceChildren();

    if (!textTrackCues) {
      return;
    }

    if (!this.isSubtitlesEnabled) {
      this.isSubtitlesEnabled = true;
      this.sendAsyncMessage("PictureInPicture:EnableSubtitlesButton");
    }

    let allCuesArray = [...textTrackCues];
    this.getOrderedWebVTTCues(allCuesArray);
    allCuesArray.forEach(cue => {
      let text = cue.text;
      const re = /(\s*\n{2,}\s*)/g;
      text = text.trim();
      text = text.replace(re, "\n");
      let cueTextNode = WebVTT.convertCueToDOMTree(playerVideoWindow, text);
      let cueDiv = this.document.createElement("div");
      cueDiv.appendChild(cueTextNode);
      pipWindowTracksContainer.appendChild(cueDiv);
    });
  }

  getOrderedWebVTTCues(allCuesArray) {
    if (!allCuesArray || allCuesArray.length <= 1) {
      return;
    }

    let allCuesHaveNumericLines = allCuesArray.find(cue => cue.line !== "auto");

    if (allCuesHaveNumericLines) {
      allCuesArray.sort((cue1, cue2) => cue1.line - cue2.line);
    } else if (allCuesArray.length >= 2) {
      allCuesArray.reverse();
    }
  }

  getPictureInPictureWindow() {
    if (this.#weakPictureInPictureWindow) {
      try {
        return this.#weakPictureInPictureWindow.get();
      } catch (e) {}
    }
    return null;
  }

  getWeakVideo() {
    if (this.weakVideo) {
      try {
        return this.weakVideo.get();
      } catch (e) {
        return null;
      }
    }
    return null;
  }

  getWeakPlayerContent() {
    if (this.weakPlayerContent) {
      try {
        return this.weakPlayerContent.get();
      } catch (e) {
        return null;
      }
    }
    return null;
  }

  inPictureInPicture(video) {
    return this.getWeakVideo() === video;
  }

  static videoIsPlaying(video) {
    return !!(!video.paused && !video.ended && video.readyState > 2);
  }

  static videoIsMuted(video) {
    return this.videoWrapper.isMuted(video);
  }

  static videoIsPiPEligible(video) {
    if (lazy.PIP_TOGGLE_ALWAYS_SHOW) {
      return true;
    }

    if (isNaN(video.duration) || video.duration < lazy.MIN_VIDEO_LENGTH) {
      return false;
    }

    const MIN_VIDEO_DIMENSION = 140; 
    if (
      video.clientWidth < MIN_VIDEO_DIMENSION ||
      video.clientHeight < MIN_VIDEO_DIMENSION
    ) {
      return false;
    }

    if (!video.mozHasAudio) {
      return false;
    }

    if (!video.checkVisibility()) {
      return false;
    }

    return true;
  }

  handleEvent(event) {
    switch (event.type) {
      case "MozStopPictureInPicture": {
        const video = this.getWeakVideo();
        if (event.isTrusted && event.target === video) {
          const reason = event.detail?.reason || "VideoElRemove";
          if (reason === "VideoElRemove") {
            this.closePictureInPictureIfDisconnected({ reason, video });
          } else {
            this.closePictureInPicture({ reason });
          }
        }
        break;
      }
      case "pagehide": {
        this.closePictureInPicture({ reason: "Pagehide" });
        break;
      }
      case "MozDOMFullscreen:Request": {
        this.closePictureInPicture({ reason: "Fullscreen" });
        break;
      }
      case "playing":
      case "play": {
        this.sendAsyncMessage("PictureInPicture:Playing");
        break;
      }
      case "pause": {
        this.sendAsyncMessage("PictureInPicture:Paused");
        break;
      }
      case "volumechange": {
        let video = this.getWeakVideo();

        if (video !== event.target) {
          lazy.logConsole.error(
            "PictureInPictureChild received volumechange for " +
              "the wrong video!"
          );
          return;
        }

        if (this.constructor.videoIsMuted(video)) {
          this.sendAsyncMessage("PictureInPicture:Muting");
        } else {
          this.sendAsyncMessage("PictureInPicture:Unmuting");
        }
        this.sendAsyncMessage("PictureInPicture:VolumeChange", {
          volume: this.videoWrapper.getVolume(video),
        });
        break;
      }
      case "resize": {
        let video = event.target;
        if (this.inPictureInPicture(video)) {
          this.sendAsyncMessage("PictureInPicture:Resize", {
            videoHeight: video.videoHeight,
            videoWidth: video.videoWidth,
          });
        }
        this.setupTextTracks(video);
        break;
      }
      case "emptied": {
        this.isSubtitlesEnabled = false;
        if (this.emptiedTimeout) {
          clearTimeout(this.emptiedTimeout);
          this.emptiedTimeout = null;
        }
        if (lazy.PIP_AUTO_CLOSE) {
          let video = this.getWeakVideo();
          this.emptiedTimeout = setTimeout(() => {
            if (!video || !video.src) {
              this.closePictureInPicture({ reason: "VideoElEmptied" });
            }
          }, lazy.EMPTIED_TIMEOUT_MS);
        }
        break;
      }
      case "change": {
        if (this._currentWebVTTTrack) {
          this._currentWebVTTTrack.removeEventListener(
            "cuechange",
            this.onCueChange
          );
          this._currentWebVTTTrack = null;
        }

        const tracks = event.target;
        this.setActiveTextTrack(tracks);
        const isCurrentTrackAvailable = this._currentWebVTTTrack;

        if (!isCurrentTrackAvailable || !tracks.length) {
          this.updateWebVTTTextTracksDisplay(null);
          return;
        }

        this._currentWebVTTTrack.addEventListener(
          "cuechange",
          this.onCueChange
        );
        const cues = this._currentWebVTTTrack.activeCues;
        this.updateWebVTTTextTracksDisplay(cues);
        break;
      }
      case "timeupdate":
      case "durationchange": {
        let video = this.getWeakVideo();
        let currentTime = this.videoWrapper.getCurrentTime(video);
        let duration = this.videoWrapper.getDuration(video);
        let scrubberPosition = currentTime === 0 ? 0 : currentTime / duration;
        let timestamp = this.videoWrapper.formatTimestamp(
          currentTime,
          duration
        );
        if (timestamp !== undefined && lazy.IMPROVED_CONTROLS_ENABLED_PREF) {
          this.sendAsyncMessage(
            "PictureInPicture:SetTimestampAndScrubberPosition",
            {
              scrubberPosition,
              timestamp,
            }
          );
        }
        break;
      }
    }
  }

  async closePictureInPicture({ reason }) {
    let video = this.getWeakVideo();
    if (video) {
      gOriginatingVideoMap.delete(video);
      this.untrackOriginatingVideo(video);
    }
    const query = this.sendQuery("PictureInPicture:Close", {
      reason,
    });

    let playerContent = this.getWeakPlayerContent();
    if (playerContent) {
      if (!playerContent.closed) {
        await new Promise(resolve => {
          playerContent.addEventListener("unload", resolve, {
            once: true,
          });
        });
      }
      this.weakPlayerContent = null;
    }
    await query;
  }

  closePictureInPictureIfDisconnected({ reason, video }) {
    this.contentWindow.requestAnimationFrame(() =>
      Services.tm.dispatchToMainThread(() => {
        if (video?.isConnected) {
          const playerVideo = this.document.getElementById("playervideo");
          if (playerVideo) {
            video.cloneElementVisually(playerVideo);
            this.stylePlayerVideo(video, playerVideo);
            return;
          }
        }
        this.closePictureInPicture({ reason });
      })
    );
  }

  async receiveMessage(message) {
    switch (message.name) {
      case "PictureInPicture:SetupPlayer": {
        const { videoRef, isPipApiRequest, initDimension } = message.data;
        return await this.setupPlayer(videoRef, isPipApiRequest, initDimension);
      }
      case "PictureInPicture:Play": {
        this.play();
        break;
      }
      case "PictureInPicture:Pause": {
        if (message.data && message.data.reason == "pip-closed") {
          let video = this.getWeakVideo();

          if (video && MediaStream.isInstance(video.srcObject)) {
            break;
          }
        }
        this.pause();
        break;
      }
      case "PictureInPicture:Mute": {
        this.mute();
        break;
      }
      case "PictureInPicture:Unmute": {
        this.unmute();
        break;
      }
      case "PictureInPicture:SeekForward":
      case "PictureInPicture:SeekBackward": {
        let selectedTime;
        let video = this.getWeakVideo();
        let currentTime = this.videoWrapper.getCurrentTime(video);
        if (message.name == "PictureInPicture:SeekBackward") {
          selectedTime = currentTime - SEEK_TIME_SECS;
          selectedTime = selectedTime >= 0 ? selectedTime : 0;
        } else {
          const maxtime = this.videoWrapper.getDuration(video);
          selectedTime = currentTime + SEEK_TIME_SECS;
          selectedTime = selectedTime <= maxtime ? selectedTime : maxtime;
        }
        this.videoWrapper.setCurrentTime(video, selectedTime);
        break;
      }
      case "PictureInPicture:KeyDown": {
        this.keyDown(message.data);
        break;
      }
      case "PictureInPicture:EnterFullscreen":
      case "PictureInPicture:ExitFullscreen": {
        let textTracks = this.document.getElementById("texttracks");
        if (textTracks) {
          this.moveTextTracks(message.data);
        }
        break;
      }
      case "PictureInPicture:ShowVideoControls":
      case "PictureInPicture:HideVideoControls": {
        let textTracks = this.document.getElementById("texttracks");
        if (textTracks) {
          this.moveTextTracks(message.data);
        }
        break;
      }
      case "PictureInPicture:ToggleTextTracks": {
        this.toggleTextTracks();
        break;
      }
      case "PictureInPicture:ChangeFontSizeTextTracks": {
        this.setTextTrackFontSize();
        break;
      }
      case "PictureInPicture:SetVideoTime": {
        const { scrubberPosition, wasPlaying } = message.data;
        this.setVideoTime(scrubberPosition, wasPlaying);
        break;
      }
      case "PictureInPicture:SetVolume": {
        const { volume } = message.data;
        let video = this.getWeakVideo();
        this.videoWrapper.setVolume(video, volume);
        break;
      }
    }
    return undefined;
  }

  setVideoTime(scrubberPosition, wasPlaying) {
    const video = this.getWeakVideo();
    let duration = this.videoWrapper.getDuration(video);
    let currentTime = scrubberPosition * duration;
    this.videoWrapper.setCurrentTime(video, currentTime, wasPlaying);
  }

  shouldShowHiddenTextTracks() {
    const video = this.getWeakVideo();
    if (!video) {
      return false;
    }
    const { documentURI } = video.ownerDocument;
    if (!documentURI) {
      return false;
    }
    for (let [override, { showHiddenTextTracks }] of lazy.gSiteOverrides) {
      if (override.matches(documentURI) && showHiddenTextTracks) {
        return true;
      }
    }
    return false;
  }

  setActiveTextTrack(textTrackList) {
    this._currentWebVTTTrack = null;

    for (let i = 0; i < textTrackList.length; i++) {
      let track = textTrackList[i];
      let isCCText = track.kind === "subtitles" || track.kind === "captions";
      let shouldShowTrack =
        track.mode === "showing" ||
        (track.mode === "hidden" && this.shouldShowHiddenTextTracks());
      if (isCCText && shouldShowTrack && track.cues) {
        this._currentWebVTTTrack = track;
        break;
      }
    }
  }

  setTextTrackFontSize() {
    const fontSize = Services.prefs.getStringPref(
      TEXT_TRACK_FONT_SIZE,
      "medium"
    );
    const root = this.document.querySelector(":root");
    if (fontSize === "small") {
      root.style.setProperty("--font-scale", "0.03");
    } else if (fontSize === "large") {
      root.style.setProperty("--font-scale", "0.09");
    } else {
      root.style.setProperty("--font-scale", "0.06");
    }
  }

  trackOriginatingVideo(originatingVideo) {
    this.observerFunction = (subject, topic, data) => {
      this.observe(subject, topic, data);
    };
    Services.prefs.addObserver(
      "media.videocontrols.picture-in-picture.display-text-tracks.enabled",
      this.observerFunction
    );

    let originatingWindow = originatingVideo.documentGlobal;
    if (originatingWindow) {
      originatingWindow.addEventListener("pagehide", this);
      originatingVideo.addEventListener("play", this);
      originatingVideo.addEventListener("playing", this);
      originatingVideo.addEventListener("pause", this);
      originatingVideo.addEventListener("volumechange", this);
      originatingVideo.addEventListener("resize", this);
      originatingVideo.addEventListener("emptied", this);
      originatingVideo.addEventListener("timeupdate", this);

      if (lazy.DISPLAY_TEXT_TRACKS_PREF) {
        this.setupTextTracks(originatingVideo);
      }

      let chromeEventHandler = originatingWindow.docShell.chromeEventHandler;
      chromeEventHandler.addEventListener(
        "MozDOMFullscreen:Request",
        this,
        true
      );
      chromeEventHandler.addEventListener(
        "MozStopPictureInPicture",
        this,
        true
      );
    }
  }

  setUpCaptionChangeListener(originatingVideo) {
    if (this.videoWrapper) {
      this.videoWrapper.setCaptionContainerObserver(originatingVideo, this);
    }
  }

  removeCaptionChangeListener(originatingVideo) {
    if (this.videoWrapper) {
      this.videoWrapper.removeCaptionContainerObserver(originatingVideo, this);
    }
  }

  untrackOriginatingVideo(originatingVideo) {
    Services.prefs.removeObserver(
      "media.videocontrols.picture-in-picture.display-text-tracks.enabled",
      this.observerFunction
    );

    let originatingWindow = originatingVideo.documentGlobal;
    if (originatingWindow) {
      originatingWindow.removeEventListener("pagehide", this);
      originatingVideo.removeEventListener("play", this);
      originatingVideo.removeEventListener("playing", this);
      originatingVideo.removeEventListener("pause", this);
      originatingVideo.removeEventListener("volumechange", this);
      originatingVideo.removeEventListener("resize", this);
      originatingVideo.removeEventListener("emptied", this);
      originatingVideo.removeEventListener("timeupdate", this);

      if (lazy.DISPLAY_TEXT_TRACKS_PREF) {
        this.removeTextTracks(originatingVideo);
      }

      let chromeEventHandler = originatingWindow.docShell?.chromeEventHandler;
      if (chromeEventHandler) {
        chromeEventHandler.removeEventListener(
          "MozDOMFullscreen:Request",
          this,
          true
        );
        chromeEventHandler.removeEventListener(
          "MozStopPictureInPicture",
          this,
          true
        );
      }
    }
  }

  async setupPlayer(videoRef, isPipApiRequest, initWindowDimension) {
    const video = await lazy.ContentDOMReference.resolve(videoRef);

    this.weakVideo = Cu.getWeakReference(video);
    let originatingVideo = this.getWeakVideo();
    if (!originatingVideo) {
      await this.closePictureInPicture({ reason: "SetupFailure" });
      return;
    }
    gOriginatingVideoMap.set(video, this);

    this.videoWrapper = applyWrapper(this, originatingVideo);

    if (isPipApiRequest) {
      const pipInstance = gVideoToPipWindow.get(originatingVideo);
      gVideoToPipWindow.delete(originatingVideo);
      if (pipInstance) {
        const { width, height } = initWindowDimension;
        this.setPictureInPictureWindowInstance(pipInstance);
        pipInstance.notifyDimensionsChanged(width, height);
      }
    }

    let loadPromise = new Promise(resolve => {
      this.contentWindow.addEventListener("load", resolve, {
        once: true,
        mozSystemGroup: true,
        capture: true,
      });
    });
    this.contentWindow.location.reload();
    await loadPromise;

    this.weakPlayerContent = Cu.getWeakReference(this.contentWindow);
    gPlayerContents.add(this.contentWindow);

    let doc = this.document;
    let playerVideo = doc.createElement("video");
    playerVideo.id = "playervideo";
    let textTracks = doc.createElement("div");

    doc.body.style.overflow = "hidden";
    doc.body.style.margin = "0";

    playerVideo.style.height = "100vh";
    playerVideo.style.width = "100vw";
    playerVideo.style.backgroundColor = "#000";

    textTracks.id = "texttracks";
    textTracks.setAttribute("overlap-video-controls", true);
    doc.body.appendChild(playerVideo);
    doc.body.appendChild(textTracks);
    let textTracksStyleSheet = this.createTextTracksStyleSheet();
    doc.head.appendChild(textTracksStyleSheet);

    this.setTextTrackFontSize();

    originatingVideo.cloneElementVisually(playerVideo);
    this.stylePlayerVideo(originatingVideo, playerVideo);

    this.onCueChange = this.onCueChange.bind(this);
    this.trackOriginatingVideo(originatingVideo);

    if (!isPipApiRequest) {
      originatingVideo.ownerDocument.notifyUserGestureActivation();
    }

    this.contentWindow.addEventListener(
      "unload",
      () => {
        let v = this.getWeakVideo();
        if (v) {
          this.untrackOriginatingVideo(v);
          v.stopCloningElementVisually();
        }
        this.weakVideo = null;
      },
      { once: true }
    );
  }

  #onResizeNotifyPictureInPictureWindowInstance() {
    const pipWindow = this.getPictureInPictureWindow();
    if (pipWindow) {
      pipWindow.notifyDimensionsChanged(
        this.contentWindow.innerWidth,
        this.contentWindow.innerHeight
      );
    }
  }

  setPictureInPictureWindowInstance(pipWindowInstance) {
    this.#weakPictureInPictureWindow = pipWindowInstance
      ? Cu.getWeakReference(pipWindowInstance)
      : null;

    if (pipWindowInstance) {
      this.contentWindow.addEventListener("resize", () =>
        this.#onResizeNotifyPictureInPictureWindowInstance()
      );
    }
  }

  stylePlayerVideo(originatingVideo, playerVideo) {
    const shadowRoot = originatingVideo.openOrClosedShadowRoot;
    if (originatingVideo.getTransformToViewport().a == -1) {
      shadowRoot.firstChild.setAttribute("flipped", true);
      playerVideo.style.transform = "scaleX(-1)";
    }
  }

  play() {
    let video = this.getWeakVideo();
    if (video && this.videoWrapper) {
      this.videoWrapper.play(video);
    }
  }

  pause() {
    let video = this.getWeakVideo();
    if (video && this.videoWrapper) {
      this.videoWrapper.pause(video);
    }
  }

  mute() {
    let video = this.getWeakVideo();
    if (video && this.videoWrapper) {
      this.videoWrapper.setMuted(video, true);
    }
  }

  unmute() {
    let video = this.getWeakVideo();
    if (video && this.videoWrapper) {
      this.videoWrapper.setMuted(video, false);
    }
  }

  onCueChange() {
    if (!lazy.DISPLAY_TEXT_TRACKS_PREF) {
      this.updateWebVTTTextTracksDisplay(null);
    } else {
      const cues = this._currentWebVTTTrack.activeCues;
      this.updateWebVTTTextTracksDisplay(cues);
    }
  }

  isKeyDisabled(key) {
    const video = this.getWeakVideo();
    if (!video) {
      return false;
    }
    const { documentURI } = video.ownerDocument;
    if (!documentURI) {
      return true;
    }
    for (let [override, { disabledKeyboardControls }] of lazy.gSiteOverrides) {
      if (
        disabledKeyboardControls !== undefined &&
        override.matches(documentURI)
      ) {
        if (disabledKeyboardControls === lazy.KEYBOARD_CONTROLS.ALL) {
          return true;
        }
        return !!(disabledKeyboardControls & key);
      }
    }
    return false;
  }

  /* eslint-disable complexity */
  keyDown({ altKey, shiftKey, metaKey, ctrlKey, keyCode }) {
    let video = this.getWeakVideo();
    if (!video) {
      return;
    }

    var keystroke = "";
    if (altKey) {
      keystroke += "alt-";
    }
    if (shiftKey) {
      keystroke += "shift-";
    }
    if (this.contentWindow.navigator.platform.startsWith("Mac")) {
      if (metaKey) {
        keystroke += "accel-";
      }
      if (ctrlKey) {
        keystroke += "control-";
      }
    } else {
      if (metaKey) {
        keystroke += "meta-";
      }
      if (ctrlKey) {
        keystroke += "accel-";
      }
    }

    switch (keyCode) {
      case this.contentWindow.KeyEvent.DOM_VK_UP:
        keystroke += "upArrow";
        break;
      case this.contentWindow.KeyEvent.DOM_VK_DOWN:
        keystroke += "downArrow";
        break;
      case this.contentWindow.KeyEvent.DOM_VK_LEFT:
        keystroke += "leftArrow";
        break;
      case this.contentWindow.KeyEvent.DOM_VK_RIGHT:
        keystroke += "rightArrow";
        break;
      case this.contentWindow.KeyEvent.DOM_VK_HOME:
        keystroke += "home";
        break;
      case this.contentWindow.KeyEvent.DOM_VK_END:
        keystroke += "end";
        break;
      case this.contentWindow.KeyEvent.DOM_VK_SPACE:
        keystroke += "space";
        break;
      case this.contentWindow.KeyEvent.DOM_VK_W:
        keystroke += "w";
        break;
    }

    const isVideoStreaming = this.videoWrapper.isLive(video);
    var oldval, newval;

    try {
      switch (keystroke) {
        case "space" :
          if (this.isKeyDisabled(lazy.KEYBOARD_CONTROLS.PLAY_PAUSE)) {
            return;
          }

          if (
            this.videoWrapper.getPaused(video) ||
            this.videoWrapper.getEnded(video)
          ) {
            this.videoWrapper.play(video);
          } else {
            this.videoWrapper.pause(video);
          }

          break;
        case "accel-w" :
          if (this.isKeyDisabled(lazy.KEYBOARD_CONTROLS.CLOSE)) {
            return;
          }
          this.pause();
          this.closePictureInPicture({ reason: "ClosePlayerShortcut" });
          break;
        case "downArrow" :
          if (
            this.isKeyDisabled(lazy.KEYBOARD_CONTROLS.VOLUME) ||
            this.videoWrapper.isMuted(video)
          ) {
            return;
          }
          oldval = this.videoWrapper.getVolume(video);
          newval = oldval < 0.1 ? 0 : oldval - 0.1;
          this.videoWrapper.setVolume(video, newval);
          this.videoWrapper.setMuted(video, newval === 0);
          break;
        case "upArrow" :
          if (this.isKeyDisabled(lazy.KEYBOARD_CONTROLS.VOLUME)) {
            return;
          }
          oldval = this.videoWrapper.getVolume(video);
          this.videoWrapper.setVolume(video, oldval > 0.9 ? 1 : oldval + 0.1);
          this.videoWrapper.setMuted(video, false);
          break;
        case "accel-downArrow" :
          if (this.isKeyDisabled(lazy.KEYBOARD_CONTROLS.MUTE_UNMUTE)) {
            return;
          }
          this.videoWrapper.setMuted(video, true);
          break;
        case "accel-upArrow" :
          if (this.isKeyDisabled(lazy.KEYBOARD_CONTROLS.MUTE_UNMUTE)) {
            return;
          }
          this.videoWrapper.setMuted(video, false);
          break;
        case "leftArrow": 
        case "accel-leftArrow" :
          if (
            this.isKeyDisabled(lazy.KEYBOARD_CONTROLS.SEEK) ||
            (isVideoStreaming &&
              this.isKeyDisabled(lazy.KEYBOARD_CONTROLS.LIVE_SEEK))
          ) {
            return;
          }

          oldval = this.videoWrapper.getCurrentTime(video);
          if (keystroke == "leftArrow") {
            newval = oldval - SEEK_TIME_SECS;
          } else {
            newval = oldval - this.videoWrapper.getDuration(video) / 10;
          }
          this.videoWrapper.setCurrentTime(video, newval >= 0 ? newval : 0);
          break;
        case "rightArrow": 
        case "accel-rightArrow" : {
          if (
            this.isKeyDisabled(lazy.KEYBOARD_CONTROLS.SEEK) ||
            (isVideoStreaming &&
              this.isKeyDisabled(lazy.KEYBOARD_CONTROLS.LIVE_SEEK))
          ) {
            return;
          }

          oldval = this.videoWrapper.getCurrentTime(video);
          var maxtime = this.videoWrapper.getDuration(video);
          if (keystroke == "rightArrow") {
            newval = oldval + SEEK_TIME_SECS;
          } else {
            newval = oldval + maxtime / 10;
          }
          let selectedTime = newval <= maxtime ? newval : maxtime;
          this.videoWrapper.setCurrentTime(video, selectedTime);
          break;
        }
        case "home" :
          if (this.isKeyDisabled(lazy.KEYBOARD_CONTROLS.SEEK)) {
            return;
          }
          if (!isVideoStreaming) {
            this.videoWrapper.setCurrentTime(video, 0);
          }
          break;
        case "end" : {
          if (this.isKeyDisabled(lazy.KEYBOARD_CONTROLS.SEEK)) {
            return;
          }

          let duration = this.videoWrapper.getDuration(video);
          if (
            !isVideoStreaming &&
            this.videoWrapper.getCurrentTime(video) != duration
          ) {
            this.videoWrapper.setCurrentTime(video, duration);
          }
          break;
        }
        default:
      }
    } catch (e) {
    }
  }

  get isSubtitlesEnabled() {
    return this.#subtitlesEnabled;
  }

  set isSubtitlesEnabled(val) {
    if (val) {
    } else {
      this.sendAsyncMessage("PictureInPicture:DisableSubtitlesButton");
    }
    this.#subtitlesEnabled = val;
  }
}

class PictureInPictureChildVideoWrapper {
  #sandbox;
  #siteWrapper;
  #PictureInPictureChild;

  constructor(videoWrapperScriptPath, video, pipChild) {
    this.#sandbox = videoWrapperScriptPath
      ? this.#createSandbox(videoWrapperScriptPath, video)
      : null;
    this.#PictureInPictureChild = pipChild;
  }

  #callWrapperMethod({ name, args = [], fallback = () => {}, validateRetVal }) {
    try {
      const wrappedMethod = this.#siteWrapper?.[name];
      if (typeof wrappedMethod === "function") {
        let retVal = wrappedMethod.call(this.#siteWrapper, ...args);

        if (!validateRetVal) {
          lazy.logConsole.error(
            `No return value validator was provided for method ${name}(). Returning null.`
          );
          return null;
        }

        if (!validateRetVal(retVal)) {
          lazy.logConsole.error(
            `Calling method ${name}() returned an unexpected value: ${retVal}. Returning null.`
          );
          return null;
        }

        return retVal;
      }
    } catch (e) {
      lazy.logConsole.error(
        `There was an error while calling ${name}(): `,
        e.message
      );
    }

    return fallback();
  }

  #createSandbox(videoWrapperScriptPath, video) {
    const addonPolicy = WebExtensionPolicy.getByID(
      "pictureinpicture@mozilla.org"
    );
    let wrapperScriptUrl = addonPolicy.getURL(videoWrapperScriptPath);
    let originatingWin = video.documentGlobal;
    let originatingDoc = video.ownerDocument;

    let sandbox = Cu.Sandbox([originatingDoc.nodePrincipal], {
      sandboxName: "Picture-in-Picture video wrapper sandbox",
      sandboxPrototype: originatingWin,
      sameZoneAs: originatingWin,
      wantXrays: false,
    });

    try {
      Services.scriptloader.loadSubScript(wrapperScriptUrl, sandbox);
    } catch (e) {
      Cu.nukeSandbox(sandbox);
      lazy.logConsole.error(
        "Error loading wrapper script for Picture-in-Picture",
        e
      );
      return null;
    }

    this.#siteWrapper = new sandbox.PictureInPictureVideoWrapper(
      video
    ).wrappedJSObject;

    return sandbox;
  }

  #isBoolean(val) {
    return typeof val === "boolean";
  }

  #isNumber(val) {
    return typeof val === "number";
  }

  destroy() {
    if (this.#sandbox) {
      Cu.nukeSandbox(this.#sandbox);
    }
  }

  updatePiPTextTracks(text, type) {
    if (!this.#PictureInPictureChild.isSubtitlesEnabled && text) {
      this.#PictureInPictureChild.isSubtitlesEnabled = true;
      this.#PictureInPictureChild.sendAsyncMessage(
        "PictureInPicture:EnableSubtitlesButton"
      );
    }
    let pipWindowTracksContainer =
      this.#PictureInPictureChild.document.getElementById("texttracks");

    pipWindowTracksContainer.innerHTML = "";

    switch (type) {
      case "vtt":
      case "html": {
        const node = WebVTT.convertCueToDOMTree(
          this.#PictureInPictureChild,
          text
        );
        pipWindowTracksContainer.appendChild(node);
        break;
      }
      default:
        pipWindowTracksContainer.textContent = text;
        break;
    }
  }


  play(video) {
    return this.#callWrapperMethod({
      name: "play",
      args: [video],
      fallback: () => video.play(),
      validateRetVal: retVal => retVal == null,
    });
  }

  pause(video) {
    return this.#callWrapperMethod({
      name: "pause",
      args: [video],
      fallback: () => video.pause(),
      validateRetVal: retVal => retVal == null,
    });
  }

  getPaused(video) {
    return this.#callWrapperMethod({
      name: "getPaused",
      args: [video],
      fallback: () => video.paused,
      validateRetVal: retVal => this.#isBoolean(retVal),
    });
  }

  getEnded(video) {
    return this.#callWrapperMethod({
      name: "getEnded",
      args: [video],
      fallback: () => video.ended,
      validateRetVal: retVal => this.#isBoolean(retVal),
    });
  }

  getDuration(video) {
    return this.#callWrapperMethod({
      name: "getDuration",
      args: [video],
      fallback: () => video.duration,
      validateRetVal: retVal => this.#isNumber(retVal),
    });
  }

  getCurrentTime(video) {
    return this.#callWrapperMethod({
      name: "getCurrentTime",
      args: [video],
      fallback: () => video.currentTime,
      validateRetVal: retVal => this.#isNumber(retVal),
    });
  }

  setCurrentTime(video, position, wasPlaying) {
    return this.#callWrapperMethod({
      name: "setCurrentTime",
      args: [video, position, wasPlaying],
      fallback: () => {
        video.currentTime = position;
      },
      validateRetVal: retVal => retVal == null,
    });
  }

  timeFromSeconds(aSeconds) {
    aSeconds = isNaN(aSeconds) ? 0 : Math.round(aSeconds);
    let seconds = Math.floor(aSeconds % 60),
      minutes = Math.floor((aSeconds / 60) % 60),
      hours = Math.floor(aSeconds / 3600);
    seconds = seconds < 10 ? "0" + seconds : seconds;
    minutes = hours > 0 && minutes < 10 ? "0" + minutes : minutes;
    return aSeconds < 3600
      ? `${minutes}:${seconds}`
      : `${hours}:${minutes}:${seconds}`;
  }

  formatTimestamp(aCurrentTime, aDuration) {
    if (!Number.isFinite(aCurrentTime) || !Number.isFinite(aDuration)) {
      return undefined;
    }

    return `${this.timeFromSeconds(aCurrentTime)} / ${this.timeFromSeconds(
      aDuration
    )}`;
  }

  getVolume(video) {
    return this.#callWrapperMethod({
      name: "getVolume",
      args: [video],
      fallback: () => video.volume,
      validateRetVal: retVal => this.#isNumber(retVal),
    });
  }

  setVolume(video, volume) {
    return this.#callWrapperMethod({
      name: "setVolume",
      args: [video, volume],
      fallback: () => {
        video.volume = volume;
      },
      validateRetVal: retVal => retVal == null,
    });
  }

  isMuted(video) {
    return this.#callWrapperMethod({
      name: "isMuted",
      args: [video],
      fallback: () => video.muted,
      validateRetVal: retVal => this.#isBoolean(retVal),
    });
  }

  setMuted(video, shouldMute) {
    return this.#callWrapperMethod({
      name: "setMuted",
      args: [video, shouldMute],
      fallback: () => {
        video.muted = shouldMute;
      },
      validateRetVal: retVal => retVal == null,
    });
  }

  setCaptionContainerObserver(video, _callback) {
    return this.#callWrapperMethod({
      name: "setCaptionContainerObserver",
      args: [
        video,
        (text, type) => {
          this.updatePiPTextTracks(text, type);
        },
      ],
      fallback: () => {},
      validateRetVal: retVal => retVal == null,
    });
  }

  removeCaptionContainerObserver(video, _callback) {
    return this.#callWrapperMethod({
      name: "removeCaptionContainerObserver",
      args: [video],
      fallback: () => {},
      validateRetVal: retVal => retVal == null,
    });
  }

  shouldHideToggle(video) {
    return this.#callWrapperMethod({
      name: "shouldHideToggle",
      args: [video],
      fallback: () => false,
      validateRetVal: retVal => this.#isBoolean(retVal),
    });
  }

  isUrlbarToggleEligible(video) {
    return this.#callWrapperMethod({
      name: "isUrlbarToggleEligible",
      args: [video],
      fallback: () => true,
      validateRetVal: retVal => this.#isBoolean(retVal),
    });
  }

  isLive(video) {
    return this.#callWrapperMethod({
      name: "isLive",
      args: [video],
      fallback: () => video.duration === Infinity,
      validateRetVal: retVal => this.#isBoolean(retVal),
    });
  }
}

export function getActorFor(videoElement) {
  return gOriginatingVideoMap.get(videoElement);
}
