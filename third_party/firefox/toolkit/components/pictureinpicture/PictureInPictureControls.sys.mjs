/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export const KEYBOARD_CONTROLS = {
  ALL: 0,
  PLAY_PAUSE: 1 << 0,
  MUTE_UNMUTE: 1 << 1,
  VOLUME: 1 << 2,
  SEEK: 1 << 3,
  CLOSE: 1 << 4,
  LIVE_SEEK: 1 << 5,
};

export const TOGGLE_POLICIES = {
  DEFAULT: 1,
  HIDDEN: 2,
  TOP: 3,
  ONE_QUARTER: 4,
  MIDDLE: 5,
  THREE_QUARTERS: 6,
  BOTTOM: 7,
};

export const TOGGLE_POLICY_STRINGS = {
  [TOGGLE_POLICIES.DEFAULT]: "default",
  [TOGGLE_POLICIES.HIDDEN]: "hidden",
  [TOGGLE_POLICIES.TOP]: "top",
  [TOGGLE_POLICIES.ONE_QUARTER]: "one-quarter",
  [TOGGLE_POLICIES.MIDDLE]: "middle",
  [TOGGLE_POLICIES.THREE_QUARTERS]: "three-quarters",
  [TOGGLE_POLICIES.BOTTOM]: "bottom",
};
