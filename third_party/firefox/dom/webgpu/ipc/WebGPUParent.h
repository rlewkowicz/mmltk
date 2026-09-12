/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(WEBGPU_PARENT_H_)
#define WEBGPU_PARENT_H_

#include <atomic>
#include <cstdint>
#include <unordered_map>

#include "WebGPUTypes.h"
#include "mozilla/WeakPtr.h"
#include "mozilla/ipc/SharedMemoryHandle.h"
#include "mozilla/webgpu/ExternalTexture.h"
#include "mozilla/webgpu/PWebGPUParent.h"
#include "mozilla/webgpu/ffi/wgpu.h"
#include "mozilla/webrender/WebRenderAPI.h"
#include "nsCOMPtr.h"
#include "nsISerialEventTarget.h"

namespace mozilla {

namespace layers {
class RemoteTextureOwnerClient;
}  

namespace webgpu {

class SharedTexture;
class PresentationData;

class ErrorBuffer {
  static constexpr unsigned BUFFER_SIZE = 512;
  ffi::WGPUErrorBufferType mType = ffi::WGPUErrorBufferType_None;
  char mMessageUtf8[BUFFER_SIZE] = {};
  bool mAwaitingGetError = false;
  RawId mDeviceId = 0;

 public:
  ErrorBuffer();
  ErrorBuffer(const ErrorBuffer&) = delete;
  ~ErrorBuffer();

  ffi::WGPUErrorBuffer ToFFI();

  ffi::WGPUErrorBufferType GetType();

  static Maybe<dom::GPUErrorFilter> ErrorTypeToFilterType(
      ffi::WGPUErrorBufferType aType);

  struct Error {
    dom::GPUErrorFilter type;
    bool isDeviceLost;
    nsCString message;
    RawId deviceId;
  };

  Maybe<Error> GetError();

  void CoerceValidationToInternal();
};


class WebGPUParent final : public PWebGPUParent, public SupportsWeakPtr {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(WebGPUParent, override)

 public:
  explicit WebGPUParent(const dom::ContentParentId& aContentId);

  void PostAdapterRequestDevice(RawId aDeviceId);
  void BufferUnmap(RawId aDeviceId, RawId aBufferId, bool aFlush);
  ipc::IPCResult RecvMessages(uint32_t nrOfMessages,
                              ipc::ByteBuf&& aSerializedMessages,
                              nsTArray<ipc::ByteBuf>&& aDataBuffers,
                              nsTArray<MutableSharedMemoryHandle>&& aShmems);
  ipc::IPCResult RecvCreateExternalTextureSource(
      RawId aDeviceId, RawId aQueueId, RawId aExternalTextureSourceId,
      const ExternalTextureSourceDescriptor& aDesc);
  ipc::IPCResult RecvExternalTextureSlotRelease(
      uint64_t aSurfaceIdHigh, uint64_t aSurfaceIdLow, uint64_t aLayer,
      uint32_t aSlot, uint64_t aContentSession, uint64_t aContentSequence,
      uint64_t aPresentationRevision);
  ipc::IPCResult RecvWorkspaceRequest(RawId aDeviceId, uint64_t aSurfaceIdHigh,
                                     uint64_t aSurfaceIdLow, uint32_t aWidth, uint32_t aHeight);
  ipc::IPCResult RecvWorkspaceAcquire(RawId aDeviceId, uint64_t aSurfaceIdHigh,
                                     uint64_t aSurfaceIdLow, uint64_t aContentSession,
                                     uint64_t aContentSequence, uint64_t aPublication);
  void QueueSubmit(RawId aQueueId, RawId aDeviceId,
                   Span<const RawId> aCommandBuffers,
                   Span<const RawId> aTextureIds,
                   Span<const RawId> aExternalTextureSourceIds);
  void DeviceCreateSwapChain(RawId aDeviceId, RawId aQueueId,
                             const layers::RGBDescriptor& aDesc,
                             const nsTArray<RawId>& aBufferIds,
                             const layers::RemoteTextureOwnerId& aOwnerId,
                             bool aUseSharedTextureInSwapChain);

