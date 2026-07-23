/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_FlushType_h
#define mozilla_FlushType_h

#include <stdint.h>

#include "mozilla/EnumeratedArray.h"

namespace mozilla {

enum class FlushType : uint8_t {
  None,             
  Event,            
  Content,          
  ContentAndNotify, 
  Style,            
  Frames,           
  EnsurePresShellInitAndFrames, 
  InterruptibleLayout, 
  Layout,              
  Display,             
  Count
};

// clang-format off
const EnumeratedArray<FlushType, const char*, size_t(FlushType::Count)>
    kFlushTypeNames = {
  "",
  "Event",
  "Content",
  "ContentAndNotify",
  "Style",
  "Style",
  "Style",
  "InterruptibleLayout",
  "Layout",
  "Display"
};
// clang-format on

struct ChangesToFlush {
  ChangesToFlush(FlushType aFlushType, bool aFlushAnimations,
                 bool aUpdateRelevancy)
      : mFlushType(aFlushType),
        mFlushAnimations(aFlushAnimations),
        mUpdateRelevancy(aUpdateRelevancy) {
    MOZ_ASSERT_IF(mUpdateRelevancy, mFlushType >= FlushType::Layout);
  }

  FlushType mFlushType;
  bool mFlushAnimations;
  bool mUpdateRelevancy;
};

}  

#endif  // mozilla_FlushType_h
