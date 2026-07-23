/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  GeolocationUtils:
    "moz-src:///browser/components/urlbar/private/GeolocationUtils.sys.mjs",
  ObliviousHTTP: "resource://gre/modules/ObliviousHTTP.sys.mjs",
  SkippableTimer: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});



const SEARCH_PARAMS = Object.freeze({
  CLIENT_VARIANTS: "client_variants",
  PROVIDERS: "providers",
  QUERY: "q",
  SEQUENCE_NUMBER: "seq",
  SESSION_ID: "sid",
});

const SESSION_TIMEOUT_MS = 5 * 60 * 1000; 

export class MerinoClient {
  #lazy = XPCOMUtils.declareLazy({
    logger: () =>
      lazy.UrlbarShared.getLogger({ prefix: `MerinoClient [${this.#name}]` }),
  });

  static get SEARCH_PARAMS() {
    return { ...SEARCH_PARAMS };
  }

  constructor(
    name = "anonymous",
    { allowOhttp = false, cachePeriodMs = 0 } = {}
  ) {
    this.#name = name;
    this.#allowOhttp = allowOhttp;
    this.#cachePeriodMs = cachePeriodMs;
  }

  get name() {
    return this.#name;
  }

  get sessionTimeoutMs() {
    return this.#sessionTimeoutMs;
  }
  set sessionTimeoutMs(value) {
    this.#sessionTimeoutMs = value;
  }

  get sessionID() {
    return this.#sessionID;
  }

  get sequenceNumber() {
    return this.#sequenceNumber;
  }

  get lastFetchStatus() {
    return this.#lastFetchStatus;
  }

  async fetch({
    query,
    providers = null,
    timeoutMs = lazy.UrlbarPrefs.get("merinoTimeoutMs"),
    otherParams = {},
    endpointUrl = lazy.UrlbarPrefs.get("merinoEndpointURL"),
  }) {
    this.#lazy.logger.debug("Fetch start", { query });

    let endpointString =
      endpointUrl || lazy.UrlbarPrefs.get("merinoEndpointURL");
    if (!endpointString) {
      return [];
    }
    let url = URL.parse(endpointString);
    if (!url) {
      let error = new Error(`${endpointString} is not a valid URL`);
      this.#lazy.logger.error("Error creating endpoint URL", error);
      return [];
    }

    url.searchParams.set(SEARCH_PARAMS.QUERY, query);

    let clientVariants = lazy.UrlbarPrefs.get("merinoClientVariants");
    if (clientVariants) {
      url.searchParams.set(SEARCH_PARAMS.CLIENT_VARIANTS, clientVariants);
    }

    let providersString;
    if (providers != null) {
      if (!Array.isArray(providers)) {
        throw new Error("providers must be an array if given");
      }
      providersString = providers.join(",");
    } else {
      let value = lazy.UrlbarPrefs.get("merinoProviders");
      if (value) {
        providersString = value;
      }
    }

    if (typeof providersString == "string") {
      url.searchParams.set(SEARCH_PARAMS.PROVIDERS, providersString);
    }

    for (const [param, value] of Object.entries(otherParams)) {
      url.searchParams.set(param, value);
    }


    let details = { query, providers, timeoutMs, url: url.toString() };

    let cacheKey;
    if (this.#cachePeriodMs && !MerinoClient._test_disableCache) {
      url.searchParams.sort();
      cacheKey = url.toString();

      if (
        this.#cache.suggestions &&
        Date.now() < this.#cache.dateMs + this.#cachePeriodMs &&
        this.#cache.key == cacheKey
      ) {
        this.#lazy.logger.debug("Fetch served from cache", details);
        return this.#cache.suggestions;
      }
    }


    if (!this.#sessionID) {
      let uuid = Services.uuid.generateUUID().toString();
      this.#sessionID = uuid.substring(1, uuid.length - 1);
      this.#sequenceNumber = 0;
      this.#sessionTimer?.cancel();

      this.#sessionTimer = new lazy.SkippableTimer({
        name: "Merino session timeout",
        time: this.#sessionTimeoutMs,
        logger: this.#lazy.logger,
        callback: () => this.resetSession(),
      });
    }
    url.searchParams.set(SEARCH_PARAMS.SESSION_ID, this.#sessionID);
    url.searchParams.set(
      SEARCH_PARAMS.SEQUENCE_NUMBER,
      this.#sequenceNumber.toString()
    );
    this.#sequenceNumber++;

    this.#lazy.logger.debug("Fetch details", {
      ...details,
      url: url.toString(),
    });

    let recordResponse = category => {
      this.#lazy.logger.debug("Fetch done", { status: category });
      this.#lastFetchStatus = category;
      recordResponse = null;
    };

    let timer = (this.#timeoutTimer = new lazy.SkippableTimer({
      name: "Merino timeout",
      time: timeoutMs,
      logger: this.#lazy.logger,
      callback: () => {
        this.#lazy.logger.debug("Fetch timed out", { timeoutMs });
        recordResponse?.("timeout");
      },
    }));

    try {
      this.#fetchController?.abort();
    } catch (error) {
      this.#lazy.logger.error("Error aborting previous fetch", error);
    }

    let response;
    let controller = (this.#fetchController = new AbortController());
    await Promise.race([
      timer.promise,
      (async () => {
        try {
          let result = await this.#fetch(url, { signal: controller.signal });
          response = result?.response;
          this.#lazy.logger.debug("Got response", {
            status: response?.status,
            elapsedMs: result ? result.elapsedMs : "n/a",
            ...details,
          });
          if (!response?.ok) {
            recordResponse?.("http_error");
          }
        } catch (error) {
          if (error.name != "AbortError") {
            this.#lazy.logger.error("Fetch error", error);
            recordResponse?.("network_error");
          }
        } finally {
          timer.cancel();
          if (controller == this.#fetchController) {
            this.#fetchController = null;
          }
          this.#nextResponseDeferred?.resolve(response);
          this.#nextResponseDeferred = null;
        }
      })(),
    ]);
    if (timer == this.#timeoutTimer) {
      this.#timeoutTimer = null;
    }

    if (!response?.ok) {
      return [];
    }

    if (response.status == 204) {
      recordResponse?.("no_suggestion");
      return [];
    }

    let body;
    try {
      body =  (await response.json());
    } catch (error) {
      this.#lazy.logger.error("Error getting response as JSON", error);
    }

    if (body) {
      this.#lazy.logger.debug("Response body", body);
    }

    if (!body?.suggestions?.length) {
      recordResponse?.("no_suggestion");
      return [];
    }

    let { suggestions, request_id } = body;
    if (!Array.isArray(suggestions)) {
      this.#lazy.logger.error("Unexpected response", body);
      recordResponse?.("no_suggestion");
      return [];
    }

    recordResponse?.("success");
    suggestions = suggestions.map(suggestion => ({
      ...suggestion,
      request_id,
      source: "merino",
    }));

    if (cacheKey) {
      this.#cache = {
        suggestions,
        key: cacheKey,
        dateMs: Date.now(),
      };
    }

    return suggestions;
  }

  async autoCompleteWeatherLocation({ source, query, timeoutMs = undefined }) {
    let response = await this.fetch({
      providers: ["accuweather"],
      query: query || "",
      otherParams: {
        request_type: "location",
        source,
      },
      timeoutMs,
    });
    return response?.[0] ?? null;
  }

  async fetchWeatherReport({
    source,
    city = undefined,
    country = undefined,
    region = undefined,
    locationName = undefined,
    timeoutMs = undefined,
  }) {
    if (locationName) {
      city = undefined;
      country = undefined;
      region = undefined;
    }

    let otherParams = {
      request_type: "weather",
      source,
    };

    if (!locationName) {
      let geoParams = await this.#resolveGeoParams(city, country, region);
      if (!geoParams) {
        return null;
      }
      Object.assign(otherParams, geoParams);
    }

    let response = await this.fetch({
      providers: ["accuweather"],
      query: locationName ?? "",
      otherParams,
      timeoutMs,
      endpointUrl: lazy.UrlbarPrefs.get("merino.weather.reportEndpointURL"),
    });
    return response?.[0] ?? null;
  }

  async fetchHourlyForecasts({
    source,
    locationName = undefined,
    city = undefined,
    region = undefined,
    country = undefined,
  }) {
    const hourlyEndpointURL = lazy.UrlbarPrefs.get(
      "merino.weather.hourlyEndpointURL"
    );

    if (!hourlyEndpointURL) {
      return null;
    }

    let url = URL.parse(hourlyEndpointURL);
    if (!url) {
      this.#lazy.logger.error(
        "Invalid hourly forecast endpoint URL",
        hourlyEndpointURL
      );
      return null;
    }

    if (locationName) {
      url.searchParams.set("q", locationName);
    }

    let geoParams = await this.#resolveGeoParams(city, country, region);
    if (!geoParams) {
      return null;
    }
    for (let [key, value] of Object.entries(geoParams)) {
      url.searchParams.set(key, value);
    }

    if (source) {
      url.searchParams.set("source", source);
    }

    try {
      const response = await fetch(url);
      const data =  (await response.json());

      this.#lazy.logger.debug("fetchHourlyForecasts response", data);
      return data;
    } catch (e) {
      this.#lazy.logger.error("Hourly forecast fetch error", e);
      return null;
    }
  }

  async #resolveGeoParams(city, country, region) {
    if (!city && !country && !region) {
      let geolocation = await lazy.GeolocationUtils.geolocation();
      if (!geolocation) {
        return null;
      }
      country = geolocation.country_code;
      region =
        geolocation.region_code || geolocation.region || geolocation.city;
      city = geolocation.city || geolocation.region;
    }
    let params = {};
    if (country) {
      params.country = country;
    }
    if (region) {
      params.region = region;
    }
    if (city) {
      params.city = city;
    }
    return params;
  }

  resetSession() {
    this.#sessionID = null;
    this.#sequenceNumber = 0;
    this.#sessionTimer?.cancel();
    this.#sessionTimer = null;
    this.#nextSessionResetDeferred?.resolve();
    this.#nextSessionResetDeferred = null;
  }

  cancelTimeoutTimer() {
    this.#timeoutTimer?.cancel();
  }

  waitForNextResponse() {
    if (!this.#nextResponseDeferred) {
      this.#nextResponseDeferred = Promise.withResolvers();
    }
    return this.#nextResponseDeferred.promise;
  }

  waitForNextSessionReset() {
    if (!this.#nextSessionResetDeferred) {
      this.#nextSessionResetDeferred = Promise.withResolvers();
    }
    return this.#nextSessionResetDeferred.promise;
  }

  async #fetch(url, { signal }) {
    let configUrl;
    let relayUrl;
    if (this.#allowOhttp) {
      configUrl = lazy.UrlbarPrefs.get("merinoOhttpConfigURL");
      relayUrl = lazy.UrlbarPrefs.get("merinoOhttpRelayURL");
    }

    let useOhttp = configUrl && relayUrl;

    let response;
    let startMs = ChromeUtils.now();
    if (!useOhttp) {
      response = await fetch(url, { signal });
    } else {
      let config = await lazy.ObliviousHTTP.getOHTTPConfig(configUrl);
      if (!config) {
        this.#lazy.logger.error("Couldn't get OHTTP config");
        return null;
      }

      this.#lazy.logger.debug("Sending request using OHTTP", { url });
      response = await lazy.ObliviousHTTP.ohttpRequest(relayUrl, config, url, {
        signal,
        headers: {},
      });
    }

    let elapsedMs = ChromeUtils.now() - startMs;
    let label = response.status.toString();
    if (useOhttp) {
      label += "_ohttp";
    }

    return { response, elapsedMs };
  }

  static _test_disableCache = false;

  get _test_sessionTimer() {
    return this.#sessionTimer;
  }

  get _test_timeoutTimer() {
    return this.#timeoutTimer;
  }

  get _test_fetchController() {
    return this.#fetchController;
  }

  #sessionID = null;
  #sequenceNumber = 0;
  #sessionTimer = null;
  #sessionTimeoutMs = SESSION_TIMEOUT_MS;

  #name;
  #timeoutTimer = null;
  #fetchController = null;
  #lastFetchStatus = null;
  #nextResponseDeferred = null;
  #nextSessionResetDeferred = null;
  #cachePeriodMs = 0;
  #allowOhttp = false;

  #cache = {
    suggestions: null,
    key: null,
    dateMs: 0,
  };
}
