/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WEBGPU_CHILD_H_
#define WEBGPU_CHILD_H_

#include <deque>
#include <unordered_map>

#include "mozilla/MozPromise.h"
#include "mozilla/WeakPtr.h"
#include "mozilla/webgpu/Adapter.h"
#include "mozilla/webgpu/Device.h"
#include "mozilla/webgpu/Instance.h"
#include "mozilla/webgpu/PWebGPUChild.h"
#include "mozilla/webgpu/SupportedFeatures.h"
#include "mozilla/webgpu/SupportedLimits.h"
#include "mozilla/webgpu/WorkspaceSlotReleaseRegistration.h"
#include "mozilla/webgpu/ffi/wgpu.h"

namespace mozilla {
namespace dom {
struct GPURequestAdapterOptions;
}  
namespace layers {
class CompositorBridgeChild;
}  
namespace webgpu {
class WorkspaceSlotReleaseListener;
namespace ffi {
struct WGPUClient;
struct WGPULimits;
struct WGPUTextureViewDescriptor;
}  

using AdapterPromise =
    MozPromise<ipc::ByteBuf, Maybe<ipc::ResponseRejectReason>, true>;
using PipelinePromise = MozPromise<RawId, ipc::ResponseRejectReason, true>;
using DevicePromise = MozPromise<bool, ipc::ResponseRejectReason, true>;

ffi::WGPUByteBuf* ToFFI(ipc::ByteBuf* x);

struct PendingRequestAdapterPromise {
  RefPtr<dom::Promise> promise;
  RefPtr<Instance> instance;
  RawId adapter_id;
};

struct PendingRequestDevicePromise {
  RefPtr<dom::Promise> promise;
  RawId device_id;
  RawId queue_id;
  nsString label;
  RefPtr<Adapter> adapter;
  RefPtr<SupportedFeatures> features;
  RefPtr<SupportedLimits> limits;
  RefPtr<AdapterInfo> adapter_info;
  RefPtr<dom::Promise> lost_promise;
};

struct PendingPopErrorScopePromise {
  RefPtr<dom::Promise> promise;
  RefPtr<Device> device;
};

struct PendingCreatePipelinePromise {
  RefPtr<dom::Promise> promise;
  RefPtr<Device> device;
  bool is_render_pipeline;
  RawId pipeline_id;
  nsString label;
};

struct PendingCreateShaderModulePromise {
  RefPtr<dom::Promise> promise;
  RefPtr<Device> device;
  RefPtr<ShaderModule> shader_module;
};

struct PendingBufferMapPromise {
  RefPtr<dom::Promise> promise;
  RefPtr<Buffer> buffer;
};

class WebGPUChild final : public PWebGPUChild {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(WebGPUChild, override)

 public:
  friend class layers::CompositorBridgeChild;
  friend class WorkspaceSlotReleaseListener;

 public:
  explicit WebGPUChild();

  RawId RenderBundleEncoderFinish(ffi::WGPURenderBundleEncoder& aEncoder,
                                  RawId aDeviceId,
                                  const dom::GPURenderBundleDescriptor& aDesc);
  RawId RenderBundleEncoderFinishError(RawId aDeviceId, const nsString& aLabel);

  ffi::WGPUClient* GetClient() const { return mClient.get(); }

  void SwapChainPresent(RawId aTextureId,
                        const RemoteTextureId& aRemoteTextureId,
                        const RemoteTextureOwnerId& aOwnerId);

  void RegisterDevice(Device* const aDevice);
  void UnregisterDevice(RawId aDeviceId);
  void ForwardExternalTextureSlotRelease(uint64_t aSurfaceIdHigh,
                                         uint64_t aSurfaceIdLow,
                                         uint64_t aLayer, uint32_t aSlot,
                                         uint64_t aContentSession,
                                         uint64_t aContentSequence,
                                         uint64_t aPresentationRevision);

  void QueueSubmit(RawId aSelfId, RawId aDeviceId,
                   nsTArray<RawId>& aCommandBuffers,
                   const nsTArray<RawId>& aUsedExternalTextureSources);
  void NotifyWaitForSubmit(RawId aTextureId);

  static void JsWarning(nsIGlobalObject* aGlobal, const nsACString& aMessage);

  void SendSerializedMessages(uint32_t aNrOfMessages,
                              ipc::ByteBuf aSerializedMessages);

 private:
  virtual ~WebGPUChild();

  UniquePtr<ffi::WGPUClient> const mClient;

