/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { BANDWIDTH } from "chrome://browser/content/ipprotection/ipprotection-constants.mjs";

const countryDisplayNames = new Intl.DisplayNames(undefined, {
  type: "region",
});

export function countryName(code) {
  try {
    return countryDisplayNames.of(code) ?? null;
  } catch (_) {
    return null;
  }
}

export function formatRemainingBandwidth(remainingBytes, locale = undefined) {
  const remainingGB = remainingBytes / BANDWIDTH.BYTES_IN_GB;
  if (remainingGB < 1) {
    return {
      value: Math.floor(remainingBytes / BANDWIDTH.BYTES_IN_MB),
      useGB: false,
    };
  }
  return {
    value: new Intl.NumberFormat(locale, {
      maximumFractionDigits: 1,
    }).format(remainingGB),
    useGB: true,
  };
}
