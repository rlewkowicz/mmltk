/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GPUProcessHost.h"
#include "chrome/common/process_watcher.h"
#include "gfxPlatform.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/gfx/GPUChild.h"
#include "mozilla/gfx/Logging.h"
#include "mozilla/layers/SynchronousTask.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_layers.h"
#include "mozilla/ipc/ProcessUtils.h"

namespace mozilla {
namespace gfx {


using namespace ipc;

GPUProcessHost::GPUProcessHost(Listener* aListener)
    : GeckoChildProcessHost(GeckoProcessType_GPU),
      mListener(aListener),
      mLaunchPhase(LaunchPhase::Unlaunched),
      mProcessToken(0),
      mShutdownRequested(false),
      mChannelClosed(false),
      mLiveToken(new media::Refcountable<bool>(true)) {
  MOZ_COUNT_CTOR(GPUProcessHost);

}

GPUProcessHost::~GPUProcessHost() { MOZ_COUNT_DTOR(GPUProcessHost); }

bool GPUProcessHost::Launch(geckoargs::ChildProcessArgs aExtraOpts) {
  MOZ_ASSERT(mLaunchPhase == LaunchPhase::Unlaunched);
  MOZ_ASSERT(!mGPUChild);
  mPrefSerializer = MakeUnique<ipc::SharedPreferenceSerializer>();
  if (!mPrefSerializer->SerializeToSharedMemory(GeckoProcessType_GPU,
                                                 ""_ns)) {
    return false;
  }
  mPrefSerializer->AddSharedPrefCmdLineArgs(*this, aExtraOpts);


  mLaunchPhase = LaunchPhase::Waiting;
  mLaunchTime = TimeStamp::Now();

  if (!GeckoChildProcessHost::AsyncLaunch(std::move(aExtraOpts))) {
    mLaunchPhase = LaunchPhase::Complete;
    mPrefSerializer = nullptr;
    return false;
  }

  return true;
}

bool GPUProcessHost::WaitForLaunch() {
  MOZ_ASSERT(mLaunchPhase != LaunchPhase::Unlaunched);
  if (mLaunchPhase == LaunchPhase::Complete) {
    return !!mGPUChild;
  }

  int32_t timeoutMs =
      StaticPrefs::layers_gpu_process_startup_timeout_ms_AtStartup();

  if (PR_GetEnv("MOZ_DEBUG_CHILD_PROCESS") ||
      PR_GetEnv("MOZ_DEBUG_CHILD_PAUSE")) {
    timeoutMs = 0;
  }

  if (mLaunchPhase == LaunchPhase::Waiting) {
    bool result = GeckoChildProcessHost::WaitUntilConnected(timeoutMs);
    InitAfterConnect(result);
    if (!result) {
      return false;
    }
  }
  MOZ_ASSERT(mLaunchPhase == LaunchPhase::Connected);
  return CompleteInitSynchronously();
}

void GPUProcessHost::OnProcessLaunchError(const base::LaunchError aError) {
  bool oom = false;

  gfxCriticalNote << "GPU proc launch error " << aError.FunctionName().get()
                  << (oom ? " OOM " : " ") << gfx::hexa(aError.ErrorCode());

  MonitorAutoLock lock(mMonitor);
  mProcessState = PROCESS_ERROR;
  mLaunchOomError = oom;
  lock.Notify();
}

void GPUProcessHost::OnChannelConnected(base::ProcessId peer_pid) {
  MOZ_ASSERT(!NS_IsMainThread());

  GeckoChildProcessHost::OnChannelConnected(peer_pid);

  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "GPUProcessHost::OnChannelConnected",
      [self = this, liveToken = mLiveToken]() {
        if (*liveToken && self->mLaunchPhase == LaunchPhase::Waiting) {
          self->InitAfterConnect(true);
        }
      }));
}

static uint64_t sProcessTokenCounter = 0;

void GPUProcessHost::InitAfterConnect(bool aSucceeded) {
  MOZ_ASSERT(mLaunchPhase == LaunchPhase::Waiting);
  MOZ_ASSERT(!mGPUChild);

  mPrefSerializer = nullptr;

  if (aSucceeded) {
    mLaunchPhase = LaunchPhase::Connected;
    mProcessToken = ++sProcessTokenCounter;
    mGPUChild = MakeRefPtr<GPUChild>(this);
    DebugOnly<bool> rv = TakeInitialEndpoint().Bind(mGPUChild.get());
    MOZ_ASSERT(rv);

    nsTArray<RefPtr<GPUChild::InitPromiseType>> initPromises;
    initPromises.AppendElement(mGPUChild->Init());


    GPUChild::InitPromiseType::All(GetCurrentSerialEventTarget(), initPromises)
        ->Then(GetCurrentSerialEventTarget(), __func__,
               [self = this, liveToken = mLiveToken]() {
                 if (*liveToken) {
                   self->OnAsyncInitComplete();
                 }
               });
  } else {
    mLaunchPhase = LaunchPhase::Complete;
    if (mListener) {
      mListener->OnProcessLaunchComplete(this);
    }
  }
}

void GPUProcessHost::OnAsyncInitComplete() {
  MOZ_ASSERT(NS_IsMainThread());
  if (mLaunchPhase == LaunchPhase::Connected) {
    mLaunchPhase = LaunchPhase::Complete;
    if (mListener) {
      mListener->OnProcessLaunchComplete(this);
    }
  }
}

bool GPUProcessHost::CompleteInitSynchronously() {
  MOZ_ASSERT(mLaunchPhase == LaunchPhase::Connected);

  const bool result = mGPUChild->EnsureGPUReady();


  mLaunchPhase = LaunchPhase::Complete;
  if (mListener) {
    mListener->OnProcessLaunchComplete(this);
  }

  return result;
}

void GPUProcessHost::Shutdown(bool aUnexpectedShutdown) {
  MOZ_ASSERT(!mShutdownRequested);

  mListener = nullptr;

  if (mGPUChild) {
    mShutdownRequested = true;

    if (!mChannelClosed) {
      mGPUChild->Close();
    }

#ifndef NS_FREE_PERMANENT_DATA
    KillHard();
#endif

    return;
  }

  DestroyProcess();
}

void GPUProcessHost::OnChannelClosed() {
  mChannelClosed = true;

  if (!mShutdownRequested && mListener) {
    mListener->OnProcessUnexpectedShutdown(this);
  } else {
    DestroyProcess();
  }

  GPUChild::Destroy(std::move(mGPUChild));
  MOZ_ASSERT(!mGPUChild);
}

void GPUProcessHost::KillHard() {
  MOZ_ASSERT(NS_IsMainThread());

  const ProcessHandle handle = GetChildProcessHandle();
  if (!base::KillProcess(handle, base::PROCESS_END_KILLED_BY_USER)) {
    NS_WARNING("failed to kill subprocess!");
  }

  SetAlreadyDead();
}

uint64_t GPUProcessHost::GetProcessToken() const { return mProcessToken; }

void GPUProcessHost::KillProcess() { KillHard(); }

void GPUProcessHost::CrashProcess() { mGPUChild->SendCrashProcess(); }

void GPUProcessHost::DestroyProcess() {
  MOZ_ASSERT(NS_IsMainThread());

  *mLiveToken = false;

  NS_DispatchToMainThread(
      NS_NewRunnableFunction("DestroyProcessRunnable", [this] { Destroy(); }));
}



}  
}  
