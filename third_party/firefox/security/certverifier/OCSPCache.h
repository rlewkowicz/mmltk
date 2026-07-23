/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
/* Copyright 2013 Mozilla Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef mozilla_psm_OCSPCache_h
#define mozilla_psm_OCSPCache_h

#include "hasht.h"
#include "mozilla/Mutex.h"
#include "mozilla/Vector.h"
#include "mozpkix/Result.h"
#include "mozpkix/Time.h"
#include "prerror.h"
#include "seccomon.h"

namespace mozilla {
class OriginAttributes;
}

namespace mozilla {
namespace pkix {
struct CertID;
}
}  

namespace mozilla {
namespace psm {

typedef uint8_t SHA384Buffer[SHA384_LENGTH];

class OCSPCache {
 public:
  OCSPCache();
  ~OCSPCache();

  bool Get(const mozilla::pkix::CertID& aCertID,
           const OriginAttributes& aOriginAttributes,
            mozilla::pkix::Result& aResult,
            mozilla::pkix::Time& aValidThrough);

  mozilla::pkix::Result Put(const mozilla::pkix::CertID& aCertID,
                            const OriginAttributes& aOriginAttributes,
                            mozilla::pkix::Result aResult,
                            mozilla::pkix::Time aThisUpdate,
                            mozilla::pkix::Time aValidThrough);

  void Clear();

  void ClearPrivateBrowsing();

 private:
  class Entry {
   public:
    Entry(mozilla::pkix::Result aResult, mozilla::pkix::Time aThisUpdate,
          mozilla::pkix::Time aValidThrough)
        : mResult(aResult),
          mThisUpdate(aThisUpdate),
          mValidThrough(aValidThrough) {}
    mozilla::pkix::Result Init(const mozilla::pkix::CertID& aCertID,
                               const OriginAttributes& aOriginAttributes);

    mozilla::pkix::Result mResult;
    mozilla::pkix::Time mThisUpdate;
    mozilla::pkix::Time mValidThrough;
    bool mIsPrivateBrowsing = false;
    SHA384Buffer mIDHash;
  };

  bool FindInternal(const mozilla::pkix::CertID& aCertID,
                    const OriginAttributes& aOriginAttributes,
                     size_t& index, const MutexAutoLock& aProofOfLock);
  void MakeMostRecentlyUsed(size_t aIndex, const MutexAutoLock& aProofOfLock);

  Mutex mMutex;
  static const size_t MaxEntries = 1024;
  Vector<Entry*, 256> mEntries MOZ_GUARDED_BY(mMutex);
};

}  
}  

#endif  // mozilla_psm_OCSPCache_h
