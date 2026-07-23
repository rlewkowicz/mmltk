/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  AsyncShutdown: "resource://gre/modules/AsyncShutdown.sys.mjs",
  DeferredTask: "resource://gre/modules/DeferredTask.sys.mjs",
  ObjectUtils: "resource://gre/modules/ObjectUtils.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "logger", function () {
  return lazy.PlacesUtils.getLogger({ prefix: "FrecencyRecalculator" });
});

const MILLIS_PER_DAY = 86400000;

const FRECENCY_DECAYRATE = "0.975";
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "frecencyDecayRate",
  "places.frecency.decayRate",
  FRECENCY_DECAYRATE,
  null,
  val => {
    if (typeof val == "string") {
      val = parseFloat(val);
    }
    if (val > 1.0) {
      lazy.logger.error("Invalid frecency decay rate value: " + val);
      val = parseFloat(FRECENCY_DECAYRATE);
    }
    return val;
  }
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "adaptiveHistoryExpireDays",
  "places.adaptiveHistory.expireDays",
  90
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "originsFrecencyCutOffDays",
  "places.frecency.originsCutOffDays",
  90
);

const PREF_ACCELERATE_RECALCULATION = "places.frecency.accelerateRecalculation";
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "accelerationRate",
  PREF_ACCELERATE_RECALCULATION,
  false,
  null,
  accelerate => (accelerate ? 2 : 1)
);

const DEFERRED_TASK_INTERVAL_MS = 2 * 60000;
const DEFERRED_TASK_MAX_IDLE_WAIT_MS = 5 * 60000;
const DEFAULT_CHUNK_SIZE = 50;
const ACCELERATION_EVENTS_THRESHOLD = 250;
const BUCKETS = 2;

export class PlacesFrecencyRecalculator {
  classID = Components.ID("1141fd31-4c1a-48eb-8f1a-2f05fad94085");

  #task = null;

  #alternativeFrecencyHelper = null;

  #finalized = false;

  #pausedForTesting = false;

  get alternativeFrecencyInfo() {
    return this.#alternativeFrecencyHelper?.sets;
  }

  constructor() {
    lazy.logger.trace("Initializing Frecency Recalculator");

    this.QueryInterface = ChromeUtils.generateQI([
      "nsIObserver",
      "nsISupportsWeakReference",
    ]);

    if (
      Services.startup.isInOrBeyondShutdownPhase(
        Ci.nsIAppStartup.SHUTDOWN_PHASE_APPSHUTDOWNCONFIRMED
      )
    ) {
      this.#finalized = true;
      return;
    }

    this.#createOrUpdateTask();

    lazy.AsyncShutdown.appShutdownConfirmed.addBlocker(
      "PlacesFrecencyRecalculator: shutdown",
      () => this.#finalize()
    );

    this.wrappedJSObject = this;
    this.pendingFrecencyDecayPromise = Promise.resolve();
    this.pendingOriginsDecayPromise = Promise.resolve();

    Services.obs.addObserver(this, "idle-daily", true);
    Services.obs.addObserver(this, "frecency-recalculation-needed", true);

    this.#alternativeFrecencyHelper = new AlternativeFrecencyHelper(this);

    lazy.PlacesUtils.history.shouldStartFrecencyRecalculation = true;
    this.maybeStartFrecencyRecalculation();
  }

