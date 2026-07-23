/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SocketProcessHost.h"

#include "SocketProcessParent.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/ipc/FileDescriptor.h"
#include "mozilla/ipc/ProcessUtils.h"
#include "nsAppRunner.h"
#include "nsIOService.h"
#include "nsIObserverService.h"
#include "nsNetUtil.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/ProcessChild.h"




using namespace mozilla::ipc;

namespace mozilla {
namespace net {


SocketProcessHost::SocketProcessHost(Listener* aListener)
    : GeckoChildProcessHost(GeckoProcessType_Socket),
      mListener(aListener),
      mTaskFactory(Some(this)),
      mLaunchPhase(LaunchPhase::Unlaunched),
      mShutdownRequested(false),
      mChannelClosed(false) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_COUNT_CTOR(SocketProcessHost);
}

SocketProcessHost::~SocketProcessHost() { MOZ_COUNT_DTOR(SocketProcessHost); }

bool SocketProcessHost::Launch() {
  MOZ_ASSERT(mLaunchPhase == LaunchPhase::Unlaunched);
  MOZ_ASSERT(!mSocketProcessParent);
  MOZ_ASSERT(NS_IsMainThread());

  geckoargs::ChildProcessArgs extraArgs;
  ProcessChild::AddPlatformBuildID(extraArgs);

  SharedPreferenceSerializer prefSerializer;
  if (!prefSerializer.SerializeToSharedMemory(GeckoProcessType_Socket,
                                               ""_ns)) {
    return false;
  }
  prefSerializer.AddSharedPrefCmdLineArgs(*this, extraArgs);

  mLaunchPhase = LaunchPhase::Waiting;
  if (!GeckoChildProcessHost::LaunchAndWaitForProcessHandle(
          std::move(extraArgs))) {
    mLaunchPhase = LaunchPhase::Complete;
    return false;
  }

  return true;
}

static void HandleErrorAfterDestroy(
    RefPtr<SocketProcessHost::Listener>&& aListener) {
  if (!aListener) {
    return;
  }

  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "HandleErrorAfterDestroy", [listener = std::move(aListener)]() {
        listener->OnProcessLaunchComplete(nullptr, false);
      }));
}

void SocketProcessHost::OnChannelConnected(base::ProcessId peer_pid) {
  MOZ_ASSERT(!NS_IsMainThread());

  GeckoChildProcessHost::OnChannelConnected(peer_pid);

  RefPtr<Runnable> runnable;
  {
    MonitorAutoLock lock(mMonitor);
    if (!mTaskFactory) {
      HandleErrorAfterDestroy(std::move(mListener));
      return;
    }
    runnable =
        (*mTaskFactory)
            .NewRunnableMethod(&SocketProcessHost::OnChannelConnectedTask);
  }
  NS_DispatchToMainThread(runnable);
}

void SocketProcessHost::OnChannelConnectedTask() {
  MOZ_ASSERT(NS_IsMainThread());

  if (mLaunchPhase == LaunchPhase::Waiting) {
    InitAfterConnect(true);
  }
}

void SocketProcessHost::InitAfterConnect(bool aSucceeded) {
  MOZ_ASSERT(mLaunchPhase == LaunchPhase::Waiting);
  MOZ_ASSERT(!mSocketProcessParent);
  MOZ_ASSERT(NS_IsMainThread());

  mLaunchPhase = LaunchPhase::Complete;
  if (!aSucceeded) {
    if (mListener) {
      mListener->OnProcessLaunchComplete(this, false);
    }
    return;
  }

  mSocketProcessParent = MakeRefPtr<SocketProcessParent>(this);
  DebugOnly<bool> rv = TakeInitialEndpoint().Bind(mSocketProcessParent.get());
  MOZ_ASSERT(rv);

  SocketPorcessInitAttributes attributes;
  nsCOMPtr<nsIIOService> ioService(do_GetIOService());
  MOZ_ASSERT(ioService, "No IO service?");
  DebugOnly<nsresult> result = ioService->GetOffline(&attributes.mOffline());
  MOZ_ASSERT(NS_SUCCEEDED(result), "Failed getting offline?");
  result = ioService->GetConnectivity(&attributes.mConnectivity());
  MOZ_ASSERT(NS_SUCCEEDED(result), "Failed getting connectivity?");

  attributes.mInitSandbox() = false;



  (void)GetActor()->SendInit(attributes);

  if (mListener) {
    mListener->OnProcessLaunchComplete(this, true);
  }
}

void SocketProcessHost::Shutdown() {
  MOZ_ASSERT(!mShutdownRequested);
  MOZ_ASSERT(NS_IsMainThread());

  mListener = nullptr;

  if (mSocketProcessParent) {
    mShutdownRequested = true;

    if (!mChannelClosed) {
      mSocketProcessParent->Close();
    }

    return;
  }

  DestroyProcess();
}

void SocketProcessHost::OnChannelClosed() {
  MOZ_ASSERT(NS_IsMainThread());

  mChannelClosed = true;

  if (!mShutdownRequested && mListener) {
    mListener->OnProcessUnexpectedShutdown(this);
  } else {
    DestroyProcess();
  }

  SocketProcessParent::Destroy(std::move(mSocketProcessParent));
  MOZ_ASSERT(!mSocketProcessParent);
}

void SocketProcessHost::DestroyProcess() {
  {
    MonitorAutoLock lock(mMonitor);
    mTaskFactory.reset();
  }

  GetCurrentSerialEventTarget()->Dispatch(NS_NewRunnableFunction(
      "DestroySocketProcessRunnable", [this] { Destroy(); }));
}



bool SocketProcessMemoryReporter::IsAlive() const {
  MOZ_ASSERT(gIOService);

  if (!gIOService->mSocketProcess) {
    return false;
  }

  return gIOService->mSocketProcess->IsConnected();
}

bool SocketProcessMemoryReporter::SendRequestMemoryReport(
    const uint32_t& aGeneration, const bool& aAnonymize,
    const bool& aMinimizeMemoryUsage,
    const Maybe<ipc::FileDescriptor>& aDMDFile) {
  MOZ_ASSERT(gIOService);

  if (!gIOService->mSocketProcess) {
    return false;
  }

  SocketProcessParent* actor = gIOService->mSocketProcess->GetActor();
  if (!actor) {
    return false;
  }

  return actor->SendRequestMemoryReport(aGeneration, aAnonymize,
                                        aMinimizeMemoryUsage, aDMDFile);
}

int32_t SocketProcessMemoryReporter::Pid() const {
  MOZ_ASSERT(gIOService);
  return gIOService->SocketProcessPid();
}

}  
}  
