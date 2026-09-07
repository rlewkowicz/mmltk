/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ReentrantMonitor_h
#define mozilla_ReentrantMonitor_h

#include "prmon.h"

#include "mozilla/BlockingResourceBase.h"
#include "mozilla/ThreadSafety.h"
#include "nsISupports.h"
namespace mozilla {

class MOZ_CAPABILITY("reentrant monitor") ReentrantMonitor
    : BlockingResourceBase {
 public:
  explicit ReentrantMonitor(const char* aName)
      : BlockingResourceBase(aName, eReentrantMonitor)
#ifdef DEBUG
        ,
        mEntryCount(0)
#endif
  {
    MOZ_COUNT_CTOR(ReentrantMonitor);
    mReentrantMonitor = PR_NewMonitor();
    if (!mReentrantMonitor) {
      MOZ_CRASH("Can't allocate mozilla::ReentrantMonitor");
    }
  }

  ~ReentrantMonitor() {
    NS_ASSERTION(mReentrantMonitor,
                 "improperly constructed ReentrantMonitor or double free");
    PR_DestroyMonitor(mReentrantMonitor);
    mReentrantMonitor = 0;
    MOZ_COUNT_DTOR(ReentrantMonitor);
  }

#ifndef DEBUG
  void Enter() MOZ_CAPABILITY_ACQUIRE() { PR_EnterMonitor(mReentrantMonitor); }

  void Exit() MOZ_CAPABILITY_RELEASE() { PR_ExitMonitor(mReentrantMonitor); }

  nsresult Wait(PRIntervalTime aInterval = PR_INTERVAL_NO_TIMEOUT) {
    PR_ASSERT_CURRENT_THREAD_IN_MONITOR(mReentrantMonitor);
    return PR_Wait(mReentrantMonitor, aInterval) == PR_SUCCESS
               ? NS_OK
               : NS_ERROR_FAILURE;
  }

#else  // ifndef DEBUG
  void Enter() MOZ_CAPABILITY_ACQUIRE();
  void Exit() MOZ_CAPABILITY_RELEASE();
  nsresult Wait(PRIntervalTime aInterval = PR_INTERVAL_NO_TIMEOUT);

#endif  // ifndef DEBUG

  nsresult Notify() {
    return PR_Notify(mReentrantMonitor) == PR_SUCCESS ? NS_OK
                                                      : NS_ERROR_FAILURE;
  }

  nsresult NotifyAll() {
    return PR_NotifyAll(mReentrantMonitor) == PR_SUCCESS ? NS_OK
                                                         : NS_ERROR_FAILURE;
  }

#ifdef DEBUG
  void AssertCurrentThreadIn() MOZ_ASSERT_CAPABILITY(this) {
    PR_ASSERT_CURRENT_THREAD_IN_MONITOR(mReentrantMonitor);
  }

  void AssertNotCurrentThreadIn() MOZ_ASSERT_CAPABILITY(!this) {
  }

#else
  void AssertCurrentThreadIn() MOZ_ASSERT_CAPABILITY(this) {}
  void AssertNotCurrentThreadIn() MOZ_ASSERT_CAPABILITY(!this) {}

#endif  // ifdef DEBUG

 private:
  ReentrantMonitor();
  ReentrantMonitor(const ReentrantMonitor&);
  ReentrantMonitor& operator=(const ReentrantMonitor&);

  PRMonitor* mReentrantMonitor;
#ifdef DEBUG
  int32_t mEntryCount;
#endif
};

class MOZ_SCOPED_CAPABILITY MOZ_STACK_CLASS ReentrantMonitorAutoEnter {
 public:
  explicit ReentrantMonitorAutoEnter(
      mozilla::ReentrantMonitor& aReentrantMonitor)
      MOZ_CAPABILITY_ACQUIRE(aReentrantMonitor)
      : mReentrantMonitor(&aReentrantMonitor) {
    NS_ASSERTION(mReentrantMonitor, "null monitor");
    mReentrantMonitor->Enter();
  }

  ~ReentrantMonitorAutoEnter(void) MOZ_CAPABILITY_RELEASE() {
    mReentrantMonitor->Exit();
  }

  nsresult Wait(PRIntervalTime aInterval = PR_INTERVAL_NO_TIMEOUT) {
    return mReentrantMonitor->Wait(aInterval);
  }

  nsresult Notify() { return mReentrantMonitor->Notify(); }
  nsresult NotifyAll() { return mReentrantMonitor->NotifyAll(); }

 private:
  ReentrantMonitorAutoEnter();
  ReentrantMonitorAutoEnter(const ReentrantMonitorAutoEnter&);
  ReentrantMonitorAutoEnter& operator=(const ReentrantMonitorAutoEnter&);
  static void* operator new(size_t) noexcept(true);

  friend class ReentrantMonitorAutoExit;

  mozilla::ReentrantMonitor* mReentrantMonitor;
};

class MOZ_SCOPED_CAPABILITY MOZ_STACK_CLASS ReentrantMonitorAutoExit {
 public:
  explicit ReentrantMonitorAutoExit(ReentrantMonitor& aReentrantMonitor)
      MOZ_EXCLUSIVE_RELEASE(aReentrantMonitor)
      : mReentrantMonitor(&aReentrantMonitor) {
    NS_ASSERTION(mReentrantMonitor, "null monitor");
    mReentrantMonitor->AssertCurrentThreadIn();
    mReentrantMonitor->Exit();
  }

  explicit ReentrantMonitorAutoExit(
      ReentrantMonitorAutoEnter& aReentrantMonitorAutoEnter)
      MOZ_EXCLUSIVE_RELEASE(aReentrantMonitorAutoEnter.mReentrantMonitor)
      : mReentrantMonitor(aReentrantMonitorAutoEnter.mReentrantMonitor) {
    NS_ASSERTION(mReentrantMonitor, "null monitor");
    mReentrantMonitor->AssertCurrentThreadIn();
    mReentrantMonitor->Exit();
  }

  ~ReentrantMonitorAutoExit(void) MOZ_EXCLUSIVE_RELEASE() {
    mReentrantMonitor->Enter();
  }

 private:
  ReentrantMonitorAutoExit();
  ReentrantMonitorAutoExit(const ReentrantMonitorAutoExit&);
  ReentrantMonitorAutoExit& operator=(const ReentrantMonitorAutoExit&);
  static void* operator new(size_t) noexcept(true);

  ReentrantMonitor* mReentrantMonitor;
};

}  

#endif  // ifndef mozilla_ReentrantMonitor_h
