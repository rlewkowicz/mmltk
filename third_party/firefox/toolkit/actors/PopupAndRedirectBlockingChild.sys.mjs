/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

export class PopupAndRedirectBlockingChild extends JSWindowActorChild {

  #mWeakDocStates;

  constructor() {
    super();
    this.#mWeakDocStates = new WeakMap();
  }

  #getOrCreateDocState() {
    let state = this.#mWeakDocStates.get(this.document);
    if (!state) {
      state = {
        popups: [],
        redirect: null,
      };
      this.#mWeakDocStates.set(this.document, state);
    }

    return state;
  }

  receiveMessage(aMessage) {
    switch (aMessage.name) {
      case "GetBlockedPopups":
        return this.#getBlockedPopups();
      case "GetBlockedRedirect":
        return this.#getBlockedRedirect();
      case "UnblockPopup":
        this.#unblockPopup(aMessage);
        break;
    }

    return null;
  }

  #getBlockedPopups() {
    const state = this.#getOrCreateDocState();
    const length = Math.min(
      state.popups.length,
      PopupAndRedirectBlockingChild.maxReportedPopups
    );
    const result = [];

    for (let reportIndex = 0; reportIndex < length; ++reportIndex) {
      const popup = state.popups[reportIndex];
      const { popupWindowURISpec } = popup;
      result.push({
        popupWindowURISpec,
        reportIndex,
      });
    }

    return result;
  }

  #getBlockedRedirect() {
    const redirect = this.#getOrCreateDocState().redirect;
    if (!redirect) {
      return null;
    }

    return {
      redirectURISpec: redirect.redirectURISpec,
    };
  }

  #unblockPopup(aMessage) {
    const reportIndex = aMessage.data.reportIndex;
    const popup = this.#getOrCreateDocState().popups[reportIndex];

    if (popup?.requestingWindow?.document == popup.requestingDocument) {
      popup.requestingWindow.open(
        popup.popupWindowURISpec,
        popup.popupWindowName,
        popup.popupWindowFeatures
      );
    }
  }

  handleEvent(aEvent) {
    if (aEvent.target != this.document) {
      return;
    }

    switch (aEvent.type) {
      case "DOMPopupBlocked":
        this.#onPopupBlocked(aEvent);
        break;
      case "DOMRedirectBlocked":
        this.#onRedirectBlocked(aEvent);
        break;
    }
  }

  #onPopupBlocked(aEvent) {
    const state = this.#getOrCreateDocState();
    if (
      state.popups.length >= PopupAndRedirectBlockingChild.maxReportedPopups
    ) {
      return;
    }

    const popup = {
      popupWindowURISpec: aEvent.popupWindowURI?.spec ?? "about:blank",
      popupWindowFeatures: aEvent.popupWindowFeatures,
      popupWindowName: aEvent.popupWindowName,
      requestingWindow: aEvent.requestingWindow,
      requestingDocument: aEvent.requestingWindow.document,
    };
    state.popups.push(popup);

    this.#updateParentAboutBlockedPopups();
  }

  #onRedirectBlocked(aEvent) {
    const state = this.#getOrCreateDocState();
    if (state.redirect) {
      return;
    }

    const redirect = {
      redirectURISpec: aEvent.redirectURI.spec,
      requestingWindow: aEvent.requestingWindow,
      requestingDocument: aEvent.requestingWindow.document,
    };
    state.redirect = redirect;

    this.#updateParentAboutBlockedRedirect();
  }

  #updateParentAboutBlockedPopups() {
    this.sendAsyncMessage("UpdateBlockedPopups", {
      count: this.#getOrCreateDocState().popups.length,
    });
  }

  #updateParentAboutBlockedRedirect() {
    this.sendAsyncMessage("UpdateBlockedRedirect");
  }
}

XPCOMUtils.defineLazyPreferenceGetter(
  PopupAndRedirectBlockingChild,
  "maxReportedPopups",
  "privacy.popups.maxReported"
);
