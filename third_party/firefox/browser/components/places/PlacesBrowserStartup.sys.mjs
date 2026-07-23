/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
});

export let PlacesBrowserStartup = {
  _placesInitialized: false,
  _placesBrowserInitComplete: false,

  onFirstWindowReady(window) {
    lazy.PlacesUtils.favicons.setDefaultIconURIPreferredSize(
      16 * window.devicePixelRatio
    );
  },

  backendInitComplete() {
    if (this._placesInitialized) {
      throw new Error("Cannot initialize Places more than once");
    }
    this._placesInitialized = true;

    void lazy.PlacesUtils.history.databaseStatus;

    this._placesBrowserInitComplete = true;
    Services.obs.notifyObservers(null, "places-browser-init-complete");
  },

  notifyIfInitializationComplete() {
    if (this._placesBrowserInitComplete) {
      Services.obs.notifyObservers(null, "places-browser-init-complete");
    }
  },
};
