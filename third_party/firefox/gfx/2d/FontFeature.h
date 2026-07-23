/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_gfx_FontFeature
#define mozilla_gfx_FontFeature

#include <cstdint>

namespace mozilla::gfx {
struct FontFeature {
  uint32_t mTag;
  uint32_t mValue;
};

inline bool operator<(const FontFeature& a, const FontFeature& b) {
  return (a.mTag < b.mTag) || ((a.mTag == b.mTag) && (a.mValue < b.mValue));
}

inline bool operator==(const FontFeature& a, const FontFeature& b) {
  return (a.mTag == b.mTag) && (a.mValue == b.mValue);
}

}  

#endif
