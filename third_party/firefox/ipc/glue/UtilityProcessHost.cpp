/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "UtilityProcessHost.h"

#include "mozilla/dom/ContentParent.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/UtilityProcessManager.h"
#include "mozilla/ipc/UtilityProcessTypes.h"

#include "chrome/common/process_watcher.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_general.h"





namespace mozilla::ipc {

LazyLogModule gUtilityProcessLog("utilityproc");
#define LOGD(...) MOZ_LOG(gUtilityProcessLog, LogLevel::Debug, (__VA_ARGS__))

UtilityProcessHost::UtilityProcessHost(UtilityProcessKind aKind,
                                       RefPtr<Listener> aListener)
    : GeckoChildProcessHost(GeckoProcessType_Utility),
      mListener(std::move(aListener)),
      mLiveToken(new media::Refcountable<bool>(true)),
      mLaunchPromise(MakeRefPtr<LaunchPromiseType::Private>(__func__)) {
  MOZ_COUNT_CTOR(UtilityProcessHost);
  LOGD("[%p] UtilityProcessHost::UtilityProcessHost utilityProcessKind=%" PRIu64,
       this, aKind);

  mUtilityKind = aKind;
}

UtilityProcessHost::~UtilityProcessHost() {
  MOZ_COUNT_DTOR(UtilityProcessHost);
  LOGD("[%p] UtilityProcessHost::~UtilityProcessHost utilityProcessKind=%" PRIu64,
       this, mUtilityKind);
}

bool UtilityProcessHost::Launch(geckoargs::ChildProcessArgs aExtraOpts) {
  MOZ_ASSERT(NS_IsMainThread());

  MOZ_ASSERT(mLaunchPhase == LaunchPhase::Unlaunched);
  MOZ_ASSERT(!mUtilityProcessParent);

  LOGD("[%p] UtilityProcessHost::Launch", this);

  mPrefSerializer = MakeUnique<ipc::SharedPreferenceSerializer>();
  if (!mPrefSerializer->SerializeToSharedMemory(GeckoProcessType_Utility,
                                                 ""_ns)) {
    return false;
  }
  mPrefSerializer->AddSharedPrefCmdLineArgs(*this, aExtraOpts);

  mLaunchPhase = LaunchPhase::Waiting;

  if (!GeckoChildProcessHost::AsyncLaunch(std::move(aExtraOpts))) {
    NS_WARNING("UtilityProcess AsyncLaunch failed, aborting.");
    mLaunchPhase = LaunchPhase::Complete;
    mPrefSerializer = nullptr;
    return false;
  }
  LOGD("[%p] UtilityProcessHost::Launch launching async", this);
  return true;
}

RefPtr<UtilityProcessHost::LaunchPromiseType>
UtilityProcessHost::LaunchPromise() {
  MOZ_ASSERT(NS_IsMainThread());

  if (mLaunchPromiseLaunched) {
    return mLaunchPromise;
  }

  WhenProcessHandleReady()->Then(
      GetCurrentSerialEventTarget(), __func__,
      [this, liveToken = mLiveToken](
          const ipc::ProcessHandlePromise::ResolveOrRejectValue& aResult) {
        if (!*liveToken) {
          return;
        }
        if (mLaunchCompleted) {
          return;
        }
        mLaunchCompleted = true;
        if (aResult.IsReject()) {
          RejectPromise(aResult.RejectValue());
        }
      });

  mLaunchPromiseLaunched = true;
  return mLaunchPromise;
}

void UtilityProcessHost::OnChannelConnected(base::ProcessId peer_pid) {
  MOZ_ASSERT(!NS_IsMainThread());
  LOGD("[%p] UtilityProcessHost::OnChannelConnected", this);

  GeckoChildProcessHost::OnChannelConnected(peer_pid);

  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "UtilityProcessHost::OnChannelConnected",
      [this, liveToken = mLiveToken]() {
        if (*liveToken && mLaunchPhase == LaunchPhase::Waiting) {
          InitAfterConnect(true);
        }
      }));
}

