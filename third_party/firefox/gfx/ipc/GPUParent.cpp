/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FFVPXRuntimeLinker.h"
#include "GLContextProvider.h"
#include "GPUParent.h"
#include "GPUProcessHost.h"
#include "GPUProcessManager.h"
#include "gfxGradientCache.h"
#include "GfxInfoBase.h"
#include "VsyncBridgeParent.h"
#include "cairo.h"
#include "gfxConfig.h"
#include "gfxPlatform.h"
#include "MediaCodecsSupport.h"
#include "mozilla/Assertions.h"
#include "mozilla/Components.h"
#include "mozilla/Preferences.h"
#include "mozilla/ProcessPriorityManager.h"
#include "mozilla/RemoteMediaManagerChild.h"
#include "mozilla/RemoteMediaManagerParent.h"
#include "mozilla/StaticPrefs_dom.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/dom/MemoryReportRequest.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/CanvasRenderThread.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/image/ImageMemoryReporter.h"
#include "mozilla/ipc/ProcessChild.h"
#include "mozilla/ipc/ProcessUtils.h"
#include "mozilla/layers/APZInputBridgeParent.h"
#include "mozilla/layers/APZPublicUtils.h"  // for apz::InitializeGlobalState
#include "mozilla/layers/APZThreadUtils.h"
#include "mozilla/layers/CompositorBridgeParent.h"
#include "mozilla/layers/CompositorManagerParent.h"
#include "mozilla/layers/CompositorThread.h"
#include "mozilla/layers/ImageBridgeParent.h"
#include "mozilla/layers/LayerTreeOwnerTracker.h"
#include "mozilla/layers/RemoteTextureMap.h"
#include "mozilla/layers/UiCompositorControllerParent.h"
#include "mozilla/layers/VideoBridgeParent.h"
#include "mozilla/webrender/RenderThread.h"
#include "mozilla/webrender/WebRenderAPI.h"
#include "nsDebugImpl.h"
#include "nsIGfxInfo.h"
#include "nsIXULRuntime.h"
#include "nsThreadManager.h"
#include "nscore.h"
#include "prenv.h"
#include "skia/include/core/SkGraphics.h"
#  include <unistd.h>
#ifdef MOZ_WIDGET_GTK
#  include <gtk/gtk.h>

#  include "skia/include/ports/SkTypeface_cairo.h"
#endif
#include "nsAppRunner.h"


namespace mozilla::gfx {

using namespace ipc;
using namespace layers;

static GPUParent* sGPUParent;

GPUParent::GPUParent() : mLaunchTime(TimeStamp::Now()) { sGPUParent = this; }

GPUParent::~GPUParent() { sGPUParent = nullptr; }

GPUParent* GPUParent::GetSingleton() {
  MOZ_DIAGNOSTIC_ASSERT(sGPUParent);
  return sGPUParent;
}

 bool GPUParent::MaybeFlushMemory() {
  return false;
}

bool GPUParent::Init(mozilla::ipc::UntypedEndpoint&& aEndpoint,
                     const char* aParentBuildID) {
  if (NS_WARN_IF(NS_FAILED(nsThreadManager::get().Init()))) {
    return false;
  }

  if (NS_WARN_IF(!aEndpoint.Bind(this))) {
    return false;
  }

  nsDebugImpl::SetMultiprocessMode("GPU");

  MessageChannel* channel = GetIPCChannel();
  if (channel && !channel->SendBuildIDsMatchMessage(aParentBuildID)) {
    ProcessChild::QuickExit();
  }

  if (NS_FAILED(NS_InitMinimalXPCOM())) {
    return false;
  }

  ProcessPriorityManager::Init();

  gfxConfig::Init();
  gfxVars::Initialize();
  gfxPlatform::InitNullMetadata();
  gfxPlatform::InitMoz2DLogging();

  CompositorThreadHolder::Start();
  RemoteTextureMap::Init();
  APZThreadUtils::SetControllerThread(NS_GetCurrentThread());
  apz::InitializeGlobalState();
  LayerTreeOwnerTracker::Initialize();
  CompositorBridgeParent::InitializeStatics();

  mozilla::ipc::SetThisProcessName("GPU Process");

  return true;
}

void GPUParent::NotifyDeviceReset(DeviceResetReason aReason,
                                  DeviceResetDetectPlace aPlace) {
  GPUDeviceData data;
  RecvGetDeviceStatus(&data);
  (void)SendNotifyDeviceReset(data, aReason, aPlace);
}

void GPUParent::NotifyOverlayInfo(layers::OverlayInfo aInfo) {
  if (!NS_IsMainThread()) {
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "gfx::GPUParent::NotifyOverlayInfo", [aInfo]() -> void {
          GPUParent::GetSingleton()->NotifyOverlayInfo(aInfo);
        }));
    return;
  }
  (void)SendNotifyOverlayInfo(aInfo);
}

