/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  MerinoClient: "moz-src:///browser/components/urlbar/MerinoClient.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "logger", () =>
  lazy.UrlbarShared.getLogger({ prefix: "GeolocationUtils" })
);

const GEOLOCATION_CACHE_PERIOD_MS = 2 * 60 * 60 * 1000; 

const EARTH_RADIUS_KM = 6371.009;

const MERINO_TIMEOUT_MS = 5000;

class _GeolocationUtils {
  async geolocation() {
    if (!this.#merino) {
      this.#merino = new lazy.MerinoClient("GeolocationUtils", {
        cachePeriodMs: GEOLOCATION_CACHE_PERIOD_MS,
      });
    }

    lazy.logger.debug("Fetching geolocation from Merino");
    let results = await this.#merino.fetch({
      providers: ["geolocation"],
      query: "",
      timeoutMs: MERINO_TIMEOUT_MS,
    });

    lazy.logger.debug("Got geolocation from Merino", results);

    return results?.[0]?.custom_details?.geolocation || null;
  }


  async best(items, locationFromItem = i => i) {
    if (items.length <= 1) {
      return items[0] || null;
    }

    let geo = await this.geolocation();
    if (!geo) {
      return items[0];
    }
    return (
      this.#bestByDistance(geo, items, locationFromItem) ||
      this.#bestByRegion(geo, items, locationFromItem) ||
      items[0]
    );
  }

  #bestByDistance(geo, items, locationFromItem) {
    let geoLat = parseFloat(geo.location?.latitude);
    let geoLong = parseFloat(geo.location?.longitude);
    if (isNaN(geoLat) || isNaN(geoLong)) {
      return null;
    }

    [geoLat, geoLong] = [geoLat, geoLong].map(toRadians);
    let geoLatSin = Math.sin(geoLat);
    let geoLatCos = Math.cos(geoLat);
    let geoRadius = geo.location?.radius || 5;

    let bestTuple;
    let dMin = Infinity;
    for (let item of items) {
      let location = locationFromItem(item);
      if (!location) {
        continue;
      }

      let locationLat =
        typeof location.latitude == "number"
          ? location.latitude
          : parseFloat(location.latitude);
      let locationLong =
        typeof location.longitude == "number"
          ? location.longitude
          : parseFloat(location.longitude);
      if (isNaN(locationLat) || isNaN(locationLong)) {
        continue;
      }

      let [itemLat, itemLong] = [locationLat, locationLong].map(toRadians);
      let d =
        EARTH_RADIUS_KM *
        Math.acos(
          geoLatSin * Math.sin(itemLat) +
            geoLatCos *
              Math.cos(itemLat) *
              Math.cos(Math.abs(geoLong - itemLong))
        );
      if (
        !bestTuple ||
        d + geoRadius < dMin ||
        (Math.abs(d - dMin) <= geoRadius &&
          hasLargerPopulation(location, bestTuple.location))
      ) {
        dMin = d;
        bestTuple = { item, location };
      }
    }

    return bestTuple?.item || null;
  }

  #bestByRegion(geo, items, locationFromItem) {
    let geoCountry = geo.country_code?.toLowerCase();
    if (!geoCountry) {
      return null;
    }

    let geoRegion = geo.region_code?.toLowerCase();

    let bestCountryTuple;
    let bestRegionTuple;
    for (let item of items) {
      let location = locationFromItem(item);
      if (location?.country?.toLowerCase() == geoCountry) {
        if (
          !bestCountryTuple ||
          hasLargerPopulation(location, bestCountryTuple.location)
        ) {
          bestCountryTuple = { item, location };
        }
        if (
          location.region?.toLowerCase() == geoRegion &&
          (!bestRegionTuple ||
            hasLargerPopulation(location, bestRegionTuple.location))
        ) {
          bestRegionTuple = { item, location };
        }
      }
    }

    return bestRegionTuple?.item || bestCountryTuple?.item || null;
  }

  #merino;
}

function toRadians(deg) {
  return (deg * Math.PI) / 180;
}

function hasLargerPopulation(a, b) {
  return (
    typeof a.population == "number" &&
    (typeof b.population != "number" || b.population < a.population)
  );
}

export const GeolocationUtils = new _GeolocationUtils();
