/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsMemoryPressure_h_
#define nsMemoryPressure_h_

#include "nscore.h"

extern const char* const kTopicMemoryPressure;
extern const char* const kTopicMemoryPressureStop;
extern const char16_t* const kSubTopicLowMemoryNew;
extern const char16_t* const kSubTopicLowMemoryOngoing;

enum class MemoryPressureState : uint32_t {
  None,  
  LowMemory,
  NoPressure,
};

void NS_NotifyOfEventualMemoryPressure(MemoryPressureState aState);

nsresult NS_NotifyOfMemoryPressure(MemoryPressureState aState);

#endif  // nsMemoryPressure_h_
