/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_URIHasher_h
#define mozilla_net_URIHasher_h

#include "mozilla/Atomics.h"
#include "mozilla/HashFunctions.h"
#include "nsString.h"

namespace mozilla::net {

class URIHasher {
 protected:
  uint32_t CachedSpecHash(const nsACString& aSpec) {
    uint32_t hash = mSpecHash;
    if (hash == 0) {
      hash = HashString(aSpec.BeginReading(), aSpec.Length());
      mSpecHash = hash;
    }
    return hash;
  }

  void ResetSpecHash() { mSpecHash = 0; }

 private:
  Atomic<uint32_t, Relaxed> mSpecHash{0};
};

}  

#endif  // mozilla_net_URIHasher_h
