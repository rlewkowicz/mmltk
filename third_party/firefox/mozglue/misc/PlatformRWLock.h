/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_PlatformRWLock_h)
#define mozilla_PlatformRWLock_h

#include "mozilla/Types.h"

#  include <pthread.h>

namespace mozilla::detail {

class RWLockImpl {
 public:
  explicit MFBT_API RWLockImpl();
  MFBT_API ~RWLockImpl();

 protected:
  [[nodiscard]] MFBT_API bool tryReadLock();
  MFBT_API void readLock();
  MFBT_API void readUnlock();

  [[nodiscard]] MFBT_API bool tryWriteLock();
  MFBT_API void writeLock();
  MFBT_API void writeUnlock();

 private:
  RWLockImpl(const RWLockImpl&) = delete;
  void operator=(const RWLockImpl&) = delete;
  RWLockImpl(RWLockImpl&&) = delete;
  void operator=(RWLockImpl&&) = delete;
  bool operator==(const RWLockImpl& rhs) = delete;

  pthread_rwlock_t mRWLock;
};

}  

#endif
