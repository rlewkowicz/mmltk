/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ScrollStyles_h
#define mozilla_ScrollStyles_h

#include <stdint.h>

struct nsStyleDisplay;

namespace mozilla {

enum class StyleOverflow : uint8_t;

struct ScrollStyles {
  StyleOverflow mHorizontal;
  StyleOverflow mVertical;

  ScrollStyles(StyleOverflow aH, StyleOverflow aV);

  enum MapOverflowToValidScrollStyleTag { MapOverflowToValidScrollStyle };
  ScrollStyles(const nsStyleDisplay&, MapOverflowToValidScrollStyleTag);

  bool operator==(const ScrollStyles&) const = default;
  bool operator!=(const ScrollStyles&) const = default;

  bool IsHiddenInBothDirections() const;
};

}  

#endif  // mozilla_ScrollStyles_h
