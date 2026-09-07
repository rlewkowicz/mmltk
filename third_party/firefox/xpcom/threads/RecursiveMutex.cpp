/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/RecursiveMutex.h"


namespace mozilla {

RecursiveMutex::RecursiveMutex(const char* aName)
    : BlockingResourceBase(aName, eRecursiveMutex)
#if defined(DEBUG)
      ,
      mOwningThread(nullptr),
      mEntryCount(0)
#endif
{
  pthread_mutexattr_t attr;

  MOZ_RELEASE_ASSERT(pthread_mutexattr_init(&attr) == 0,
                     "pthread_mutexattr_init failed");

  MOZ_RELEASE_ASSERT(
      pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0,
      "pthread_mutexattr_settype failed");

  MOZ_RELEASE_ASSERT(pthread_mutex_init(&mMutex, &attr) == 0,
                     "pthread_mutex_init failed");

  MOZ_RELEASE_ASSERT(pthread_mutexattr_destroy(&attr) == 0,
                     "pthread_mutexattr_destroy failed");
}

RecursiveMutex::~RecursiveMutex() {
  MOZ_RELEASE_ASSERT(pthread_mutex_destroy(&mMutex) == 0,
                     "pthread_mutex_destroy failed");
}

void RecursiveMutex::LockInternal() {
  MOZ_RELEASE_ASSERT(pthread_mutex_lock(&mMutex) == 0,
                     "pthread_mutex_lock failed");
}

void RecursiveMutex::UnlockInternal() {
  MOZ_RELEASE_ASSERT(pthread_mutex_unlock(&mMutex) == 0,
                     "pthread_mutex_unlock failed");
}

}  
