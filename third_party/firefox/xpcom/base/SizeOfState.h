/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef SizeOfState_h
#define SizeOfState_h

#include "mozilla/fallible.h"
#include "mozilla/MemoryReporting.h"
#include "nsTHashtable.h"
#include "nsHashKeys.h"


namespace mozilla {

class SeenPtrs : public nsTHashtable<nsPtrHashKey<const void>> {
 public:
  bool HaveSeenPtr(const void* aPtr) {
    uint32_t oldCount = Count();

    (void)PutEntry(aPtr, fallible);

    return oldCount == Count();
  }
};

class SizeOfState {
 public:
  explicit SizeOfState(MallocSizeOf aMallocSizeOf)
      : mMallocSizeOf(aMallocSizeOf) {}

  bool HaveSeenPtr(const void* aPtr) { return mSeenPtrs.HaveSeenPtr(aPtr); }

  MallocSizeOf mMallocSizeOf;
  SeenPtrs mSeenPtrs;
};

}  

#endif  // SizeOfState_h
