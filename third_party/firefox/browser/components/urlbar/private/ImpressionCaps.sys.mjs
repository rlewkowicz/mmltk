/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { SuggestFeature } from "moz-src:///browser/components/urlbar/private/SuggestFeature.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AsyncShutdown: "resource://gre/modules/AsyncShutdown.sys.mjs",
  QuickSuggest: "moz-src:///browser/components/urlbar/QuickSuggest.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  clearInterval: "resource://gre/modules/Timer.sys.mjs",
  setInterval: "resource://gre/modules/Timer.sys.mjs",
});

const IMPRESSION_COUNTERS_RESET_INTERVAL_MS = 60 * 60 * 1000; 

export class ImpressionCaps extends SuggestFeature {
  constructor() {
    super();
    lazy.UrlbarPrefs.addObserver(this);
  }

  get enablingPreferences() {
    return [
      "quickSuggestImpressionCapsSponsoredEnabled",
      "quickSuggestImpressionCapsNonSponsoredEnabled",
    ];
  }

  enable(enabled) {
    if (enabled) {
      this.#init();
    } else {
      this.#uninit();
    }
  }

  updateStats(type) {
    this.logger.debug("Starting impression stats update", {
      type,
      currentStats: this.#stats,
      impression_caps: lazy.QuickSuggest.config.impression_caps,
    });

    let isSponsored = type == "sponsored";
    if (
      (isSponsored &&
        !lazy.UrlbarPrefs.get("quickSuggestImpressionCapsSponsoredEnabled")) ||
      (!isSponsored &&
        !lazy.UrlbarPrefs.get("quickSuggestImpressionCapsNonSponsoredEnabled"))
    ) {
      this.logger.debug("Impression caps disabled, skipping update");
      return;
    }

    let stats = this.#stats[type];
    if (!stats) {
      this.logger.debug("Impression caps undefined, skipping update");
      return;
    }

    for (let stat of stats) {
      stat.count++;
      stat.impressionDateMs = Date.now();

      if (stat.count == stat.maxCount) {
        this.logger.debug("Impression cap hit", { type, hitStat: stat });
      }
    }

    this.#updatingStats = true;
    try {
      lazy.UrlbarPrefs.set(
        "quicksuggest.impressionCaps.stats",
        JSON.stringify(this.#stats)
      );
    } finally {
      this.#updatingStats = false;
    }

    this.logger.debug("Finished impression stats update", {
      newStats: this.#stats,
    });
  }

  getHitStats(type) {
    this.#resetElapsedCounters();
    let stats = this.#stats[type];
    if (stats) {
      let hitStats = stats.filter(s => s.maxCount <= s.count);
      if (hitStats.length) {
        return hitStats;
      }
    }
    return null;
  }

  onPrefChanged(pref) {
    switch (pref) {
      case "quicksuggest.impressionCaps.stats":
        if (!this.#updatingStats) {
          this.logger.debug(
            "browser.urlbar.quicksuggest.impressionCaps.stats changed"
          );
          this.#loadStats();
        }
        break;
    }
  }

  #init() {
    this.#loadStats();

    this._onConfigSet = () => this.#validateStats();

    this.#setCountersResetInterval();

    this._shutdownBlocker = () => this.#resetElapsedCounters();
    lazy.AsyncShutdown.profileChangeTeardown.addBlocker(
      "QuickSuggest: Record impression counters reset telemetry",
      this._shutdownBlocker
    );
  }

  #uninit() {
    this._onConfigSet = null;

    lazy.clearInterval(this._impressionCountersResetInterval);
    this._impressionCountersResetInterval = 0;

