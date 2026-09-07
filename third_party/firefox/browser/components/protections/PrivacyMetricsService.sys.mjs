/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "TrackingDBService",
  "@mozilla.org/tracking-db-service;1",
  Ci.nsITrackingDBService
);


export const PrivacyMetricsService = {
  async getWeeklyStats() {
    const todayInMs = Date.now();
    const weekAgoInMs = todayInMs - 7 * 24 * 60 * 60 * 1000;

    const eventRows = await lazy.TrackingDBService.getEventsByDateRange(
      weekAgoInMs,
      todayInMs
    );

    return this._aggregateStats(eventRows);
  },

  async getTodayStats() {
    const todayInMs = Date.now();

    const eventRows = await lazy.TrackingDBService.getEventsByDateRange(
      todayInMs,
      todayInMs
    );

    return this._aggregateStats(eventRows);
  },

  _aggregateStats(eventRows) {
    let trackers = 0;
    let cookies = 0;
    let fingerprinters = 0;
    let cryptominers = 0;
    let socialTrackers = 0;

    for (let row of eventRows) {
      const count = row.getResultByName("count");
      const type = row.getResultByName("type");

      switch (type) {
        case Ci.nsITrackingDBService.TRACKERS_ID:
          trackers += count;
          break;
        case Ci.nsITrackingDBService.TRACKING_COOKIES_ID:
          cookies += count;
          break;
        case Ci.nsITrackingDBService.FINGERPRINTERS_ID:
        case Ci.nsITrackingDBService.SUSPICIOUS_FINGERPRINTERS_ID:
          fingerprinters += count;
          break;
        case Ci.nsITrackingDBService.CRYPTOMINERS_ID:
          cryptominers += count;
          break;
        case Ci.nsITrackingDBService.SOCIAL_ID:
          socialTrackers += count;
          break;
      }
    }

    const total =
      trackers + cookies + cryptominers + fingerprinters + socialTrackers;

    return {
      total,
      trackers,
      cookies,
      fingerprinters,
      cryptominers,
      socialTrackers,
      lastUpdated: Date.now(),
    };
  },
};
