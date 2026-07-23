/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(MOZILLA_GFX_CRITICALSECTION_H_)
#define MOZILLA_GFX_CRITICALSECTION_H_

#  include <pthread.h>
#  include "mozilla/DebugOnly.h"

namespace mozilla {
namespace gfx {


class PosixCondvar;
class CriticalSection {
 public:
  CriticalSection() {
    DebugOnly<int> err = pthread_mutex_init(&mMutex, nullptr);
    MOZ_ASSERT(!err);
  }

  ~CriticalSection() {
    DebugOnly<int> err = pthread_mutex_destroy(&mMutex);
    MOZ_ASSERT(!err);
  }

  void Enter() {
    DebugOnly<int> err = pthread_mutex_lock(&mMutex);
    MOZ_ASSERT(!err);
  }

  void Leave() {
    DebugOnly<int> err = pthread_mutex_unlock(&mMutex);
    MOZ_ASSERT(!err);
  }

 protected:
  pthread_mutex_t mMutex;
  friend class PosixCondVar;
};


struct CriticalSectionAutoEnter final {
  explicit CriticalSectionAutoEnter(CriticalSection* aSection)
      : mSection(aSection) {
    mSection->Enter();
  }
  ~CriticalSectionAutoEnter() { mSection->Leave(); }

 protected:
  CriticalSection* mSection;
};

}  
}  

#endif