  #createOrUpdateTask() {
    if (this.#finalized) {
      lazy.logger.trace(`Not resurrecting #task because finalized`);
      return;
    }
    let wasArmed = this.#task?.isArmed;
    if (this.#task) {
      this.#task.disarm();
      this.#task.finalize().catch(console.error);
    }
    this.#task = new lazy.DeferredTask(
      this.#taskFn.bind(this),
      DEFERRED_TASK_INTERVAL_MS / lazy.accelerationRate,
      DEFERRED_TASK_MAX_IDLE_WAIT_MS / lazy.accelerationRate
    );
    if (wasArmed) {
      this.#task.arm();
    }
  }

  async #taskFn() {
    if (this.#task.isFinalized || this.#pausedForTesting) {
      return;
    }
    try {
      if (await this.recalculateSomeFrecencies()) {
      } else {
      }
    } catch (ex) {
      console.error(ex);
      lazy.logger.error(ex);
    }
  }

  #finalize() {
    lazy.logger.trace("Finalizing frecency recalculator");
    this.#task.disarm();
    this.#task.finalize().catch(console.error);
    this.#finalized = true;
  }

  #lastEventsCount = 0;

  maybeUpdateRecalculationSpeed() {
    if (lazy.accelerationRate > 1) {
      return true;
    }
    let eventsCount =
      PlacesObservers.counts.get("page-visited") +
      PlacesObservers.counts.get("bookmark-added");
    let accelerate =
      eventsCount - this.#lastEventsCount > ACCELERATION_EVENTS_THRESHOLD;
    if (accelerate) {
      Services.prefs.setBoolPref(PREF_ACCELERATE_RECALCULATION, true);
      this.#createOrUpdateTask();
    }
    this.#lastEventsCount = eventsCount;
    return accelerate;
  }

  #resetRecalculationSpeed() {
    if (lazy.accelerationRate > 1) {
      Services.prefs.clearUserPref(PREF_ACCELERATE_RECALCULATION);
      this.#createOrUpdateTask();
    }
  }

  async recalculateSomeFrecencies({ chunkSize = DEFAULT_CHUNK_SIZE } = {}) {
    lazy.logger.trace(
      `Recalculate ${chunkSize >= 0 ? chunkSize : "infinite"} frecency values`
    );
    let affectedCount = 0;
    let hasRecalculatedAnything = false;
    let db = await lazy.PlacesUtils.promiseUnsafeWritableDBConnection();
    let affected = await db.executeCached(
      `UPDATE moz_places
      SET frecency = CALCULATE_FRECENCY(id)
      WHERE id IN (
        SELECT id FROM moz_places
        WHERE recalc_frecency = 1
        ORDER BY frecency DESC, visit_count DESC
        LIMIT ${chunkSize}
      )
      RETURNING id`
    );
    affectedCount += affected.length;
    let shouldRestartRecalculation = affectedCount >= chunkSize;
    hasRecalculatedAnything = affectedCount > 0;
    if (hasRecalculatedAnything) {
      PlacesObservers.notifyListeners([new PlacesRanking()]);
    }

    affectedCount = await this.#recalculateSomeOriginsFrecencies({
      chunkSize,
    });
    shouldRestartRecalculation ||= affectedCount >= chunkSize;
    hasRecalculatedAnything ||= affectedCount > 0;

    affectedCount =
      await this.#alternativeFrecencyHelper.recalculateSomeAlternativeFrecencies(
        { chunkSize }
      );
    shouldRestartRecalculation ||= affectedCount >= chunkSize;
    hasRecalculatedAnything ||= affectedCount > 0;

    if (chunkSize > 0 && shouldRestartRecalculation) {
      this.maybeUpdateRecalculationSpeed();
      this.#task.arm();
    } else {
      this.#resetRecalculationSpeed();
      lazy.PlacesUtils.history.shouldStartFrecencyRecalculation = false;
      this.#task.disarm();
    }
    return hasRecalculatedAnything;
  }

  async #recalculateSomeOriginsFrecencies({ chunkSize }) {
    lazy.logger.trace(`Recalculate ${chunkSize} origins frecency values`);


    let affectedCount = 0;
    let db = await lazy.PlacesUtils.promiseUnsafeWritableDBConnection();
    let affected = await db.executeCached(
      `
      UPDATE moz_origins
      SET frecency = IFNULL((
        SELECT CAST(
          AVG(h.frecency) * COUNT(
            DISTINCT date(v.visit_date / 1000000, 'unixepoch')
          ) AS INTEGER
        )
        FROM moz_places h
        JOIN moz_historyvisits v ON v.place_id = h.id
        WHERE h.origin_id = moz_origins.id
          AND v.visit_type = ${lazy.PlacesUtils.history.TRANSITION_TYPED}
          AND v.visit_date >
            strftime('%s','now','localtime','start of day',
                     '-${lazy.originsFrecencyCutOffDays} day','utc') * 1000000
      ), 1.0),
      recalc_frecency = 0
      WHERE id IN (
        SELECT id FROM moz_origins
        WHERE recalc_frecency = 1
        ORDER BY frecency DESC
        LIMIT ${chunkSize}
      )
      RETURNING id`
    );
    affectedCount += affected.length;

    let threshold = (
      await db.executeCached(
        `
        WITH ntiled AS (
          SELECT
            host,
            frecency,
            NTILE(:buckets) OVER (ORDER BY frecency ASC) AS ntile
            FROM moz_origins
            WHERE frecency > 1)
        SELECT MAX(frecency)
        FROM ntiled
        WHERE ntile = 1
      `,
        { buckets: BUCKETS }
      )
    )[0].getResultByIndex(0);
    await lazy.PlacesUtils.metadata.set(
      "origin_frecency_threshold",
      threshold ?? 2
    );

    return affectedCount;
  }

  async recalculateAnyOutdatedFrecencies() {
    this.#task.disarm();
    return this.recalculateSomeFrecencies({ chunkSize: -1 });
  }

  get isRecalculationPending() {
    return this.#task.isArmed;
  }

  maybeStartFrecencyRecalculation() {
    if (
      lazy.PlacesUtils.history.shouldStartFrecencyRecalculation &&
      !this.#task.isFinalized
    ) {
      lazy.logger.trace("Arm frecency recalculation");
      this.#task.arm();
    }
  }

  async decay() {
    lazy.logger.trace("Decay adaptive history.");
    try {
      let db = await lazy.PlacesUtils.promiseUnsafeWritableDBConnection();
      await db.executeCached(
        `UPDATE moz_inputhistory SET use_count = use_count * :decay_rate`,
        { decay_rate: lazy.frecencyDecayRate }
      );
      await db.executeCached(
        `DELETE FROM moz_inputhistory WHERE use_count < :use_count`,
        {
          use_count: Math.pow(
            lazy.frecencyDecayRate,
            lazy.adaptiveHistoryExpireDays
          ),
        }
      );

    } catch (ex) {
      console.error(ex);
      lazy.logger.error(ex);
    }
  }

  async requestRecalcOfNotRecentlyVisitedOrigins() {
    const now = Date.now();
    const key = "origins_frecency_last_decay_timestamp";
    let lastRecalcTime = await lazy.PlacesUtils.metadata.get(key, now);
    if (lastRecalcTime > now - 7 * MILLIS_PER_DAY) {
      lazy.logger.trace("Skipping as not enough time passed");
      return;
    }
    await lazy.PlacesUtils.metadata.set(key, now);

    let threshold = await lazy.PlacesUtils.metadata.get(
      "origin_frecency_threshold",
      0
    );
    if (threshold < 100) {
      lazy.logger.trace("Skipping as threshold too low");
      return;
    }

    lazy.logger.trace("Recalculate origins not recently visited");
    let db = await lazy.PlacesUtils.promiseUnsafeWritableDBConnection();
    await db.execute(
      `
      UPDATE moz_origins
      SET recalc_frecency = 1, recalc_alt_frecency = 1
      WHERE id IN (
        SELECT id
        FROM moz_origins
        WHERE frecency >= :threshold
        AND recalc_frecency = 0
        AND NOT EXISTS (
          SELECT 1 FROM moz_places
	        WHERE origin_id = moz_origins.id
	          AND (
              foreign_count > 0 OR
              last_visit_date > strftime('%s', 'now', '-' || :cutoff || ' days') * 1000000
            )
        )
      )
    `,
      { threshold, cutoff: lazy.originsFrecencyCutOffDays }
    );
  }

  observe(subject, topic) {
    lazy.logger.trace(`Got ${topic} topic`);
    if (this.#finalized) {
      lazy.logger.trace(`Ignoring topic because finalized`);
      return;
    }
    switch (topic) {
      case "idle-daily":
        this.pendingFrecencyDecayPromise = this.decay();
        lazy.logger.trace("Frecency recalculation on idle");
        lazy.PlacesUtils.history.shouldStartFrecencyRecalculation = true;
        this.maybeStartFrecencyRecalculation();
        this.pendingOriginsDecayPromise =
          this.requestRecalcOfNotRecentlyVisitedOrigins();
        return;
      case "frecency-recalculation-needed":
        lazy.logger.trace("Frecency recalculation requested");
        this.maybeUpdateRecalculationSpeed();
        this.maybeStartFrecencyRecalculation();
        return;
      case "test-execute-taskFn":
        subject.promise = this.#taskFn();
        return;
      case "test-pause-frecency-recalculation":
        this.#pausedForTesting = true;
        return;
      case "test-resume-frecency-recalculation":
        this.#pausedForTesting = false;
        return;
      case "test-alternative-frecency-init":
        this.#alternativeFrecencyHelper = new AlternativeFrecencyHelper(this);
        subject.promise =
          this.#alternativeFrecencyHelper.initializedDeferred.promise;
    }
  }
}

