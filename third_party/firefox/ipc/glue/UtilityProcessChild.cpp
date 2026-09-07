/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "UtilityProcessChild.h"

#include "mozilla/AppShutdown.h"
#include "mozilla/Logging.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/JSOracleChild.h"
#include "mozilla/dom/MemoryReportRequest.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/UtilityProcessManager.h"
#include "mozilla/ipc/UtilityProcessTypes.h"
#include "mozilla/Preferences.h"
#include "mozilla/RemoteMediaManagerParent.h"






#include "nsDebugImpl.h"
#include "nsIXULRuntime.h"
#include "nsThreadManager.h"
#include "mozilla/ipc/ProcessChild.h"

#include "mozilla/Services.h"

namespace mozilla::ipc {

using namespace layers;

static StaticMutex sUtilityProcessChildMutex;
static StaticRefPtr<UtilityProcessChild> sUtilityProcessChild
    MOZ_GUARDED_BY(sUtilityProcessChildMutex);

UtilityProcessChild::UtilityProcessChild() : mChildStartTime(TimeStamp::Now()) {
  nsDebugImpl::SetMultiprocessMode("Utility");
}

UtilityProcessChild::~UtilityProcessChild() = default;

RefPtr<UtilityProcessChild> UtilityProcessChild::GetSingleton() {
  MOZ_ASSERT(XRE_IsUtilityProcess());
  if (AppShutdown::IsInOrBeyond(ShutdownPhase::XPCOMShutdownFinal)) {
    return nullptr;
  }
  StaticMutexAutoLock lock(sUtilityProcessChildMutex);
  if (!sUtilityProcessChild) {
    sUtilityProcessChild = new UtilityProcessChild();
  }
  return sUtilityProcessChild;
}

RefPtr<UtilityProcessChild> UtilityProcessChild::Get() {
  StaticMutexAutoLock lock(sUtilityProcessChildMutex);
  return sUtilityProcessChild;
}

bool UtilityProcessChild::Init(mozilla::ipc::UntypedEndpoint&& aEndpoint,
                               const nsCString& aParentBuildID,
                               UtilityProcessKind aUtilityProcessKind) {
  MOZ_ASSERT(NS_IsMainThread());

  if (NS_WARN_IF(NS_FAILED(nsThreadManager::get().Init()))) {
    return false;
  }

  if (NS_WARN_IF(!aEndpoint.Bind(this))) {
    return false;
  }

  MessageChannel* channel = GetIPCChannel();
  if (channel && !channel->SendBuildIDsMatchMessage(aParentBuildID.get())) {
    ipc::ProcessChild::QuickExit();
  }

  if (NS_FAILED(NS_InitMinimalXPCOM())) {
    return false;
  }

  mKind = aUtilityProcessKind;

  if (mKind == UtilityProcessKind::GENERIC_UTILITY) {
    if (!JS_FrontendOnlyInit()) {
      return false;
    }
  }

  SendInitCompleted();


  RunOnShutdown(
      [kind = mKind] {
        StaticMutexAutoLock lock(sUtilityProcessChildMutex);
        sUtilityProcessChild = nullptr;
        if (kind == UtilityProcessKind::GENERIC_UTILITY) {
          JS_FrontendOnlyShutDown();
        }
      },
      ShutdownPhase::XPCOMShutdownFinal);

  return true;
}


mozilla::ipc::IPCResult UtilityProcessChild::RecvInit(
    const Maybe<FileDescriptor>& aBrokerFd) {
  mozilla::ipc::SetThisProcessName("Utility Process");


  return IPC_OK();
}

mozilla::ipc::IPCResult UtilityProcessChild::RecvPreferenceUpdate(
    const Pref& aPref) {
  Preferences::SetPreference(aPref);
  return IPC_OK();
}

mozilla::ipc::IPCResult UtilityProcessChild::RecvRequestMemoryReport(
    const uint32_t& aGeneration, const bool& aAnonymize,
    const bool& aMinimizeMemoryUsage, const Maybe<FileDescriptor>& aDMDFile,
    const RequestMemoryReportResolver& aResolver) {
  nsPrintfCString processName("Utility (pid %" PRIPID
                              ", utilityProcessKind %" PRIu64 ")",
                              base::GetCurrentProcId(), mKind);

  mozilla::dom::MemoryReportRequestClient::Start(
      aGeneration, aAnonymize, aMinimizeMemoryUsage, aDMDFile, processName,
      [&](const MemoryReport& aReport) {
        (void)GetSingleton()->SendAddMemoryReport(aReport);
      },
      aResolver);
  return IPC_OK();
}

#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
IPCResult UtilityProcessChild::RecvStartPKCS11ModuleService(
    Endpoint<PPKCS11ModuleChild>&& aEndpoint, nsCString&& aProfilePath) {
  auto child = MakeRefPtr<psm::PKCS11ModuleChild>();
  if (!child ||
      NS_FAILED(child->Start(std::move(aEndpoint), std::move(aProfilePath)))) {
    return IPC_FAIL(this, "Failed to create and start PKCS11ModuleChild");
  }

  mPKCS11ModuleInstance = std::move(child);
  return IPC_OK();
}
#endif  // NIGHTLY_BUILD && !MOZ_NO_SMART_CARDS


mozilla::ipc::IPCResult UtilityProcessChild::RecvStartUtilityMediaService(
    Endpoint<PUtilityMediaServiceParent>&& aEndpoint,
    nsTArray<gfx::GfxVarUpdate>&& aUpdates) {
  mUtilityMediaServiceInstance =
      new UtilityMediaServiceParent(std::move(aUpdates));
  if (!mUtilityMediaServiceInstance) {
    return IPC_FAIL(this, "Failed to create UtilityMediaServiceParent");
  }

  mUtilityMediaServiceInstance->Start(std::move(aEndpoint));
  return IPC_OK();
}

mozilla::ipc::IPCResult UtilityProcessChild::RecvStartJSOracleService(
    Endpoint<PJSOracleChild>&& aEndpoint) {
  mJSOracleInstance = new mozilla::dom::JSOracleChild();
  if (!mJSOracleInstance) {
    return IPC_FAIL(this, "Failed to create JSOracleParent");
  }

  mJSOracleInstance->Start(std::move(aEndpoint));
  return IPC_OK();
}


void UtilityProcessChild::ActorDestroy(ActorDestroyReason aWhy) {

  if (AbnormalShutdown == aWhy) {
    NS_WARNING("Shutting down Utility process early due to a crash!");
    ipc::ProcessChild::QuickExit();
  }

#ifndef NS_FREE_PERMANENT_DATA
  ProcessChild::QuickExit();
#else

  if (mProfilerController) {
    mProfilerController->Shutdown();
    mProfilerController = nullptr;
  }

  uint32_t timeout = 0;
  if (mUtilityMediaServiceInstance) {
    mUtilityMediaServiceInstance = nullptr;
    timeout = 10 * 1000;
  }

  mJSOracleInstance = nullptr;


#  if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
  mPKCS11ModuleInstance = nullptr;
#  endif  // NIGHTLY_BUILD && !MOZ_NO_SMART_CARDS

  mShutdownBlockers.WaitUntilClear(timeout)->Then(
      GetCurrentSerialEventTarget(), __func__, [&]() {

        XRE_ShutdownChildProcess();
      });
#endif    // NS_FREE_PERMANENT_DATA
}

}  
