/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DecodePool.h"

#include <algorithm>

#include "Decoder.h"
#include "IDecodingTask.h"
#include "RasterImage.h"
#include "mozilla/AppShutdown.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Monitor.h"
#include "mozilla/SchedulerGroup.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPrefs_image.h"
#include "mozilla/TaskController.h"
#include "mozilla/TimeStamp.h"
#include "nsCOMPtr.h"
#include "nsIObserverService.h"
#include "nsThreadManager.h"
#include "nsThreadUtils.h"
#include "nsXPCOMCIDInternal.h"
#include "prsystem.h"


using std::max;
using std::min;

namespace mozilla {
namespace image {


StaticRefPtr<DecodePool> DecodePool::sSingleton;
uint32_t DecodePool::sNumCores = 0;

NS_IMPL_ISUPPORTS(DecodePool, nsIObserver)

void DecodePool::Initialize() {
  MOZ_ASSERT(NS_IsMainThread());
  sNumCores = max<int32_t>(PR_GetNumberOfProcessors(), 1);
  DecodePool::Singleton();
}

DecodePool* DecodePool::Singleton() {
  if (!sSingleton) {
    MOZ_ASSERT(NS_IsMainThread());
    sSingleton = new DecodePool();
    ClearOnShutdown(&sSingleton);
  }

  return sSingleton;
}

uint32_t DecodePool::NumberOfCores() { return sNumCores; }


DecodePool::DecodePool() : mMutex("image::IOThread") {
  nsresult rv = NS_NewNamedThread("ImageIO", getter_AddRefs(mIOThread));
  MOZ_RELEASE_ASSERT(NS_SUCCEEDED(rv) && mIOThread,
                     "Should successfully create image I/O thread");

  nsCOMPtr<nsIObserverService> obsSvc = services::GetObserverService();
  if (obsSvc) {
    obsSvc->AddObserver(this, "xpcom-shutdown-threads", false);
  }
}

DecodePool::~DecodePool() {
  MOZ_ASSERT(NS_IsMainThread(), "Must shut down DecodePool on main thread!");
}

NS_IMETHODIMP
DecodePool::Observe(nsISupports*, const char* aTopic, const char16_t*) {
  MOZ_ASSERT(strcmp(aTopic, "xpcom-shutdown-threads") == 0, "Unexpected topic");

  mShuttingDown = true;

  nsCOMPtr<nsIThread> ioThread;

  {
    MutexAutoLock lock(mMutex);
    ioThread.swap(mIOThread);
  }

  if (ioThread) {
    ioThread->Shutdown();
  }

  return NS_OK;
}

 bool DecodePool::IsShuttingDown() {
  if (MOZ_UNLIKELY(!sSingleton)) {
    return AppShutdown::IsInOrBeyond(ShutdownPhase::XPCOMShutdownThreads);
  }

  return sSingleton->mShuttingDown;
}

class DecodingTask final : public Task {
 public:
  explicit DecodingTask(RefPtr<IDecodingTask>&& aTask)
      : Task(Kind::OffMainThreadOnly, aTask->Priority() == TaskPriority::eLow
                                          ? EventQueuePriority::Normal
                                          : EventQueuePriority::RenderBlocking),
        mTask(aTask) {}

  TaskResult Run() override {
    if (MOZ_LIKELY(!DecodePool::IsShuttingDown())) {
      mTask->Run();
    }
    return TaskResult::Complete;
  }

#if defined(MOZ_COLLECTING_RUNNABLE_TELEMETRY)
  bool GetName(nsACString& aName) override {
    aName.AssignLiteral("ImageDecodingTask");
    return true;
  }
#endif

 private:
  RefPtr<IDecodingTask> mTask;
};

void DecodePool::AsyncRun(IDecodingTask* aTask) {
  MOZ_ASSERT(aTask);

  TaskController::Get()->AddTask(
      MakeAndAddRef<DecodingTask>((RefPtr<IDecodingTask>(aTask))));
}

bool DecodePool::SyncRunIfPreferred(IDecodingTask* aTask,
                                    const nsCString& aURI) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aTask);


  if (aTask->ShouldPreferSyncRun()) {
    aTask->Run();
    return true;
  }

  AsyncRun(aTask);
  return false;
}

void DecodePool::SyncRunIfPossible(IDecodingTask* aTask,
                                   const nsCString& aURI) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aTask);


  aTask->Run();
}

already_AddRefed<nsISerialEventTarget> DecodePool::GetIOEventTarget() {
  MutexAutoLock threadPoolLock(mMutex);
  nsCOMPtr<nsISerialEventTarget> target = mIOThread;
  return target.forget();
}

}  
}  