void GPUParent::NotifySwapChainInfo(layers::SwapChainInfo aInfo) {
  if (!NS_IsMainThread()) {
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "gfx::GPUParent::NotifySwapChainInfo", [aInfo]() -> void {
          GPUParent::GetSingleton()->NotifySwapChainInfo(aInfo);
        }));
    return;
  }
  (void)SendNotifySwapChainInfo(aInfo);
}

void GPUParent::NotifyDisableRemoteCanvas() {
  if (!NS_IsMainThread()) {
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "gfx::GPUParent::NotifyDisableRemoteCanvas", []() -> void {
          GPUParent::GetSingleton()->NotifyDisableRemoteCanvas();
        }));
    return;
  }
  (void)SendNotifyDisableRemoteCanvas();
}

void GPUParent::ReportGLStrings(GfxInfoGLStrings&& aStrings) {
  if (!NS_IsMainThread()) {
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "gfx::GPUParent::ReportGLStrings",
        [strings = std::move(aStrings)]() mutable -> void {
          GPUParent::GetSingleton()->ReportGLStrings(std::move(strings));
        }));
    return;
  }
  (void)SendReportGLStrings(std::move(aStrings));
}

mozilla::ipc::IPCResult GPUParent::RecvInit(
    nsTArray<GfxVarUpdate>&& vars, const DevicePrefs& devicePrefs,
    nsTArray<LayerTreeIdMapping>&& aMappings,
    nsTArray<GfxInfoFeatureStatus>&& aFeatures, uint32_t aWrNamespace,
    InitResolver&& aInitResolver) {
  gfxVars::ApplyUpdate(vars);

  gfxConfig::Inherit(Feature::HW_COMPOSITING, devicePrefs.hwCompositing());
  gfxConfig::Inherit(Feature::D3D11_COMPOSITING,
                     devicePrefs.d3d11Compositing());
  gfxConfig::Inherit(Feature::OPENGL_COMPOSITING, devicePrefs.oglCompositing());
  gfxConfig::Inherit(Feature::D3D11_HW_ANGLE, devicePrefs.d3d11HwAngle());

  for (const LayerTreeIdMapping& map : aMappings) {
    LayerTreeOwnerTracker::Get()->Map(map.layersId(), map.ownerId());
  }

  widget::GfxInfoBase::SetFeatureStatus(std::move(aFeatures));

  SkGraphics::Init();

  bool useRemoteCanvas = gfxVars::UseAcceleratedCanvas2D();
  if (useRemoteCanvas) {
    gfxGradientCache::Init();
  }


#if defined(MOZ_WIDGET_GTK)
  char* display_name = PR_GetEnv("MOZ_GDK_DISPLAY");
  if (!display_name) {
    bool waylandEnabled = false;
#  ifdef MOZ_WAYLAND
    waylandEnabled = IsWaylandEnabled();
#  endif
    if (!waylandEnabled) {
      display_name = PR_GetEnv("DISPLAY");
    }
  }
  if (display_name) {
    int argc = 3;
    char option_name[] = "--display";
    char* argv[] = {
                    nullptr, option_name, display_name, nullptr};
    char** argvp = argv;
    gtk_init(&argc, &argvp);
  } else {
    gtk_init(nullptr, nullptr);
  }

  FT_Library library = Factory::NewFTLibrary();
  MOZ_ASSERT(library);
  Factory::SetFTLibrary(library);

  SkInitCairoFT(true);

  nsCOMPtr<nsIGfxInfo> gfxInfo = components::GfxInfo::Service();
  (void)gfxInfo;
#endif


  wr::RenderThread::Start(aWrNamespace);
  gfx::CanvasRenderThread::Start();
  image::ImageMemoryReporter::InitForWebRender();

  gfxPlatform::InitMemoryReportersForGPUProcess();

  GPUDeviceData data;
  RecvGetDeviceStatus(&data);
  aInitResolver(data);

  MOZ_ALWAYS_SUCCEEDS(NS_DispatchBackgroundTask(
      NS_NewRunnableFunction(
          "GPUParent::Supported",
          []() {
            NS_DispatchToMainThread(NS_NewRunnableFunction(
                "GPUParent::UpdateMediaCodecsSupported",
                [supported = media::MCSInfo::GetSupportFromFactory()]() {
                  (void)GPUParent::GetSingleton()
                      ->SendUpdateMediaCodecsSupported(supported);
                }));
          }),
      nsIEventTarget::DISPATCH_NORMAL));

  return IPC_OK();
}


