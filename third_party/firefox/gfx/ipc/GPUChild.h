/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _include_mozilla_gfx_ipc_GPUChild_h_
#define _include_mozilla_gfx_ipc_GPUChild_h_

#include "mozilla/RefPtr.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/gfx/PGPUChild.h"
#include "mozilla/gfx/gfxVarReceiver.h"

namespace mozilla {

namespace dom {
class MemoryReportRequestHost;
}  
namespace gfx {

class GPUProcessHost;

class GPUChild final : public PGPUChild,
                       public gfxVarReceiver {
  typedef mozilla::dom::MemoryReportRequestHost MemoryReportRequestHost;

 public:
  NS_INLINE_DECL_REFCOUNTING(GPUChild, final)

  explicit GPUChild(GPUProcessHost* aHost);

  using InitPromiseType = MozPromise<Ok, Ok, true>;
  RefPtr<InitPromiseType> Init();

  bool IsGPUReady() const { return mGPUReady; }

  bool EnsureGPUReady(bool aForceSync = false);

  void OnVarChanged(const nsTArray<GfxVarUpdate>& aVar) override;

  mozilla::ipc::IPCResult RecvDeclareStable();
  mozilla::ipc::IPCResult RecvReportCheckerboard(const uint32_t& aSeverity,
                                                 const nsCString& aLog);
  void ActorDestroy(ActorDestroyReason aWhy) override;
  mozilla::ipc::IPCResult RecvGraphicsError(const nsCString& aError);
  mozilla::ipc::IPCResult RecvFlushActiveCheckerboardReportsDone();
  mozilla::ipc::IPCResult RecvNotifyDeviceReset(
      const GPUDeviceData& aData, const DeviceResetReason& aReason,
      const DeviceResetDetectPlace& aPlace);
  mozilla::ipc::IPCResult RecvNotifyOverlayInfo(const OverlayInfo aInfo);
  mozilla::ipc::IPCResult RecvNotifySwapChainInfo(const SwapChainInfo aInfo);
  mozilla::ipc::IPCResult RecvNotifyDisableRemoteCanvas();
  mozilla::ipc::IPCResult RecvFlushMemory(const nsString& aReason);
  mozilla::ipc::IPCResult RecvAddMemoryReport(const MemoryReport& aReport);
  mozilla::ipc::IPCResult RecvUpdateFeature(const Feature& aFeature,
                                            const FeatureFailure& aChange);
  mozilla::ipc::IPCResult RecvUsedFallback(const Fallback& aFallback,
                                           const nsCString& aMessage);
  mozilla::ipc::IPCResult RecvUpdateMediaCodecsSupported(
      const media::MediaCodecsSupported& aSupported);
  mozilla::ipc::IPCResult RecvReportGLStrings(GfxInfoGLStrings&& aStrings);

  bool SendRequestMemoryReport(const uint32_t& aGeneration,
                               const bool& aAnonymize,
                               const bool& aMinimizeMemoryUsage,
                               const Maybe<ipc::FileDescriptor>& aDMDFile);

  static void Destroy(RefPtr<GPUChild>&& aChild);

 private:
  virtual ~GPUChild();

  void OnInitComplete(const GPUDeviceData& aData);

  GPUProcessHost* mHost;
  UniquePtr<MemoryReportRequestHost> mMemoryReportRequest;
  bool mGPUReady;
};

}  
}  

#endif  // _include_mozilla_gfx_ipc_GPUChild_h_
