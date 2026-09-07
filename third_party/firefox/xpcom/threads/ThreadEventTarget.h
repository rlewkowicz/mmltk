/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ThreadEventTarget_h
#define mozilla_ThreadEventTarget_h

#include "mozilla/MemoryReporting.h"
#include "mozilla/Mutex.h"
#include "mozilla/SynchronizedEventQueue.h"  // for ThreadTargetSink
#include "nsISerialEventTarget.h"

namespace mozilla {
class DelayedRunnable;

class ThreadEventTarget final : public nsISerialEventTarget {
 public:
  ThreadEventTarget(ThreadTargetSink* aSink, bool aIsMainThread,
                    bool aBlockDispatch);

  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIEVENTTARGET_FULL

  void Disconnect(const MutexAutoLock& aProofOfLock) {
    mSink->Disconnect(aProofOfLock);
  }

  void SetCurrentThread(PRThread* aThread);
  void ClearCurrentThread();

  size_t SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const {
    size_t n = 0;
    if (mSink) {
      n += mSink->SizeOfIncludingThis(aMallocSizeOf);
    }
    return aMallocSizeOf(this) + n;
  }

#ifdef DEBUG
  static void XPCOMShutdownThreadsNotificationFinished();
#endif

 private:
  ~ThreadEventTarget();

  RefPtr<ThreadTargetSink> mSink;
#ifdef DEBUG
  const bool mIsMainThread;
#endif
  const bool mBlockDispatch;
};

}  

#endif  // mozilla_ThreadEventTarget_h
