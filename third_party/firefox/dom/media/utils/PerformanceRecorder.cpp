/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PerformanceRecorder.h"

#include "base/process_util.h"
#include "nsPrintfCString.h"

namespace mozilla {

TrackingId::TrackingId() : mSource(Source::Unimplemented), mUniqueInProcId(0) {}

TrackingId::TrackingId(Source aSource, uint32_t aUniqueInProcId,
                       TrackAcrossProcesses aTrack)
    : mSource(aSource),
      mUniqueInProcId(aUniqueInProcId),
      mProcId(aTrack == TrackAcrossProcesses::Yes
                  ? Some(base::GetCurrentProcId())
                  : Nothing()) {}

nsCString TrackingId::ToString() const {
  if (mProcId) {
    return nsPrintfCString("%s-%u-%u", EnumValueToString(mSource), *mProcId,
                           mUniqueInProcId);
  }
  return nsPrintfCString("%s-%u", EnumValueToString(mSource), mUniqueInProcId);
}

}  