  void SwapChainPresent(RawId aTextureId, RawId aCommandEncoderId,
                        RawId aCommandBufferId,
                        const layers::RemoteTextureId& aRemoteTextureId,
                        const layers::RemoteTextureOwnerId& aOwnerId);
  void SwapChainDrop(const layers::RemoteTextureOwnerId& aOwnerId,
                     layers::RemoteTextureTxnType aTxnType,
                     layers::RemoteTextureTxnId aTxnId);

  void DevicePushErrorScope(RawId aDeviceId, dom::GPUErrorFilter);
  PopErrorScopeResult DevicePopErrorScope(RawId aDeviceId);

  ipc::IPCResult GetFrontBufferSnapshot(
      IProtocol* aProtocol, const layers::RemoteTextureOwnerId& aOwnerId,
      const RawId& aCommandEncoderId, const RawId& aCommandBufferId,
      Maybe<Shmem>& aShmem, gfx::IntSize& aSize, uint32_t& aByteStride);

  void ActorDestroy(ActorDestroyReason aWhy) override;

  struct BufferMapData {
    std::shared_ptr<ipc::SharedMemoryMapping> mShmem;
    bool mHasMapFlags;
    uint64_t mMappedOffset;
    uint64_t mMappedSize;
    RawId mDeviceId;
  };

  BufferMapData* GetBufferMapData(RawId aBufferId);

  bool UseSharedTextureForSwapChain(ffi::WGPUSwapChainId aSwapChainId);

  void DisableSharedTextureForSwapChain(ffi::WGPUSwapChainId aSwapChainId);

  bool EnsureSharedTextureForSwapChain(ffi::WGPUSwapChainId aSwapChainId,
                                       ffi::WGPUDeviceId aDeviceId,
                                       ffi::WGPUTextureId aTextureId,
                                       uint32_t aWidth, uint32_t aHeight,
                                       struct ffi::WGPUTextureFormat aFormat,
                                       ffi::WGPUTextureUsages aUsage);

  void EnsureSharedTextureForReadBackPresent(
      ffi::WGPUSwapChainId aSwapChainId, ffi::WGPUDeviceId aDeviceId,
      ffi::WGPUTextureId aTextureId, uint32_t aWidth, uint32_t aHeight,
      struct ffi::WGPUTextureFormat aFormat, ffi::WGPUTextureUsages aUsage);

  std::shared_ptr<SharedTexture> CreateSharedTexture(
      const layers::RemoteTextureOwnerId& aOwnerId, ffi::WGPUDeviceId aDeviceId,
      ffi::WGPUTextureId aTextureId, uint32_t aWidth, uint32_t aHeight,
      const struct ffi::WGPUTextureFormat aFormat,
      ffi::WGPUTextureUsages aUsage);

  std::shared_ptr<SharedTexture> GetSharedTexture(ffi::WGPUTextureId aId);

  void PostSharedTexture(const std::shared_ptr<SharedTexture>&& aSharedTexture,
                         const layers::RemoteTextureId aRemoteTextureId,
                         const layers::RemoteTextureOwnerId aOwnerId);

  bool ForwardError(ErrorBuffer& aError);

  ffi::WGPUGlobal* GetContext() const { return mContext.get(); }

  bool IsDeviceActive(const RawId aDeviceId) {
    return mActiveDeviceIds.Contains(aDeviceId);
  }

  RefPtr<gfx::FileHandleWrapper> GetDeviceFenceHandle(const RawId aDeviceId);

  void RemoveSharedTexture(RawId aTextureId);

  const ExternalTextureSourceHost& GetExternalTextureSource(
      ffi::WGPUExternalTextureSourceId aId) const;
  void DestroyExternalTextureSource(RawId aId);
  void DropExternalTextureSource(RawId aId);

