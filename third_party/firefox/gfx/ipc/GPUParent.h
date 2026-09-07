/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _include_gfx_ipc_GPUParent_h_
#define _include_gfx_ipc_GPUParent_h_

#include "mozilla/RefPtr.h"
#include "mozilla/gfx/PGPUParent.h"
#include "mozilla/ipc/AsyncBlockers.h"

namespace mozilla {

class TimeStamp;
namespace gfx {

class VsyncBridgeParent;

class GPUParent final : public PGPUParent {
 public:
  NS_INLINE_DECL_REFCOUNTING(GPUParent, final)

  GPUParent();

  static GPUParent* GetSingleton();

  ipc::AsyncBlockers& AsyncShutdownService() { return mShutdownBlockers; }

  static void GetGPUProcessName(nsACString& aStr);

  static bool MaybeFlushMemory();

  bool Init(mozilla::ipc::UntypedEndpoint&& aEndpoint,
            const char* aParentBuildID);
  void NotifyDeviceReset(DeviceResetReason aReason,
                         DeviceResetDetectPlace aPlace);
  void NotifyOverlayInfo(layers::OverlayInfo aInfo);
  void NotifySwapChainInfo(layers::SwapChainInfo aInfo);
  void NotifyDisableRemoteCanvas();

  void ReportGLStrings(GfxInfoGLStrings&& aStrings);

  mozilla::ipc::IPCResult RecvInit(nsTArray<GfxVarUpdate>&& vars,
                                   const DevicePrefs& devicePrefs,
                                   nsTArray<LayerTreeIdMapping>&& mappings,
                                   nsTArray<GfxInfoFeatureStatus>&& features,
                                   uint32_t wrNamespace,
                                   InitResolver&& aInitResolver);
  mozilla::ipc::IPCResult RecvInitCompositorManager(
      Endpoint<PCompositorManagerParent>&& aEndpoint, uint32_t aNamespace);
  mozilla::ipc::IPCResult RecvInitVsyncBridge(
      Endpoint<PVsyncBridgeParent>&& aVsyncEndpoint);
  mozilla::ipc::IPCResult RecvInitImageBridge(
      Endpoint<PImageBridgeParent>&& aEndpoint, uint32_t aNamespace);
  mozilla::ipc::IPCResult RecvInitVideoBridge(
      Endpoint<PVideoBridgeParent>&& aEndpoint,
      const layers::VideoBridgeSource& aSource);
  mozilla::ipc::IPCResult RecvInitUiCompositorController(
      const LayersId& aRootLayerTreeId,
      Endpoint<PUiCompositorControllerParent>&& aEndpoint);
  mozilla::ipc::IPCResult RecvInitAPZInputBridge(
      const LayersId& aRootLayerTreeId,
      Endpoint<PAPZInputBridgeParent>&& aEndpoint);
  mozilla::ipc::IPCResult RecvUpdateVar(const nsTArray<GfxVarUpdate>& var);
  mozilla::ipc::IPCResult RecvPreferenceUpdate(const Pref& pref);
  mozilla::ipc::IPCResult RecvScreenInformationChanged();
  mozilla::ipc::IPCResult RecvNotifyBatteryInfo(
      const BatteryInformation& aBatteryInfo);
  mozilla::ipc::IPCResult RecvNewContentCompositorManager(
      Endpoint<PCompositorManagerParent>&& aEndpoint,
      const ContentParentId& aChildId, uint32_t aNamespace);
  mozilla::ipc::IPCResult RecvNewContentImageBridge(
      Endpoint<PImageBridgeParent>&& aEndpoint, const ContentParentId& aChildId,
      uint32_t aNamespace);
  mozilla::ipc::IPCResult RecvNewContentRemoteMediaManager(
      Endpoint<PRemoteMediaManagerParent>&& aEndpoint,
      const ContentParentId& aChildId);
  mozilla::ipc::IPCResult RecvGetDeviceStatus(GPUDeviceData* aOutStatus);
  mozilla::ipc::IPCResult RecvSimulateDeviceReset();
  mozilla::ipc::IPCResult RecvAddLayerTreeIdMapping(
      const LayerTreeIdMapping& aMapping);
  mozilla::ipc::IPCResult RecvRemoveLayerTreeIdMapping(
      const LayerTreeIdMapping& aMapping);
  mozilla::ipc::IPCResult RecvFlushActiveCheckerboardReports();
  mozilla::ipc::IPCResult RecvRequestMemoryReport(
      const uint32_t& generation, const bool& anonymize,
      const bool& minimizeMemoryUsage,
      const Maybe<ipc::FileDescriptor>& DMDFile,
      const RequestMemoryReportResolver& aResolver);
  mozilla::ipc::IPCResult RecvCrashProcess();

  void ActorDestroy(ActorDestroyReason aWhy) override;

 private:
  ~GPUParent();

  const TimeStamp mLaunchTime;
  RefPtr<VsyncBridgeParent> mVsyncBridge;
  ipc::AsyncBlockers mShutdownBlockers;
};

}  
}  

#endif  // _include_gfx_ipc_GPUParent_h_