mozilla::ipc::IPCResult GPUParent::RecvInitCompositorManager(
    Endpoint<PCompositorManagerParent>&& aEndpoint, uint32_t aNamespace) {
  CompositorManagerParent::Create(std::move(aEndpoint), ContentParentId(),
                                  aNamespace,  true);
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvInitVsyncBridge(
    Endpoint<PVsyncBridgeParent>&& aVsyncEndpoint) {
  mVsyncBridge = VsyncBridgeParent::Start(std::move(aVsyncEndpoint));
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvInitImageBridge(
    Endpoint<PImageBridgeParent>&& aEndpoint, uint32_t aNamespace) {
  ImageBridgeParent::CreateForGPUProcess(std::move(aEndpoint), aNamespace);
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvInitVideoBridge(
    Endpoint<PVideoBridgeParent>&& aEndpoint,
    const layers::VideoBridgeSource& aSource) {
  MOZ_ASSERT(aSource == layers::VideoBridgeSource::RddProcess);
  VideoBridgeParent::Open(std::move(aEndpoint), aSource);
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvInitUiCompositorController(
    const LayersId& aRootLayerTreeId,
    Endpoint<PUiCompositorControllerParent>&& aEndpoint) {
  UiCompositorControllerParent::Start(aRootLayerTreeId, std::move(aEndpoint));
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvInitAPZInputBridge(
    const LayersId& aRootLayerTreeId,
    Endpoint<PAPZInputBridgeParent>&& aEndpoint) {
  APZInputBridgeParent::Create(aRootLayerTreeId, std::move(aEndpoint));
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvUpdateVar(
    const nsTArray<GfxVarUpdate>& aUpdate) {
  gfxVars::ApplyUpdate(aUpdate);
  MOZ_ALWAYS_SUCCEEDS(NS_DispatchBackgroundTask(
      NS_NewRunnableFunction(
          "GPUParent::RecvUpdateVar",
          []() {
            if (StaticPrefs::media_ffvpx_hw_enabled()) {
              FFVPXRuntimeLinker::Init();
            }
            NS_DispatchToMainThread(NS_NewRunnableFunction(
                "GPUParent::UpdateMediaCodecsSupported",
                [supported = media::MCSInfo::GetSupportFromFactory(
                     true )]() {
                  if (auto* gpu = GPUParent::GetSingleton()) {
                    (void)gpu->SendUpdateMediaCodecsSupported(supported);
                  }
                }));
          }),
      nsIEventTarget::DISPATCH_NORMAL));
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvPreferenceUpdate(const Pref& aPref) {
  Preferences::SetPreference(aPref);
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvScreenInformationChanged() {
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvNotifyBatteryInfo(
    const BatteryInformation& aBatteryInfo) {
  wr::RenderThread::Get()->SetBatteryInfo(aBatteryInfo);
  return IPC_OK();
}

static void CopyFeatureChange(Feature aFeature, Maybe<FeatureFailure>* aOut) {
  FeatureState& feature = gfxConfig::GetFeature(aFeature);
  if (feature.DisabledByDefault() || feature.IsEnabled()) {
    *aOut = Nothing();
    return;
  }

  MOZ_ASSERT(!feature.IsEnabled());

  nsCString message;
  message.AssignASCII(feature.GetFailureMessage());

  *aOut =
      Some(FeatureFailure(feature.GetValue(), message, feature.GetFailureId()));
}

mozilla::ipc::IPCResult GPUParent::RecvGetDeviceStatus(GPUDeviceData* aOut) {
  CopyFeatureChange(Feature::D3D11_COMPOSITING, &aOut->d3d11Compositing());
  CopyFeatureChange(Feature::OPENGL_COMPOSITING, &aOut->oglCompositing());

  aOut->gpuDevice() = Nothing();

  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvSimulateDeviceReset() {
  wr::RenderThread::Get()->SimulateDeviceReset();
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvNewContentCompositorManager(
    Endpoint<PCompositorManagerParent>&& aEndpoint,
    const ContentParentId& aChildId, uint32_t aNamespace) {
  CompositorManagerParent::Create(std::move(aEndpoint), aChildId, aNamespace,
                                   false);
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvNewContentImageBridge(
    Endpoint<PImageBridgeParent>&& aEndpoint, const ContentParentId& aChildId,
    uint32_t aNamespace) {
  if (!ImageBridgeParent::CreateForContent(std::move(aEndpoint), aChildId,
                                           aNamespace)) {
    return IPC_FAIL_NO_REASON(this);
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvNewContentRemoteMediaManager(
    Endpoint<PRemoteMediaManagerParent>&& aEndpoint,
    const ContentParentId& aChildId) {
  if (!RemoteMediaManagerParent::CreateForContent(std::move(aEndpoint),
                                                  aChildId)) {
    return IPC_FAIL_NO_REASON(this);
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvAddLayerTreeIdMapping(
    const LayerTreeIdMapping& aMapping) {
  LayerTreeOwnerTracker::Get()->Map(aMapping.layersId(), aMapping.ownerId());
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvRemoveLayerTreeIdMapping(
    const LayerTreeIdMapping& aMapping) {
  LayerTreeOwnerTracker::Get()->Unmap(aMapping.layersId(), aMapping.ownerId());
  CompositorBridgeParent::DeallocateLayerTreeId(aMapping.layersId());
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvFlushActiveCheckerboardReports() {
  nsCOMPtr<nsIObserverService> obsSvc = mozilla::services::GetObserverService();
  MOZ_ASSERT(obsSvc);
  if (obsSvc) {
    obsSvc->NotifyObservers(nullptr, "APZ:FlushActiveCheckerboard", nullptr);
  }
  return IPC_OK();
}

void GPUParent::GetGPUProcessName(nsACString& aStr) {
  auto processType = XRE_GetProcessType();
  unsigned pid = 0;
  if (processType == GeckoProcessType_GPU) {
    pid = getpid();
  } else {
    MOZ_DIAGNOSTIC_ASSERT(processType == GeckoProcessType_Default);
    pid = GPUProcessManager::Get()->GPUProcessPid();
  }

  nsPrintfCString processName("GPU (pid %u)", pid);
  aStr.Assign(processName);
}

mozilla::ipc::IPCResult GPUParent::RecvRequestMemoryReport(
    const uint32_t& aGeneration, const bool& aAnonymize,
    const bool& aMinimizeMemoryUsage, const Maybe<FileDescriptor>& aDMDFile,
    const RequestMemoryReportResolver& aResolver) {
  nsAutoCString processName;
  GetGPUProcessName(processName);

  mozilla::dom::MemoryReportRequestClient::Start(
      aGeneration, aAnonymize, aMinimizeMemoryUsage, aDMDFile, processName,
      [&](const MemoryReport& aReport) {
        (void)GetSingleton()->SendAddMemoryReport(aReport);
      },
      aResolver);
  return IPC_OK();
}

mozilla::ipc::IPCResult GPUParent::RecvCrashProcess() {
  MOZ_CRASH("Deliberate GPU process crash");
  return IPC_OK();
}

void GPUParent::ActorDestroy(ActorDestroyReason aWhy) {
  if (AbnormalShutdown == aWhy) {
    NS_WARNING("Shutting down GPU process early due to a crash!");
    ProcessChild::QuickExit();
  }

#ifndef NS_FREE_PERMANENT_DATA
  ProcessChild::QuickExit();
#endif

  mShutdownBlockers.WaitUntilClear(10 * 1000 )
      ->Then(GetCurrentSerialEventTarget(), __func__, [self = RefPtr{this}]() {
        if (self->mVsyncBridge) {
          self->mVsyncBridge->Shutdown();
          self->mVsyncBridge = nullptr;
        }
        VideoBridgeParent::Shutdown();
        CanvasRenderThread::Shutdown();
        CompositorThreadHolder::Shutdown();
        RemoteTextureMap::Shutdown();
        if (wr::RenderThread::Get()) {
          wr::RenderThread::ShutDown();
        }

        image::ImageMemoryReporter::ShutdownForWebRender();

        gl::GLContextProvider::Shutdown();


        Factory::ShutDown();

#ifdef NS_FREE_PERMANENT_DATA
        SkGraphics::PurgeFontCache();
        cairo_debug_reset_static_data();
#endif

        LayerTreeOwnerTracker::Shutdown();
        gfxVars::Shutdown();
        gfxConfig::Shutdown();
        XRE_ShutdownChildProcess();
      });
}

}  
