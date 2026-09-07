/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_LayoutStructs_h
#define mozilla_LayoutStructs_h

#include "mozilla/AspectRatio.h"
#include "mozilla/ServoStyleConsts.h"

namespace mozilla {

struct StyleSizeOverrides {
  Maybe<StyleSize> mStyleISize;
  Maybe<StyleSize> mStyleBSize;
  Maybe<AspectRatio> mAspectRatio;

  bool HasAnyOverrides() const { return mStyleISize || mStyleBSize; }
  bool HasAnyLengthOverrides() const {
    return (mStyleISize && mStyleISize->ConvertsToLength()) ||
           (mStyleBSize && mStyleBSize->ConvertsToLength());
  }

  bool mApplyOverridesVerbatim = false;
};

enum class BreakType : uint8_t {
  Auto,
  Column,
  Page,
};

}  

#endif  // mozilla_LayoutStructs_h
