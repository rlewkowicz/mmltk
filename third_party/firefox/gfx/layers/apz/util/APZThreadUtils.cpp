/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "APZThreadUtils.h"

#include "mozilla/ClearOnShutdown.h"
#include "mozilla/StaticMutex.h"

#include "nsISerialEventTarget.h"
#include "nsThreadUtils.h"
#include "nsXULAppAPI.h"

namespace mozilla {
namespace layers {

static bool sThreadAssertionsEnabled = true;
static StaticRefPtr<nsISerialEventTarget> sControllerThread;
static StaticMutex sControllerThreadMutex MOZ_UNANNOTATED;

void APZThreadUtils::SetThreadAssertionsEnabled(bool aEnabled) {
  StaticMutexAutoLock lock(sControllerThreadMutex);
  sThreadAssertionsEnabled = aEnabled;
}

bool APZThreadUtils::GetThreadAssertionsEnabled() {
  StaticMutexAutoLock lock(sControllerThreadMutex);
  return sThreadAssertionsEnabled;
}

void APZThreadUtils::SetControllerThread(nsISerialEventTarget* aThread) {
  StaticMutexAutoLock lock(sControllerThreadMutex);
  MOZ_ASSERT(!sControllerThread || !aThread || sControllerThread == aThread);
  if (aThread != sControllerThread) {
    sControllerThread = aThread;
    ClearOnShutdown(&sControllerThread);
  }
}

void APZThreadUtils::AssertOnControllerThread() {
#if DEBUG
  if (!GetThreadAssertionsEnabled()) {
    return;
  }
  StaticMutexAutoLock lock(sControllerThreadMutex);
  MOZ_ASSERT(sControllerThread && sControllerThread->IsOnCurrentThread());
#endif
}

void APZThreadUtils::RunOnControllerThread(
    RefPtr<Runnable>&& aTask, nsIEventTarget::DispatchFlags flags) {
  RefPtr<nsISerialEventTarget> thread;
  {
    StaticMutexAutoLock lock(sControllerThreadMutex);
    thread = sControllerThread;
  }
  RefPtr<Runnable> task = std::move(aTask);

  if (!thread) {
    NS_WARNING("Dropping task posted to controller thread");
    return;
  }

  if (thread->IsOnCurrentThread()) {
    task->Run();
  } else {
    thread->Dispatch(task.forget(), flags);
  }
}

already_AddRefed<nsISerialEventTarget> APZThreadUtils::GetControllerThread() {
  StaticMutexAutoLock lock(sControllerThreadMutex);
  return do_AddRef(sControllerThread);
}

bool APZThreadUtils::IsControllerThread() {
  StaticMutexAutoLock lock(sControllerThreadMutex);
  return sControllerThread && sControllerThread->IsOnCurrentThread();
}

bool APZThreadUtils::IsControllerThreadAlive() {
  StaticMutexAutoLock lock(sControllerThreadMutex);
  return !!sControllerThread;
}

void APZThreadUtils::DelayedDispatch(already_AddRefed<Runnable> aRunnable,
                                     int aDelayMs) {
  MOZ_ASSERT(!XRE_IsContentProcess(),
             "ContentProcessController should only be used remotely.");
  RefPtr<nsISerialEventTarget> thread;
  {
    StaticMutexAutoLock lock(sControllerThreadMutex);
    thread = sControllerThread;
  }
  if (!thread) {
    NS_WARNING("Dropping task posted to controller thread");
    return;
  }
  if (aDelayMs) {
    thread->DelayedDispatch(std::move(aRunnable), aDelayMs);
  } else {
    thread->Dispatch(std::move(aRunnable));
  }
}

}  
}  