void UtilityProcessHost::InitAfterConnect(bool aSucceeded) {
  MOZ_ASSERT(NS_IsMainThread());

  MOZ_ASSERT(mLaunchPhase == LaunchPhase::Waiting);
  MOZ_ASSERT(!mUtilityProcessParent);

  MOZ_ASSERT(aSucceeded);

  mLaunchPhase = LaunchPhase::Complete;

  mUtilityProcessParent = MakeRefPtr<UtilityProcessParent>(this);
  DebugOnly<bool> rv = TakeInitialEndpoint().Bind(mUtilityProcessParent.get());
  MOZ_ASSERT(rv);

  mPrefSerializer = nullptr;

  Maybe<FileDescriptor> brokerFd;


  (void)GetActor()->SendInit(brokerFd);

  LOGD("[%p] UtilityProcessHost::InitAfterConnect succeeded", this);

}

void UtilityProcessHost::Shutdown() {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mShutdownRequested);
  LOGD("[%p] UtilityProcessHost::Shutdown", this);

  RejectPromise(LaunchError("aborted by UtilityProcessHost::Shutdown"));

  if (mUtilityProcessParent) {
    LOGD("[%p] UtilityProcessHost::Shutdown not destroying utility process.",
         this);

    mShutdownRequested = true;

    if (mUtilityProcessParent->CanSend()) {
      mUtilityProcessParent->Close();
    }

#ifndef NS_FREE_PERMANENT_DATA
    KillHard("NormalShutdown");
#endif

    return;
  }

  DestroyProcess();
}

void UtilityProcessHost::OnChannelClosed(
    IProtocol::ActorDestroyReason aReason) {
  MOZ_ASSERT(NS_IsMainThread());
  LOGD("[%p] UtilityProcessHost::OnChannelClosed", this);

  RejectPromise(
      LaunchError("UtilityProcessHost::OnChannelClosed", 1 + (long)aReason));

  if (!mShutdownRequested && mListener) {
    mListener->OnProcessUnexpectedShutdown(this);
  }

  DestroyProcess();

  UtilityProcessParent::Destroy(std::move(mUtilityProcessParent));
}

void UtilityProcessHost::KillHard(const char* aReason) {
  MOZ_ASSERT(NS_IsMainThread());
  LOGD("[%p] UtilityProcessHost::KillHard", this);

  ProcessHandle handle = GetChildProcessHandle();
  if (!base::KillProcess(handle, base::PROCESS_END_KILLED_BY_USER)) {
    NS_WARNING("failed to kill subprocess!");
  }

  SetAlreadyDead();
}

void UtilityProcessHost::DestroyProcess() {
  MOZ_ASSERT(NS_IsMainThread());
  LOGD("[%p] UtilityProcessHost::DestroyProcess", this);

  RejectPromise(LaunchError("UtilityProcessHost::DestroyProcess"));

  *mLiveToken = false;

  NS_DispatchToMainThread(
      NS_NewRunnableFunction("DestroyProcessRunnable", [this] { Destroy(); }));
}

void UtilityProcessHost::ResolvePromise() {
  MOZ_ASSERT(NS_IsMainThread());
  LOGD("[%p] UtilityProcessHost connected - resolving launch promise", this);

  if (!mLaunchPromiseSettled) {
    mLaunchPromise->Resolve(Ok{}, __func__);
    mLaunchPromiseSettled = true;
  }

  mLaunchCompleted = true;
}

void UtilityProcessHost::RejectPromise(LaunchError err) {
  MOZ_ASSERT(NS_IsMainThread());
  LOGD("[%p] UtilityProcessHost connection failed - rejecting launch promise",
       this);

  if (!mLaunchPromiseSettled) {
    mLaunchPromise->Reject(std::move(err), __func__);
    mLaunchPromiseSettled = true;
  }

  mLaunchCompleted = true;
}


}  
