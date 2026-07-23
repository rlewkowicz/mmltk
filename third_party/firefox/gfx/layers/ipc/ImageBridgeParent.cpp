/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ImageBridgeParent.h"
#include <stdint.h>            // for uint64_t, uint32_t
#include "CompositableHost.h"  // for CompositableParent, Create
#include "base/process.h"      // for ProcessId
#include "base/task.h"         // for CancelableTask, DeleteTask, etc
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/gfx/Point.h"  // for IntSize
#include "mozilla/Hal.h"        // for hal::SetCurrentThreadPriority()
#include "mozilla/HalTypes.h"   // for hal::THREAD_PRIORITY_COMPOSITOR
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/MessageChannel.h"  // for MessageChannel, etc
#include "mozilla/layers/BufferTexture.h"
#include "mozilla/layers/CompositableTransactionParent.h"
#include "mozilla/layers/LayersMessages.h"  // for EditReply
#include "mozilla/layers/PImageBridgeParent.h"
#include "mozilla/layers/TextureHostOGL.h"  // for TextureHostOGL
#include "mozilla/layers/Compositor.h"
#include "mozilla/layers/RemoteTextureMap.h"
#include "mozilla/Monitor.h"
#include "mozilla/mozalloc.h"  // for operator new, etc
#include "nsDebug.h"                 // for NS_ASSERTION, etc
#include "nsISupportsImpl.h"         // for ImageBridgeParent::Release, etc
#include "nsTArray.h"                // for nsTArray, nsTArray_Impl
#include "nsTArrayForwardDeclare.h"  // for nsTArray
#include "nsXULAppAPI.h"             // for XRE_GetAsyncIOEventTarget
#include "mozilla/layers/TextureHost.h"
#include "nsThreadUtils.h"


namespace mozilla {
namespace layers {

using namespace mozilla::ipc;
using namespace mozilla::gfx;
using namespace mozilla::media;

MOZ_RUNINIT ImageBridgeParent::ImageBridgeMap ImageBridgeParent::sImageBridges;

StaticAutoPtr<mozilla::Monitor> sImageBridgesLock;

static StaticRefPtr<ImageBridgeParent> sImageBridgeParentSingleton;

void ImageBridgeParent::Setup() {
  MOZ_ASSERT(NS_IsMainThread());
  if (!sImageBridgesLock) {
    sImageBridgesLock = new Monitor("ImageBridges");
    mozilla::ClearOnShutdown(&sImageBridgesLock);
  }
}

ImageBridgeParent::ImageBridgeParent(nsISerialEventTarget* aThread,
                                     EndpointProcInfo aChildProcessInfo,
                                     dom::ContentParentId aContentId,
                                     uint32_t aNamespace)
    : mThread(aThread),
      mContentId(aContentId),
      mNamespace(aNamespace),
      mClosed(false),
      mCompositorThreadHolder(CompositorThreadHolder::GetSingleton()) {
  MOZ_ASSERT(NS_IsMainThread());
  SetOtherEndpointProcInfo(aChildProcessInfo);
  mRemoteTextureTxnScheduler = RemoteTextureTxnScheduler::Create(this);
}

ImageBridgeParent::~ImageBridgeParent() = default;

ImageBridgeParent* ImageBridgeParent::CreateSameProcess(uint32_t aNamespace) {
  EndpointProcInfo procInfo = EndpointProcInfo::Current();
  RefPtr<ImageBridgeParent> parent = new ImageBridgeParent(
      CompositorThread(), procInfo, dom::ContentParentId(), aNamespace);

  {
    MonitorAutoLock lock(*sImageBridgesLock);
    MOZ_RELEASE_ASSERT(sImageBridges.count(procInfo.mPid) == 0);
    sImageBridges[procInfo.mPid] = parent;
  }

  sImageBridgeParentSingleton = parent;
  return parent;
}

bool ImageBridgeParent::CreateForGPUProcess(
    Endpoint<PImageBridgeParent>&& aEndpoint, uint32_t aNamespace) {
  MOZ_ASSERT(XRE_GetProcessType() == GeckoProcessType_GPU);

  nsCOMPtr<nsISerialEventTarget> compositorThread = CompositorThread();
  if (!compositorThread) {
    return false;
  }

  RefPtr<ImageBridgeParent> parent =
      new ImageBridgeParent(compositorThread, aEndpoint.OtherEndpointProcInfo(),
                            dom::ContentParentId(), aNamespace);

  compositorThread->Dispatch(NewRunnableMethod<Endpoint<PImageBridgeParent>&&>(
      "layers::ImageBridgeParent::Bind", parent, &ImageBridgeParent::Bind,
      std::move(aEndpoint)));

  sImageBridgeParentSingleton = parent;
  return true;
}

void ImageBridgeParent::ShutdownInternal() {
  nsTArray<RefPtr<ImageBridgeParent>> actors;
  {
    MonitorAutoLock lock(*sImageBridgesLock);
    for (const auto& iter : sImageBridges) {
      actors.AppendElement(iter.second);
    }
  }

  for (auto const& actor : actors) {
    MOZ_RELEASE_ASSERT(!actor->mClosed);
    actor->Close();
  }

  sImageBridgeParentSingleton = nullptr;
}

void ImageBridgeParent::Shutdown() {
  CompositorThread()->Dispatch(NS_NewRunnableFunction(
      "ImageBridgeParent::Shutdown",
      []() -> void { ImageBridgeParent::ShutdownInternal(); }));
}

void ImageBridgeParent::ActorDestroy(ActorDestroyReason aWhy) {
  mClosed = true;

  if (mRemoteTextureTxnScheduler) {
    mRemoteTextureTxnScheduler = nullptr;
  }
  for (const auto& entry : mCompositables) {
    entry.second->OnReleased();
  }
  mCompositables.clear();
  {
    MonitorAutoLock lock(*sImageBridgesLock);
    sImageBridges.erase(OtherPid());
  }
  GetThread()->Dispatch(
      NewRunnableMethod("layers::ImageBridgeParent::DeferredDestroy", this,
                        &ImageBridgeParent::DeferredDestroy));

}

class MOZ_STACK_CLASS AutoImageBridgeParentAsyncMessageSender final {
 public:
  explicit AutoImageBridgeParentAsyncMessageSender(
      ImageBridgeParent* aImageBridge,
      nsTArray<OpDestroy>* aToDestroy = nullptr)
      : mImageBridge(aImageBridge), mToDestroy(aToDestroy) {
    mImageBridge->SetAboutToSendAsyncMessages();
  }