class AlternativeFrecencyHelper {
  initializedDeferred = Promise.withResolvers();
  #recalculator = null;

  sets = {
    pages: {
      enabled: lazy.PlacesUtils.history.isAlternativeFrecencyEnabled,
      metadataKey: "page_alternative_frecency",
      table: "moz_places",
      variables: {
        version: 3,
        veryHighWeight: Services.prefs.getIntPref(
          "places.frecency.pages.alternative.veryHighWeight",
          200
        ),
        highWeight: Services.prefs.getIntPref(
          "places.frecency.pages.alternative.highWeight",
          100
        ),
        mediumWeight: Services.prefs.getIntPref(
          "places.frecency.pages.alternative.mediumWeight",
          50
        ),
        lowWeight: Services.prefs.getIntPref(
          "places.frecency.pages.alternative.lowWeight",
          20
        ),
        halfLifeDays: Services.prefs.getIntPref(
          "places.frecency.pages.alternative.halfLifeDays",
          30
        ),
        numSampledVisits: Services.prefs.getIntPref(
          "places.frecency.pages.alternative.numSampledVisits",
          10
        ),
      },
      method: this.#recalculateSomePagesAlternativeFrecencies,
    },

    origins: {
      enabled: Services.prefs.getBoolPref(
        "places.frecency.origins.alternative.featureGate",
        false
      ),
      metadataKey: "origin_alternative_frecency",
      table: "moz_origins",
      variables: {
        version: 2,
        daysCutOff: Services.prefs.getIntPref(
          "places.frecency.origins.alternative.daysCutOff",
          90
        ),
      },
      method: this.#recalculateSomeOriginsAlternativeFrecencies,
    },
  };

