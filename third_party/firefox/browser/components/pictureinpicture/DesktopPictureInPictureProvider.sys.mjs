/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { getActorFor } from "resource://gre/actors/PictureInPictureChild.sys.mjs";

class PictureInPictureFunctionsImpl {
  QueryInterface = ChromeUtils.generateQI(["nsIMediaPictureInPictureProvider"]);

  #getActor(videoElement, actorType) {
    if (!videoElement) {
      throw Components.Exception(
        "Invalid video element",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    const docShell = videoElement.documentGlobal.docShell;
    const windowGlobalChild = docShell.domWindow.windowGlobalChild;

    if (!windowGlobalChild) {
      throw Components.Exception(
        "No WindowGlobalChild available",
        Cr.NS_ERROR_FAILURE
      );
    }

    const actor = windowGlobalChild.getActor(actorType);
    if (!actor) {
      throw Components.Exception(
        `${actorType} actor not found`,
        Cr.NS_ERROR_FAILURE
      );
    }
    return actor;
  }

  async openMediaPictureInPictureWindow(videoElement, pictureInPictureWindow) {
    if (!pictureInPictureWindow) {
      throw Components.Exception(
        "Invalid PictureInPictureWindow argument",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    const actor = this.#getActor(videoElement, "PictureInPictureLauncher");
    if (videoElement.isCloningElementVisually) {
      return;
    }
    await actor.togglePictureInPicture({
      video: videoElement,
      reason: "Api",
      pictureInPictureWindow,
      eventExtraKeys: {},
    });

    if (!videoElement.isCloningElementVisually) {
      throw Components.Exception(
        "Video is not cloning.",
        Cr.NS_ERROR_INVALID_ARG
      );
    }
  }

  closeMediaPictureInPictureWindow(videoElement) {
    if (!videoElement) {
      throw Components.Exception(
        "Invalid PictureInPictureWindow argument",
        Cr.NS_ERROR_INVALID_ARG
      );
    }

    if (!videoElement.isCloningElementVisually) {
      return Promise.resolve();
    }

    const actor = getActorFor(videoElement);
    if (!actor) {
      throw Components.Exception(
        "No actor found available",
        Cr.NS_ERROR_FAILURE
      );
    }
    return actor.closePictureInPicture({ reason: "Api" });
  }
}

export function PictureInPictureProvider() {
  return new PictureInPictureFunctionsImpl();
}