  void DeallocBufferShmem(RawId aBufferId);
  void PreDeviceDrop(RawId aDeviceId);
  // Thread-safe entry from the native workspace copy dispatcher. The IPDL send
  // is always queued onto this actor's owning serial event target.
  void NotifyExternalTextureFrame(RawId aDeviceId, uint64_t aSurfaceIdHigh,
                                  uint64_t aSurfaceIdLow,
                                  uint64_t aLayer,
                                  uint32_t aSlot,
                                  uint64_t aContentSession,
                                  uint64_t aContentSequence,
                                  uint64_t aPresentationRevision,
                                  uint32_t aContentWidth,
                                  uint32_t aContentHeight, bool aCopyComplete,
                                  uint64_t aSourceHigh, uint64_t aSourceLow, bool aDirectSampling);
  void NotifyWorkspaceSourceRequested(uint64_t aSourceHigh, uint64_t aSourceLow);
  void NotifyWorkspaceRequestReady(RawId aDeviceId, uint64_t aHigh,
                                  uint64_t aLow, uint32_t aWidth, uint32_t aHeight);
  void NotifyExternalTextureImportReady(RawId aDeviceId,
                                        uint64_t aSurfaceIdHigh,
                                        uint64_t aSurfaceIdLow);
  struct MapRequest {
    WeakPtr<WebGPUParent> mParent;
    ffi::WGPUDeviceId mDeviceId;
    ffi::WGPUBufferId mBufferId;
    ffi::WGPUHostMap mHostMap;
    uint64_t mOffset;
    uint64_t mSize;
  };

  static void MapCallback( uint8_t* aUserData,
                          ffi::WGPUBufferMapAsyncStatus aStatus);

  struct OnSubmittedWorkDoneRequest {
    WeakPtr<WebGPUParent> mParent;
    ffi::WGPUDeviceId mQueueId;
  };

  static void OnSubmittedWorkDoneCallback(
       uint8_t* userdata);

  void ReportError(RawId aDeviceId, GPUErrorFilter, const nsCString& message);

  std::vector<std::shared_ptr<ipc::SharedMemoryMapping>> mTempMappings;

  std::unordered_map<RawId, BufferMapData> mSharedMemoryMap;

  const dom::ContentParentId mContentId;

 private:
  static void DeviceLostCallback(uint8_t* aUserData, uint8_t aReason,
                                 const char* aMessage);

  virtual ~WebGPUParent();
  void LoseDevice(const RawId aDeviceId, dom::GPUDeviceLostReason aReason,
                  const nsACString& aMessage);

  nsCOMPtr<nsISerialEventTarget> mOwningEventTarget;
  UniquePtr<ffi::WGPUGlobal> mContext;

  std::unordered_map<layers::RemoteTextureOwnerId, RefPtr<PresentationData>,
                     layers::RemoteTextureOwnerId::HashFn>
      mPresentationDataMap;

  RefPtr<layers::RemoteTextureOwnerClient> mRemoteTextureOwner;

  std::unordered_map<uint64_t, std::vector<ErrorScope>>
      mErrorScopeStackByDevice;

  std::unordered_map<ffi::WGPUTextureId, std::shared_ptr<SharedTexture>>
      mSharedTextures;

  std::unordered_map<RawId, ExternalTextureSourceHost> mExternalTextureSources;

  nsTHashSet<RawId> mLostDeviceIds;

  nsTHashSet<RawId> mActiveDeviceIds;

  std::unordered_map<RawId, RefPtr<gfx::FileHandleWrapper>> mDeviceFenceHandles;
};

#if defined(XP_LINUX) && !0
class VkImageHandle final {
 public:
  explicit VkImageHandle(ffi::WGPUVkImageHandle* aVkImageHandle)
      : mVkImageHandle(aVkImageHandle) {}
  VkImageHandle(const VkImageHandle&) = delete;
  VkImageHandle& operator=(const VkImageHandle&) = delete;

  const ffi::WGPUVkImageHandle* Get() { return mVkImageHandle; }

  ~VkImageHandle();

 private:
  ffi::WGPUVkImageHandle* mVkImageHandle;
};

class VkSemaphoreHandle final {
 public:
  explicit VkSemaphoreHandle(ffi::WGPUVkSemaphoreHandle* aVkSemaphoreHandle)
      : mVkSemaphoreHandle(aVkSemaphoreHandle) {}
  VkSemaphoreHandle(const VkSemaphoreHandle&) = delete;
  VkSemaphoreHandle& operator=(const VkSemaphoreHandle&) = delete;

  const ffi::WGPUVkSemaphoreHandle* Get() { return mVkSemaphoreHandle; }

  ~VkSemaphoreHandle();

 private:
  ffi::WGPUVkSemaphoreHandle* mVkSemaphoreHandle;
};
#endif

}  
}  

#endif