  constructor(recalculator) {
    this.#recalculator = recalculator;
    this.#kickOffAlternativeFrecencies()
      .catch(console.error)
      .finally(() => this.initializedDeferred.resolve());
  }

  async #kickOffAlternativeFrecencies() {
    let recalculateFirstChunk = false;
    for (let [type, set] of Object.entries(this.sets)) {
      let storedVariables = await lazy.PlacesUtils.metadata.get(
        set.metadataKey,
        null
      );

      if (
        set.enabled &&
        !lazy.ObjectUtils.deepEqual(set.variables, storedVariables)
      ) {
        lazy.logger.trace(
          `Alternative frecency of ${type} must be recalculated`
        );
        await lazy.PlacesUtils.withConnectionWrapper(
          `PlacesFrecencyRecalculator :: ${type} alternative frecency set recalc`,
          async db => {
            await db.execute(
              `UPDATE ${set.table}
               SET alt_frecency = CASE WHEN frecency = 0 THEN 0 ELSE -1 END,
               recalc_alt_frecency = CASE WHEN frecency = 0 THEN 0 ELSE 1 END
               WHERE alt_frecency IS NULL`
            );
          }
        );
        await lazy.PlacesUtils.metadata.set(set.metadataKey, set.variables);
        recalculateFirstChunk = true;
        continue;
      }

      if (!set.enabled && storedVariables) {
        lazy.logger.trace(`Clean up alternative frecency of ${type}`);
        await lazy.PlacesUtils.withConnectionWrapper(
          `PlacesFrecencyRecalculator :: ${type} alternative frecency set NULL`,
          async db => {
            await db.execute(`UPDATE ${set.table} SET alt_frecency = NULL`);
          }
        );
        await lazy.PlacesUtils.metadata.delete(set.metadataKey);
      }
    }

