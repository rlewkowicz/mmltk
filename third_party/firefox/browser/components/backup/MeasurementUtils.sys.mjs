/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export const BYTES_IN_KILOBYTE = 1000;
export const BYTES_IN_MEGABYTE = 1000000;

export const BYTES_IN_KIBIBYTE = 1024;
export const BYTES_IN_MEBIBYTE = 1048576;

export const MeasurementUtils = {
  fuzzByteSize(bytes, nearest) {
    const fuzzed = Math.round(bytes / nearest) * nearest;
    return Math.max(fuzzed, nearest);
  },

};
