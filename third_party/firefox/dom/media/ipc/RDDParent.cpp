/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "RDDParent.h"

#  include <unistd.h>

#include "MediaCodecsSupport.h"
#include "gfxConfig.h"
#include "mozilla/Assertions.h"
#include "mozilla/Preferences.h"
#include "mozilla/RemoteMediaManagerParent.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/dom/MemoryReportRequest.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/ipc/ProcessChild.h"




#include "mozilla/ipc/ProcessUtils.h"
#include "nsDebugImpl.h"
#include "nsIObserverService.h"
#include "nsIXULRuntime.h"
#include "nsThreadManager.h"


#  include "VideoUtils.h"

#if defined(MOZ_WIDGET_GTK)
#  include "mozilla/widget/DMABufSurface.h"
#endif

namespace mozilla {

using namespace ipc;
using namespace gfx;

static RDDParent* sRDDParent;

RDDParent::RDDParent() : mLaunchTime(TimeStamp::Now()) { sRDDParent = this; }

RDDParent::~RDDParent() { sRDDParent = nullptr; }

RDDParent* RDDParent::GetSingleton() {
  MOZ_DIAGNOSTIC_ASSERT(sRDDParent);
  return sRDDParent;
}

bool RDDParent::Init(mozilla::ipc::UntypedEndpoint&& aEndpoint,
                     const char* aParentBuildID) {
  if (NS_WARN_IF(NS_FAILED(nsThreadManager::get().Init()))) {
    return false;
  }

  if (NS_WARN_IF(!aEndpoint.Bind(this))) {
    return false;
  }

  nsDebugImpl::SetMultiprocessMode("RDD");

  MessageChannel* channel = GetIPCChannel();
  if (channel && !channel->SendBuildIDsMatchMessage(aParentBuildID)) {
    ProcessChild::QuickExit();
  }

  if (NS_FAILED(NS_InitMinimalXPCOM())) {
    return false;
  }

  gfxConfig::Init();
  gfxVars::Initialize();

  mozilla::ipc::SetThisProcessName("RDD Process");

  return true;
}


mozilla::ipc::IPCResult RDDParent::RecvInit(
    nsTArray<GfxVarUpdate>&& vars, const Maybe<FileDescriptor>& aBrokerFd) {
  gfxVars::ApplyUpdate(vars);

  auto supported = media::MCSInfo::GetSupportFromFactory();
  (void)SendUpdateMediaCodecsSupported(supported);


  return IPC_OK();
}

IPCResult RDDParent::RecvUpdateVar(const nsTArray<GfxVarUpdate>& aUpdate) {
  gfxVars::ApplyUpdate(aUpdate);

  MOZ_ALWAYS_SUCCEEDS(NS_DispatchBackgroundTask(
      NS_NewRunnableFunction(
          "RDDParent::RecvUpdateVar",
          []() {
            NS_DispatchToMainThread(NS_NewRunnableFunction(
                "RDDParent::UpdateMediaCodecsSupported",
                [supported = media::MCSInfo::GetSupportFromFactory(
                     true )]() {
                  if (auto* rdd = RDDParent::GetSingleton()) {
                    (void)rdd->SendUpdateMediaCodecsSupported(supported);
                  }
                }));
          }),
      nsIEventTarget::DISPATCH_NORMAL));
  return IPC_OK();
}

mozilla::ipc::IPCResult RDDParent::RecvNewContentRemoteMediaManager(
    Endpoint<PRemoteMediaManagerParent>&& aEndpoint,
    const ContentParentId& aParentId) {
  if (!RemoteMediaManagerParent::CreateForContent(std::move(aEndpoint),
                                                  aParentId)) {
    return IPC_FAIL_NO_REASON(this);
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult RDDParent::RecvInitVideoBridge(
    Endpoint<PVideoBridgeChild>&& aEndpoint, const bool& aCreateHardwareDevice,
    const ContentDeviceData& aContentDeviceData) {
  if (!RemoteMediaManagerParent::CreateVideoBridgeToOtherProcess(
          std::move(aEndpoint))) {
    return IPC_FAIL_NO_REASON(this);
  }

  gfxConfig::Inherit(
      {
          Feature::HW_COMPOSITING,
          Feature::D3D11_COMPOSITING,
          Feature::OPENGL_COMPOSITING,
      },
      aContentDeviceData.prefs());

  return IPC_OK();
}

mozilla::ipc::IPCResult RDDParent::RecvRequestMemoryReport(
    const uint32_t& aGeneration, const bool& aAnonymize,
    const bool& aMinimizeMemoryUsage, const Maybe<FileDescriptor>& aDMDFile,
    const RequestMemoryReportResolver& aResolver) {
  nsPrintfCString processName("RDD (pid %u)", (unsigned)getpid());

  mozilla::dom::MemoryReportRequestClient::Start(
      aGeneration, aAnonymize, aMinimizeMemoryUsage, aDMDFile, processName,
      [&](const MemoryReport& aReport) {
        (void)GetSingleton()->SendAddMemoryReport(aReport);
      },
      aResolver);
  return IPC_OK();
}


mozilla::ipc::IPCResult RDDParent::RecvPreferenceUpdate(const Pref& aPref) {
  Preferences::SetPreference(aPref);
  return IPC_OK();
}


void RDDParent::ActorDestroy(ActorDestroyReason aWhy) {

  if (AbnormalShutdown == aWhy) {
    NS_WARNING("Shutting down RDD process early due to a crash!");
    ProcessChild::QuickExit();
  }

#ifndef NS_FREE_PERMANENT_DATA
  ProcessChild::QuickExit();
#endif

  mShutdownBlockers.WaitUntilClear(10 * 1000 )
      ->Then(GetCurrentSerialEventTarget(), __func__, [&]() {


        RemoteMediaManagerParent::ShutdownVideoBridge();

#if defined(MOZ_WIDGET_GTK)
        DMABufSurface::ReleaseSnapshotGLContext();
#endif

        gfxVars::Shutdown();
        gfxConfig::Shutdown();
        XRE_ShutdownChildProcess();
      });
}

}  