    if (recalculateFirstChunk) {
      await this.recalculateSomeAlternativeFrecencies();

      lazy.PlacesUtils.history.shouldStartFrecencyRecalculation = true;
      this.#recalculator.maybeStartFrecencyRecalculation();
    }
  }

  async recalculateSomeAlternativeFrecencies({
    chunkSize = DEFAULT_CHUNK_SIZE,
  } = {}) {
    let affected = 0;
    for (let set of Object.values(this.sets)) {
      if (!set.enabled) {
        continue;
      }
      try {
        affected += await set.method({ chunkSize, variables: set.variables });
      } catch (ex) {
        console.error(ex);
      }
    }
    return affected;
  }

  async #recalculateSomePagesAlternativeFrecencies({ chunkSize }) {
    lazy.logger.trace(
      `Recalculate ${chunkSize * 2} alternative pages frecency values`
    );
    let db = await lazy.PlacesUtils.promiseUnsafeWritableDBConnection();
    let affected = await db.executeCached(
      `UPDATE moz_places
       SET alt_frecency = CALCULATE_ALT_FRECENCY(moz_places.id),
           recalc_alt_frecency = 0
       WHERE id IN (
        SELECT id FROM moz_places
          WHERE recalc_alt_frecency = 1
          ORDER BY frecency DESC
          LIMIT ${chunkSize * 2}
      )
      RETURNING id`
    );
    return affected;
  }

  async #recalculateSomeOriginsAlternativeFrecencies({ chunkSize, variables }) {
    lazy.logger.trace(
      `Recalculate ${chunkSize} alternative origins frecency values`
    );


    let affectedCount = 0;
    let db = await lazy.PlacesUtils.promiseUnsafeWritableDBConnection();
    let affected = await db.executeCached(
      `
      UPDATE moz_origins
      SET alt_frecency = (
        SELECT sum(frecency)
        FROM moz_places h
        WHERE origin_id = moz_origins.id
        AND last_visit_date >
          strftime('%s','now','localtime','start of day',
                   '-${variables.daysCutOff} day','utc') * 1000000
      ), recalc_alt_frecency = 0
      WHERE id IN (
        SELECT id FROM moz_origins
        WHERE recalc_alt_frecency = 1
        ORDER BY frecency DESC
        LIMIT ${chunkSize}
      )
      RETURNING id`
    );
    affectedCount += affected.length;

    if (affected.length) {
      let threshold = (
        await db.executeCached(`SELECT avg(alt_frecency) FROM moz_origins`)
      )[0].getResultByIndex(0);
      await lazy.PlacesUtils.metadata.set(
        "origin_alt_frecency_threshold",
        threshold
      );
    }

    return affectedCount;
  }
}