    lazy.AsyncShutdown.profileChangeTeardown.removeBlocker(
      this._shutdownBlocker
    );
    this._shutdownBlocker = null;
  }

  #loadStats() {
    let json = lazy.UrlbarPrefs.get("quicksuggest.impressionCaps.stats");
    if (!json) {
      this.#stats = {};
    } else {
      try {
        this.#stats = JSON.parse(
          json,
          (key, value) =>
            key == "intervalSeconds" && value === null ? Infinity : value
        );
      } catch (error) {}
    }
    this.#validateStats();
  }

  #validateStats() {
    let { impression_caps } = lazy.QuickSuggest.config;

    this.logger.debug("Validating impression stats", {
      impression_caps,
      currentStats: this.#stats,
    });

    if (!this.#stats || typeof this.#stats != "object") {
      this.#stats = {};
    }

    for (let [type, cap] of Object.entries(impression_caps || {})) {
      let maxCapCounts = (cap.custom || []).reduce(
        (map, { interval_s, max_count }) => {
          map.set(interval_s, max_count);
          return map;
        },
        new Map()
      );
      if (typeof cap.lifetime == "number") {
        maxCapCounts.set(Infinity, cap.lifetime);
      }

      let stats = this.#stats[type];
      if (!Array.isArray(stats)) {
        stats = [];
        this.#stats[type] = stats;
      }

      let orphanStats = [];
      let maxCountInStats = 0;
      for (let i = 0; i < stats.length; ) {
        let stat = stats[i];
        if (
          typeof stat.intervalSeconds != "number" ||
          typeof stat.startDateMs != "number" ||
          typeof stat.count != "number" ||
          typeof stat.maxCount != "number" ||
          typeof stat.impressionDateMs != "number"
        ) {
          stats.splice(i, 1);
        } else {
          maxCountInStats = Math.max(maxCountInStats, stat.count);
          let maxCount = maxCapCounts.get(stat.intervalSeconds);
          if (maxCount === undefined) {
            stats.splice(i, 1);
            orphanStats.push(stat);
          } else {
            stat.maxCount = maxCount;
            i++;
          }
        }
      }

      for (let [intervalSeconds, maxCount] of maxCapCounts.entries()) {
        if (!stats.some(s => s.intervalSeconds == intervalSeconds)) {
          stats.push({
            maxCount,
            intervalSeconds,
            startDateMs: Date.now(),
            count: 0,
            impressionDateMs: 0,
          });
        }
      }

      for (let orphan of orphanStats) {
        for (let stat of stats) {
          if (orphan.intervalSeconds <= stat.intervalSeconds) {
            stat.count = Math.max(stat.count, orphan.count);
            stat.startDateMs = Math.min(stat.startDateMs, orphan.startDateMs);
            stat.impressionDateMs = Math.max(
              stat.impressionDateMs,
              orphan.impressionDateMs
            );
          }
        }
      }

      let lifetimeStat = stats.find(s => s.intervalSeconds == Infinity);
      if (lifetimeStat) {
        lifetimeStat.count = maxCountInStats;
      }

      stats.sort((a, b) => a.intervalSeconds - b.intervalSeconds);
    }

    this.logger.debug("Finished validating impression stats", {
      newStats: this.#stats,
    });
  }

  #resetElapsedCounters() {
    this.logger.debug("Checking for elapsed impression cap intervals", {
      currentStats: this.#stats,
      impression_caps: lazy.QuickSuggest.config.impression_caps,
    });

    let now = Date.now();
    for (let [type, stats] of Object.entries(this.#stats)) {
      for (let stat of stats) {
        let elapsedMs = now - stat.startDateMs;
        let intervalMs = 1000 * stat.intervalSeconds;
        let elapsedIntervalCount = Math.floor(elapsedMs / intervalMs);
        if (elapsedIntervalCount) {
          this.logger.debug("Resetting impression counter", {
            type,
            stat,
            elapsedMs,
            elapsedIntervalCount,
            intervalSecs: stat.intervalSeconds,
          });

          let newStartDateMs =
            stat.startDateMs + elapsedIntervalCount * intervalMs;

          stat.startDateMs = newStartDateMs;
          stat.count = 0;
        }
      }
    }

    this.logger.debug("Finished checking elapsed impression cap intervals", {
      newStats: this.#stats,
    });
  }

  #setCountersResetInterval(ms = IMPRESSION_COUNTERS_RESET_INTERVAL_MS) {
    if (this._impressionCountersResetInterval) {
      lazy.clearInterval(this._impressionCountersResetInterval);
    }
    this._impressionCountersResetInterval = lazy.setInterval(
      () => this.#resetElapsedCounters(),
      ms
    );
  }

  _getStartupDateMs() {
    return Services.startup.getStartupInfo().process.getTime();
  }

  get _test_stats() {
    return this.#stats;
  }

  _test_reloadStats() {
    this.#stats = null;
    this.#loadStats();
  }

  _test_resetElapsedCounters() {
    this.#resetElapsedCounters();
  }

  _test_setCountersResetInterval(ms) {
    this.#setCountersResetInterval(ms);
  }

  #stats = {};

  #updatingStats = false;
}
