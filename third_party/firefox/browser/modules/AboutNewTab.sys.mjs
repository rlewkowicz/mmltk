/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AboutNewTabResourceMapping:
    "resource:///modules/AboutNewTabResourceMapping.sys.mjs",
  ActivityStream: "resource://newtab/lib/ActivityStream.sys.mjs",
  NimbusFeatures: "resource://nimbus/ExperimentAPI.sys.mjs",
  ObjectUtils: "resource://gre/modules/ObjectUtils.sys.mjs",
  ProfileAge: "resource://gre/modules/ProfileAge.sys.mjs",
  TelemetryReportingPolicy:
    "resource://gre/modules/TelemetryReportingPolicy.sys.mjs",
});

const ABOUT_URL = "about:newtab";
const PREF_ACTIVITY_STREAM_DEBUG = "browser.newtabpage.activity-stream.debug";
const TOPIC_APP_QUIT = "profile-before-change";
const TRAINHOP_NIMBUS_FEATURE = "newtabTrainhop";

export const AboutNewTab = {
  QueryInterface: ChromeUtils.generateQI([
    "nsIObserver",
    "nsISupportsWeakReference",
  ]),

  initialized: false,

  willNotifyUser: false,

  _activityStreamEnabled: false,
  activityStream: null,
  activityStreamDebug: false,
  activityStreamPromise: null,
  _activityStreamResolver: null,

  _cachedTopSites: null,

  _newTabURL: ABOUT_URL,
  _newTabURLOverridden: false,

  init() {
    if (this.initialized) {
      return;
    }
    let { promise, resolve } = Promise.withResolvers();
    this.activityStreamPromise = promise;
    this._activityStreamResolver = resolve;

    Services.obs.addObserver(this, TOPIC_APP_QUIT);
    if (!AppConstants.RELEASE_OR_BETA) {
      XPCOMUtils.defineLazyPreferenceGetter(
        this,
        "activityStreamDebug",
        PREF_ACTIVITY_STREAM_DEBUG,
        false,
        () => {
          this.notifyChange();
        }
      );
    }

    XPCOMUtils.defineLazyPreferenceGetter(
      this,
      "privilegedAboutProcessEnabled",
      "browser.tabs.remote.separatePrivilegedContentProcess",
      false,
      () => {
        this.notifyChange();
      }
    );

    lazy.AboutNewTabResourceMapping.init();

    const AStelemetryPref = "browser.newtabpage.activity-stream.telemetry";
    if (
      Services.prefs.getPrefType(AStelemetryPref) ===
      Services.prefs.PREF_INVALID
    ) {
      Services.prefs
        .getDefaultBranch("")
        .setBoolPref(AStelemetryPref, AppConstants.MOZILLA_OFFICIAL);
    }

    this.toggleActivityStream(true);
    this.initialized = true;

    Services.obs.addObserver(
      this,
      lazy.TelemetryReportingPolicy.TELEMETRY_TOU_ACCEPTED_OR_INELIGIBLE
    );
  },

  toggleActivityStream(stateEnabled, forceState = false) {
    if (
      !forceState &&
      (this._newTabURLOverridden ||
        stateEnabled === this._activityStreamEnabled)
    ) {
      return false;
    }
    if (stateEnabled) {
      this._activityStreamEnabled = true;
    } else {
      this._activityStreamEnabled = false;
    }

    this._newTabURL = ABOUT_URL;
    return true;
  },

  get newTabURL() {
    return this._newTabURL;
  },

  set newTabURL(aNewTabURL) {
    let newTabURL = aNewTabURL.trim();
    if (newTabURL === ABOUT_URL) {
      this.resetNewTabURL();
      return;
    } else if (newTabURL === "") {
      newTabURL = "about:blank";
    }

    this.toggleActivityStream(false);
    this._newTabURL = newTabURL;
    this._newTabURLOverridden = true;
    this.notifyChange();
  },

  get newTabURLOverridden() {
    return this._newTabURLOverridden;
  },

  get activityStreamEnabled() {
    return this._activityStreamEnabled;
  },

  resetNewTabURL() {
    this._newTabURLOverridden = false;
    this._newTabURL = ABOUT_URL;
    this.toggleActivityStream(true, true);
    this.notifyChange();
  },

  notifyChange() {
    Services.obs.notifyObservers(null, "newtab-url-changed", this._newTabURL);
  },

  async onBrowserReady() {
    if (this.activityStream && this.activityStream.initialized) {
      return;
    }


    const nimbusFeature = lazy.NimbusFeatures[TRAINHOP_NIMBUS_FEATURE];
    const trainhopFeatureReady = nimbusFeature.ready();

    let redirector = Cc[
      "@mozilla.org/network/protocol/about;1?what=newtab"
    ].getService(Ci.nsIAboutModule).wrappedJSObject;

    const addonInitted = redirector.promiseBuiltInAddonInitialized;

    const profileCreatedAccessorReady = lazy
      .ProfileAge()
      .then(accessor => accessor.created);

    const [createdTimestamp] = await Promise.all([
      profileCreatedAccessorReady,
      trainhopFeatureReady,
      addonInitted,
    ]);
    const createdInstant = createdTimestamp
      ? Temporal.Instant.fromEpochMilliseconds(createdTimestamp)
      : null;

    lazy.AboutNewTabResourceMapping.scheduleUpdateTrainhopAddonState();

    try {
      this.activityStream = new lazy.ActivityStream(createdInstant);
      this._activityStreamResolver();
    } catch (error) {
      console.error(error);
      throw error;
    }

    try {
      this.activityStream.init();
      this._subscribeToActivityStream();
    } catch (e) {
      console.error(e);
    }
  },

  _subscribeToActivityStream() {
    const store = this.activityStream.store;
    let unsubscribe = store.subscribe(() => {
      let topSites = store.getState().TopSites.rows.map(site => {
        site = { ...site };
        delete site.screenshot;
        return site;
      });
      if (!lazy.ObjectUtils.deepEqual(topSites, this._cachedTopSites)) {
        this._cachedTopSites = topSites;
        Services.obs.notifyObservers(null, "newtab-top-sites-changed");
      }
    });
    this._unsubscribeFromActivityStream = () => {
      try {
        unsubscribe();
      } catch (e) {
        console.error(e);
      }
    };
  },

  uninit() {
    if (this.activityStream) {
      this._unsubscribeFromActivityStream?.();
      this.activityStream.uninit();
      this.activityStream = null;
    }
    try {
      Services.obs.removeObserver(this, TOPIC_APP_QUIT);
      Services.obs.removeObserver(
        this,
        lazy.TelemetryReportingPolicy.TELEMETRY_TOU_ACCEPTED_OR_INELIGIBLE
      );
    } catch (e) {
    }

    this.initialized = false;
  },

  getTopSites() {
    return this.activityStream
      ? this.activityStream.store.getState().TopSites.rows
      : [];
  },

  _alreadyRecordedTopsitesPainted: false,
  _nonDefaultStartup: false,

  noteNonDefaultStartup() {
    this._nonDefaultStartup = true;
  },

  maybeRecordTopsitesPainted() {
    if (this._alreadyRecordedTopsitesPainted || this._nonDefaultStartup) {
      return;
    }

    this._alreadyRecordedTopsitesPainted = true;
  },


  observe(subject, topic) {
    switch (topic) {
      case TOPIC_APP_QUIT: {
        this.uninit();
        break;
      }
      case lazy.TelemetryReportingPolicy.TELEMETRY_TOU_ACCEPTED_OR_INELIGIBLE: {
        Services.obs.removeObserver(
          this,
          lazy.TelemetryReportingPolicy.TELEMETRY_TOU_ACCEPTED_OR_INELIGIBLE
        );

        Services.tm.dispatchToMainThread(() => this.onBrowserReady());
        break;
      }
    }
  },
};