  ~AutoImageBridgeParentAsyncMessageSender() {
    mImageBridge->SendPendingAsyncMessages();
    if (mToDestroy) {
      mImageBridge->DestroyActors(*mToDestroy);
    }
  }

 private:
  ImageBridgeParent* mImageBridge;
  nsTArray<OpDestroy>* mToDestroy;
};

mozilla::ipc::IPCResult ImageBridgeParent::RecvUpdate(
    EditArray&& aEdits, OpDestroyArray&& aToDestroy,
    const uint64_t& aFwdTransactionId) {

  AutoImageBridgeParentAsyncMessageSender autoAsyncMessageSender(this,
                                                                 &aToDestroy);
  UpdateFwdTransactionId(aFwdTransactionId);

  auto result = IPC_OK();

  for (const auto& edit : aEdits) {
    RefPtr<CompositableHost> compositable =
        FindCompositable(edit.compositable());
    if (!compositable ||
        !ReceiveCompositableUpdate(edit.detail(), WrapNotNull(compositable),
                                   edit.compositable())) {
      result = IPC_FAIL_NO_REASON(this);
      break;
    }
    uint32_t dropped = compositable->GetDroppedFrames();
    if (dropped) {
      (void)SendReportFramesDropped(edit.compositable(), dropped);
    }
  }

  if (mRemoteTextureTxnScheduler) {
    mRemoteTextureTxnScheduler->NotifyTxn(aFwdTransactionId);
  }

  return result;
}

bool ImageBridgeParent::CreateForContent(
    Endpoint<PImageBridgeParent>&& aEndpoint, dom::ContentParentId aContentId,
    uint32_t aNamespace) {
  nsCOMPtr<nsISerialEventTarget> compositorThread = CompositorThread();
  if (!compositorThread) {
    return false;
  }

  RefPtr<ImageBridgeParent> bridge =
      new ImageBridgeParent(compositorThread, aEndpoint.OtherEndpointProcInfo(),
                            aContentId, aNamespace);
  compositorThread->Dispatch(NewRunnableMethod<Endpoint<PImageBridgeParent>&&>(
      "layers::ImageBridgeParent::Bind", bridge, &ImageBridgeParent::Bind,
      std::move(aEndpoint)));

  return true;
}

void ImageBridgeParent::Bind(Endpoint<PImageBridgeParent>&& aEndpoint) {
  if (!aEndpoint.Bind(this)) return;

  RefPtr<ImageBridgeParent> oldActor;
  {
    MonitorAutoLock lock(*sImageBridgesLock);
    ImageBridgeMap::const_iterator i = sImageBridges.find(OtherPid());
    if (i != sImageBridges.end()) {
      oldActor = i->second;
    }
  }

  if (oldActor) {
    MOZ_RELEASE_ASSERT(!oldActor->mClosed);
    oldActor->Close();
  }

  {
    MonitorAutoLock lock(*sImageBridgesLock);
    sImageBridges[OtherPid()] = this;
  }
}

mozilla::ipc::IPCResult ImageBridgeParent::RecvWillClose() {
  nsTArray<PTextureParent*> textures;
  ManagedPTextureParent(textures);
  for (unsigned int i = 0; i < textures.Length(); ++i) {
    RefPtr<TextureHost> tex = TextureHost::AsTextureHost(textures[i]);
    tex->DeallocateDeviceData();
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult ImageBridgeParent::RecvNewCompositable(
    const CompositableHandle& aHandle, const TextureInfo& aInfo) {
  RefPtr<CompositableHost> host = AddCompositable(aHandle, aInfo);
  if (!host) {
    return IPC_FAIL_NO_REASON(this);
  }

  host->SetAsyncRef(AsyncCompositableRef(OtherPid(), aHandle));
  return IPC_OK();
}

mozilla::ipc::IPCResult ImageBridgeParent::RecvReleaseCompositable(
    const CompositableHandle& aHandle) {
  ReleaseCompositable(aHandle);
  return IPC_OK();
}

already_AddRefed<PTextureParent> ImageBridgeParent::AllocPTextureParent(
    const SurfaceDescriptor& aSharedData, ReadLockDescriptor& aReadLock,
    const LayersBackend& aLayersBackend, const TextureFlags& aFlags,
    const uint64_t& aSerial, const wr::MaybeExternalImageId& aExternalImageId) {
  if (aExternalImageId.isSome() &&
      !OwnsExternalImageId(aExternalImageId.ref())) {
    NS_ERROR("We do not own this external image id.");
    return nullptr;
  }
  return TextureHost::CreateIPDLActor(this, aSharedData, std::move(aReadLock),
                                      aLayersBackend, aFlags, mContentId,
                                      aSerial, aExternalImageId);
}

void ImageBridgeParent::SendAsyncMessage(
    Span<const AsyncParentMessageData> aMessage) {
  (void)SendParentAsyncMessages(aMessage);
}

class ProcessIdComparator {
 public:
  bool Equals(const ImageCompositeNotificationInfo& aA,
              const ImageCompositeNotificationInfo& aB) const {
    return aA.mImageBridgeProcessId == aB.mImageBridgeProcessId;
  }
  bool LessThan(const ImageCompositeNotificationInfo& aA,
                const ImageCompositeNotificationInfo& aB) const {
    return aA.mImageBridgeProcessId < aB.mImageBridgeProcessId;
  }
};

bool ImageBridgeParent::NotifyImageComposites(
    nsTArray<ImageCompositeNotificationInfo>& aNotifications) {
  aNotifications.Sort(ProcessIdComparator());
  uint32_t i = 0;
  bool ok = true;
  while (i < aNotifications.Length()) {
    AutoTArray<ImageCompositeNotification, 1> notifications;
    notifications.AppendElement(aNotifications[i].mNotification);
    uint32_t end = i + 1;
    MOZ_ASSERT(aNotifications[i].mNotification.compositable());
    ProcessId pid = aNotifications[i].mImageBridgeProcessId;
    while (end < aNotifications.Length() &&
           aNotifications[end].mImageBridgeProcessId == pid) {
      notifications.AppendElement(aNotifications[end].mNotification);
      ++end;
    }
    RefPtr<ImageBridgeParent> bridge = GetInstance(pid);
    if (!bridge || bridge->mClosed) {
      i = end;
      continue;
    }
    bridge->SendPendingAsyncMessages();
    if (!bridge->SendDidComposite(notifications)) {
      ok = false;
    }
    i = end;
  }
  return ok;
}

void ImageBridgeParent::DeferredDestroy() { mCompositorThreadHolder = nullptr; }

already_AddRefed<ImageBridgeParent> ImageBridgeParent::GetInstance(
    ProcessId aId) {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  MonitorAutoLock lock(*sImageBridgesLock);
  ImageBridgeMap::const_iterator i = sImageBridges.find(aId);
  if (i == sImageBridges.end()) {
    NS_WARNING("Cannot find image bridge for process!");
    return nullptr;
  }
  RefPtr<ImageBridgeParent> bridge = i->second;
  return bridge.forget();
}

bool ImageBridgeParent::OwnsExternalImageId(
    const wr::ExternalImageId& aId) const {
  return (mNamespace == static_cast<uint32_t>(wr::AsUint64(aId) >> 32));
}

bool ImageBridgeParent::AllocShmem(size_t aSize, ipc::Shmem* aShmem) {
  if (mClosed) {
    return false;
  }
  return PImageBridgeParent::AllocShmem(aSize, aShmem);
}

bool ImageBridgeParent::AllocUnsafeShmem(size_t aSize, ipc::Shmem* aShmem) {
  if (mClosed) {
    return false;
  }
  return PImageBridgeParent::AllocUnsafeShmem(aSize, aShmem);
}

bool ImageBridgeParent::DeallocShmem(ipc::Shmem& aShmem) {
  if (mClosed) {
    return false;
  }
  return PImageBridgeParent::DeallocShmem(aShmem);
}

bool ImageBridgeParent::IsSameProcess() const {
  return OtherPid() == base::GetCurrentProcId();
}

void ImageBridgeParent::NotifyNotUsed(PTextureParent* aTexture,
                                      uint64_t aTransactionId) {
  RefPtr<TextureHost> texture = TextureHost::AsTextureHost(aTexture);
  if (!texture) {
    return;
  }

  if (!(texture->GetFlags() & TextureFlags::RECYCLE) &&
      !(texture->GetFlags() & TextureFlags::WAIT_HOST_USAGE_END)) {
    return;
  }

  uint64_t textureId = TextureHost::GetTextureSerial(aTexture);
  mPendingAsyncMessage.AppendElement(
      OpNotifyNotUsed(textureId, aTransactionId));

  if (!IsAboutToSendAsyncMessages()) {
    SendPendingAsyncMessages();
  }
}

}  
}  
