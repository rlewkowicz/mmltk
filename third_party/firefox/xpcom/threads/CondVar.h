/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_CondVar_h
#define mozilla_CondVar_h

#include "mozilla/BlockingResourceBase.h"
#include "mozilla/PlatformConditionVariable.h"
#include "mozilla/Mutex.h"
#include "mozilla/TimeStamp.h"

namespace mozilla {

class OffTheBooksCondVar : BlockingResourceBase {
 public:
  OffTheBooksCondVar(OffTheBooksMutex& aLock, const char* aName)
      : BlockingResourceBase(aName, eCondVar), mLock(&aLock) {}

  ~OffTheBooksCondVar() = default;

#ifndef DEBUG
  void Wait() {
    mImpl.wait(*mLock);
  }

  CVStatus Wait(TimeDuration aDuration) {
    return mImpl.wait_for(*mLock, aDuration);
  }
#else
  void Wait();
  CVStatus Wait(TimeDuration aDuration);
#endif

  void Notify() { mImpl.notify_one(); }

  void NotifyAll() { mImpl.notify_all(); }

#ifdef DEBUG
  void AssertCurrentThreadOwnsMutex() const MOZ_ASSERT_CAPABILITY(mLock) {
    mLock->AssertCurrentThreadOwns();
  }

  void AssertNotCurrentThreadOwnsMutex() const MOZ_ASSERT_CAPABILITY(!mLock) {
    mLock->AssertNotCurrentThreadOwns();
  }

#else
  void AssertCurrentThreadOwnsMutex() const MOZ_ASSERT_CAPABILITY(mLock) {}
  void AssertNotCurrentThreadOwnsMutex() const MOZ_ASSERT_CAPABILITY(!mLock) {}

#endif  // ifdef DEBUG

 private:
  OffTheBooksCondVar();
  OffTheBooksCondVar(const OffTheBooksCondVar&) = delete;
  OffTheBooksCondVar& operator=(const OffTheBooksCondVar&) = delete;

  OffTheBooksMutex* mLock;
  detail::ConditionVariableImpl mImpl;
};

class CondVar : public OffTheBooksCondVar {
 public:
  CondVar(OffTheBooksMutex& aLock, const char* aName)
      : OffTheBooksCondVar(aLock, aName) {
    MOZ_COUNT_CTOR(CondVar);
  }

  MOZ_COUNTED_DTOR(CondVar)

 private:
  CondVar();
  CondVar(const CondVar&);
  CondVar& operator=(const CondVar&);
};

}  

#endif  // ifndef mozilla_CondVar_h