  std::unordered_map<RawId, WeakPtr<Device>> mDeviceMap;
  WorkspaceSlotReleaseRegistrationOwner mWorkspaceSlotReleaseRegistrations;
  std::unordered_map<uint64_t, RefPtr<WorkspaceSlotReleaseListener>>
      mWorkspaceSlotReleaseListeners;

  nsTArray<RawId> mSwapChainTexturesWaitingForSubmit;

  bool mScheduledFlushQueuedMessages = false;
  void ScheduledFlushQueuedMessages();
  nsTArray<ipc::ByteBuf> mQueuedDataBuffers;
  nsTArray<ipc::MutableSharedMemoryHandle> mQueuedHandles;

  std::deque<PendingRequestAdapterPromise> mPendingRequestAdapterPromises;
  std::deque<PendingRequestDevicePromise> mPendingRequestDevicePromises;
  std::unordered_map<RawId, RefPtr<dom::Promise>> mPendingDeviceLostPromises;
  std::deque<PendingPopErrorScopePromise> mPendingPopErrorScopePromises;
  std::deque<PendingCreatePipelinePromise> mPendingCreatePipelinePromises;
  std::deque<PendingCreateShaderModulePromise>
      mPendingCreateShaderModulePromises;
  std::unordered_map<RawId, std::deque<PendingBufferMapPromise>>
      mPendingBufferMapPromises;
  std::unordered_map<ffi::WGPUQueueId, std::deque<RefPtr<dom::Promise>>>
      mPendingOnSubmittedWorkDonePromises;

  void ClearActorState();
  void UnregisterWorkspaceWindow(uint64_t aWindowId);
  void DetachWorkspaceWindow(uint64_t aWindowId);
  void ClearWorkspaceSlotReleaseRegistrations();

 public:
  ipc::IPCResult RecvServerMessage(const ipc::ByteBuf& aByteBuf);
  ipc::IPCResult RecvUncapturedError(RawId aDeviceId,
                                     const dom::GPUErrorFilter aType,
                                     const nsACString& aMessage);
  ipc::IPCResult RecvDeviceLost(RawId aDeviceId,
                                const dom::GPUDeviceLostReason aReason,
                                const nsACString& aMessage);
  ipc::IPCResult RecvExternalTextureFrame(RawId aDeviceId,
                                          uint64_t aSurfaceIdHigh,
                                          uint64_t aSurfaceIdLow,
                                          uint64_t aLayer,
                                          uint32_t aSlot,
                                          uint64_t aContentSession,
                                          uint64_t aContentSequence,
                                          uint64_t aPresentationRevision,
                                          uint32_t aContentWidth,
                                          uint32_t aContentHeight, bool aCopyComplete);
  ipc::IPCResult RecvExternalTextureImportReady(RawId aDeviceId,
                                                 uint64_t aSurfaceIdHigh,
                                                 uint64_t aSurfaceIdLow);

  size_t QueueDataBuffer(ipc::ByteBuf&& bb);
  size_t QueueShmemHandle(ipc::MutableSharedMemoryHandle&& handle);
  void ScheduleFlushQueuedMessages();
  void FlushQueuedMessages();

  void ActorDestroy(ActorDestroyReason) override;

  void EnqueueRequestAdapterPromise(PendingRequestAdapterPromise&& promise);
  void EnqueueRequestDevicePromise(PendingRequestDevicePromise&& promise);
  void RegisterDeviceLostPromise(RawId id, RefPtr<dom::Promise>& promise);
  void EnqueuePopErrorScopePromise(PendingPopErrorScopePromise&& promise);
  void EnqueueCreatePipelinePromise(PendingCreatePipelinePromise&& promise);
  void EnqueueCreateShaderModulePromise(
      PendingCreateShaderModulePromise&& promise);
  void EnqueueBufferMapPromise(RawId id, PendingBufferMapPromise&& promise);
  void EnqueueOnSubmittedWorkDonePromise(RawId id,
                                         RefPtr<dom::Promise>& promise);

  PendingRequestAdapterPromise DequeueRequestAdapterPromise();
  PendingRequestDevicePromise DequeueRequestDevicePromise();
  PendingPopErrorScopePromise DequeuePopErrorScopePromise();
  PendingCreatePipelinePromise DequeueCreatePipelinePromise();
  PendingCreateShaderModulePromise DequeueCreateShaderModulePromise();
  PendingBufferMapPromise DequeueBufferMapPromise(RawId id);
  RefPtr<dom::Promise> DequeueOnSubmittedWorkDonePromise(RawId id);
};

}  
}  

#endif  // WEBGPU_CHILD_H_
