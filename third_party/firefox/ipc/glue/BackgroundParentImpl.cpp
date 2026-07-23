/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BackgroundParentImpl.h"

#include "BroadcastChannelParent.h"
#include "mozilla/Assertions.h"
#include "mozilla/RDDProcessManager.h"
#include "mozilla/ipc/UtilityProcessManager.h"
#include "mozilla/RemoteDecodeUtils.h"
#include "mozilla/RefPtr.h"
#include "mozilla/dom/BackgroundSessionStorageServiceParent.h"
#include "mozilla/dom/ClientManagerActors.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/CookieStoreParent.h"
#include "mozilla/dom/DOMTypes.h"
#include "mozilla/dom/FetchParent.h"
#include "mozilla/dom/FileCreatorParent.h"
#include "mozilla/dom/FileSystemManagerParentFactory.h"
#include "mozilla/dom/FileSystemRequestParent.h"
#include "mozilla/dom/ProcessIsolation.h"
#include "mozilla/dom/MessagePortParent.h"
#include "mozilla/dom/RemoteWorkerControllerParent.h"
#include "mozilla/dom/RemoteWorkerServiceParent.h"
#include "mozilla/dom/ServiceWorkerActors.h"
#include "mozilla/dom/ServiceWorkerContainerParent.h"
#include "mozilla/dom/ServiceWorkerManagerParent.h"
#include "mozilla/dom/ServiceWorkerParent.h"
#include "mozilla/dom/ServiceWorkerRegistrar.h"
#include "mozilla/dom/ServiceWorkerRegistrationParent.h"
#include "mozilla/dom/SessionStorageManager.h"
#include "mozilla/dom/SharedWorkerParent.h"
#include "mozilla/dom/StorageActivityService.h"
#include "mozilla/dom/StorageIPC.h"
#include "mozilla/dom/TemporaryIPCBlobParent.h"
#include "mozilla/dom/WebTransportParent.h"
#include "mozilla/dom/cache/ActorUtils.h"
#include "mozilla/dom/indexedDB/ActorsParent.h"
#include "mozilla/dom/locks/LockManagerParent.h"
#include "mozilla/dom/localstorage/ActorsParent.h"
#include "mozilla/dom/quota/ActorsParent.h"
#include "mozilla/dom/quota/QuotaParent.h"
#include "mozilla/dom/simpledb/ActorsParent.h"
#include "mozilla/dom/cache/BoundStorageKeyParent.h"
#include "mozilla/dom/VsyncParent.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/IdleSchedulerParent.h"
#include "mozilla/ipc/PBackgroundSharedTypes.h"
#include "mozilla/net/BackgroundDataBridgeParent.h"
#include "mozilla/net/HttpBackgroundChannelParent.h"
#include "nsIHttpChannelInternal.h"
#include "nsIPrincipal.h"
#include "nsProxyRelease.h"
#include "nsThreadUtils.h"
#include "nsXULAppAPI.h"

using mozilla::AssertIsOnMainThread;
using mozilla::dom::FileSystemRequestParent;
using mozilla::dom::MessagePortParent;
using mozilla::dom::PMessagePortParent;
using mozilla::dom::PServiceWorkerContainerParent;
using mozilla::dom::PServiceWorkerParent;
using mozilla::dom::PServiceWorkerRegistrationParent;
using mozilla::dom::ServiceWorkerParent;
using mozilla::dom::cache::PCacheParent;
using mozilla::dom::cache::PCacheStorageParent;
using mozilla::dom::cache::PCacheStreamControlParent;
using mozilla::ipc::AssertIsOnBackgroundThread;

namespace mozilla::ipc {

using mozilla::dom::BroadcastChannelParent;
using mozilla::dom::ContentParent;
using mozilla::dom::ThreadsafeContentParentHandle;

BackgroundParentImpl::BackgroundParentImpl() {
  AssertIsInMainProcess();

  MOZ_COUNT_CTOR(mozilla::ipc::BackgroundParentImpl);
}

BackgroundParentImpl::~BackgroundParentImpl() {
  AssertIsInMainProcess();
  AssertIsOnMainThread();

  MOZ_COUNT_DTOR(mozilla::ipc::BackgroundParentImpl);
}

void BackgroundParentImpl::ProcessingError(Result aCode, const char* aReason) {
  if (MsgDropped == aCode) {
    return;
  }

  if (MsgValueError == aCode) {
    return;
  }

  nsDependentCString reason(aReason);
  if (BackgroundParent::IsOtherProcessActor(this)) {
    BackgroundParent::KillHardAsync(this, reason);
    if (CanSend()) {
      GetIPCChannel()->InduceConnectionError();
    }
  } else {
    MOZ_CRASH("in-process BackgroundParent abort due to IPC error");
  }
}

void BackgroundParentImpl::ActorDestroy(ActorDestroyReason aWhy) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
}

auto BackgroundParentImpl::AllocPBackgroundIDBFactoryParent(
    const LoggingInfo& aLoggingInfo, const nsACString& aSystemLocale)
    -> already_AddRefed<PBackgroundIDBFactoryParent> {
  using mozilla::dom::indexedDB::AllocPBackgroundIDBFactoryParent;

  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return AllocPBackgroundIDBFactoryParent(aLoggingInfo, aSystemLocale);
}

mozilla::ipc::IPCResult
BackgroundParentImpl::RecvPBackgroundIDBFactoryConstructor(
    PBackgroundIDBFactoryParent* aActor, const LoggingInfo& aLoggingInfo,
    const nsACString& aSystemLocale) {
  using mozilla::dom::indexedDB::RecvPBackgroundIDBFactoryConstructor;

  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  if (!RecvPBackgroundIDBFactoryConstructor(aActor, aLoggingInfo,
                                            aSystemLocale)) {
    return IPC_FAIL_NO_REASON(this);
  }
  return IPC_OK();
}

auto BackgroundParentImpl::AllocPBackgroundIndexedDBUtilsParent()
    -> PBackgroundIndexedDBUtilsParent* {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return mozilla::dom::indexedDB::AllocPBackgroundIndexedDBUtilsParent();
}

bool BackgroundParentImpl::DeallocPBackgroundIndexedDBUtilsParent(
    PBackgroundIndexedDBUtilsParent* aActor) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return mozilla::dom::indexedDB::DeallocPBackgroundIndexedDBUtilsParent(
      aActor);
}

already_AddRefed<BackgroundParentImpl::PBackgroundSDBConnectionParent>
BackgroundParentImpl::AllocPBackgroundSDBConnectionParent(
    const PersistenceType& aPersistenceType,
    const PrincipalInfo& aPrincipalInfo) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  if (!BackgroundParent::ValidatePrincipalInfo(this, aPrincipalInfo)) {
    return nullptr;
  }

  return mozilla::dom::AllocPBackgroundSDBConnectionParent(aPersistenceType,
                                                           aPrincipalInfo);
}

mozilla::ipc::IPCResult
BackgroundParentImpl::RecvPBackgroundSDBConnectionConstructor(
    PBackgroundSDBConnectionParent* aActor,
    const PersistenceType& aPersistenceType,
    const PrincipalInfo& aPrincipalInfo) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  if (!mozilla::dom::RecvPBackgroundSDBConnectionConstructor(
          aActor, aPersistenceType, aPrincipalInfo)) {
    return IPC_FAIL_NO_REASON(this);
  }
  return IPC_OK();
}

BackgroundParentImpl::PBackgroundLSObserverParent*
BackgroundParentImpl::AllocPBackgroundLSObserverParent(
    const uint64_t& aObserverId) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return mozilla::dom::AllocPBackgroundLSObserverParent(aObserverId);
}

mozilla::ipc::IPCResult
BackgroundParentImpl::RecvPBackgroundLSObserverConstructor(
    PBackgroundLSObserverParent* aActor, const uint64_t& aObserverId) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  if (!mozilla::dom::RecvPBackgroundLSObserverConstructor(aActor,
                                                          aObserverId)) {
    return IPC_FAIL_NO_REASON(this);
  }
  return IPC_OK();
}

bool BackgroundParentImpl::DeallocPBackgroundLSObserverParent(
    PBackgroundLSObserverParent* aActor) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return mozilla::dom::DeallocPBackgroundLSObserverParent(aActor);
}

BackgroundParentImpl::PBackgroundLSRequestParent*
BackgroundParentImpl::AllocPBackgroundLSRequestParent(
    const LSRequestParams& aParams) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return mozilla::dom::AllocPBackgroundLSRequestParent(this, aParams);
}

mozilla::ipc::IPCResult
BackgroundParentImpl::RecvPBackgroundLSRequestConstructor(
    PBackgroundLSRequestParent* aActor, const LSRequestParams& aParams) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  if (!mozilla::dom::RecvPBackgroundLSRequestConstructor(aActor, aParams)) {
    return IPC_FAIL_NO_REASON(this);
  }
  return IPC_OK();
}

bool BackgroundParentImpl::DeallocPBackgroundLSRequestParent(
    PBackgroundLSRequestParent* aActor) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return mozilla::dom::DeallocPBackgroundLSRequestParent(aActor);
}

BackgroundParentImpl::PBackgroundLSSimpleRequestParent*
BackgroundParentImpl::AllocPBackgroundLSSimpleRequestParent(
    const LSSimpleRequestParams& aParams) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return mozilla::dom::AllocPBackgroundLSSimpleRequestParent(this, aParams);
}

mozilla::ipc::IPCResult
BackgroundParentImpl::RecvPBackgroundLSSimpleRequestConstructor(
    PBackgroundLSSimpleRequestParent* aActor,
    const LSSimpleRequestParams& aParams) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  if (!mozilla::dom::RecvPBackgroundLSSimpleRequestConstructor(aActor,
                                                               aParams)) {
    return IPC_FAIL_NO_REASON(this);
  }
  return IPC_OK();
}

bool BackgroundParentImpl::DeallocPBackgroundLSSimpleRequestParent(
    PBackgroundLSSimpleRequestParent* aActor) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return mozilla::dom::DeallocPBackgroundLSSimpleRequestParent(aActor);
}

BackgroundParentImpl::PBackgroundLocalStorageCacheParent*
BackgroundParentImpl::AllocPBackgroundLocalStorageCacheParent(
    const PrincipalInfo& aPrincipalInfo, const nsACString& aOriginKey,
    const uint32_t& aPrivateBrowsingId) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return mozilla::dom::AllocPBackgroundLocalStorageCacheParent(
      aPrincipalInfo, aOriginKey, aPrivateBrowsingId);
}

mozilla::ipc::IPCResult
BackgroundParentImpl::RecvPBackgroundLocalStorageCacheConstructor(
    PBackgroundLocalStorageCacheParent* aActor,
    const PrincipalInfo& aPrincipalInfo, const nsACString& aOriginKey,
    const uint32_t& aPrivateBrowsingId) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  if (!BackgroundParent::ValidatePrincipalInfo(this, aPrincipalInfo)) {
    return IPC_FAIL(
        this,
        "Invalid aPrincipalInfo in PBackgroundLocalStorageCacheConstructor");
  }

  return mozilla::dom::RecvPBackgroundLocalStorageCacheConstructor(
      this, aActor, aPrincipalInfo, aOriginKey, aPrivateBrowsingId);
}

bool BackgroundParentImpl::DeallocPBackgroundLocalStorageCacheParent(
    PBackgroundLocalStorageCacheParent* aActor) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return mozilla::dom::DeallocPBackgroundLocalStorageCacheParent(aActor);
}

auto BackgroundParentImpl::AllocPBackgroundStorageParent(
    const nsAString& aProfilePath, const uint32_t& aPrivateBrowsingId)
    -> PBackgroundStorageParent* {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return mozilla::dom::AllocPBackgroundStorageParent(aProfilePath,
                                                     aPrivateBrowsingId);
}

mozilla::ipc::IPCResult BackgroundParentImpl::RecvPBackgroundStorageConstructor(
    PBackgroundStorageParent* aActor, const nsAString& aProfilePath,
    const uint32_t& aPrivateBrowsingId) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return mozilla::dom::RecvPBackgroundStorageConstructor(aActor, aProfilePath,
                                                         aPrivateBrowsingId);
}

bool BackgroundParentImpl::DeallocPBackgroundStorageParent(
    PBackgroundStorageParent* aActor) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return mozilla::dom::DeallocPBackgroundStorageParent(aActor);
}

already_AddRefed<BackgroundParentImpl::PBackgroundSessionStorageManagerParent>
BackgroundParentImpl::AllocPBackgroundSessionStorageManagerParent(
    const uint64_t& aTopContextId) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return dom::AllocPBackgroundSessionStorageManagerParent(aTopContextId);
}

already_AddRefed<mozilla::dom::PBackgroundSessionStorageServiceParent>
BackgroundParentImpl::AllocPBackgroundSessionStorageServiceParent() {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return MakeAndAddRef<mozilla::dom::BackgroundSessionStorageServiceParent>();
}

mozilla::ipc::IPCResult BackgroundParentImpl::RecvCreateFileSystemManagerParent(
    const PrincipalInfo& aPrincipalInfo,
    Endpoint<PFileSystemManagerParent>&& aParentEndpoint,
    CreateFileSystemManagerParentResolver&& aResolver) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  EnumSet<dom::ValidatePrincipalOptions> options;
  if (BackgroundParent::GetRemoteType(this) == INFERENCE_REMOTE_TYPE) {
    options += dom::ValidatePrincipalOptions::AllowSystem;
  }
  if (!BackgroundParent::ValidatePrincipalInfo(this, aPrincipalInfo, options)) {
    aResolver(NS_ERROR_FAILURE);
    return IPC_OK();
  }

  return mozilla::dom::CreateFileSystemManagerParent(
      this, aPrincipalInfo, std::move(aParentEndpoint), std::move(aResolver));
}

mozilla::ipc::IPCResult BackgroundParentImpl::RecvCreateWebTransportParent(
    const nsAString& aURL, nsIPrincipal* aPrincipal,
    const uint64_t& aBrowsingContextID, const IPCClientInfo& aClientInfo,
    const bool& aDedicated, const bool& aRequireUnreliable,
    const uint32_t& aCongestionControl,
    nsTArray<WebTransportHash>&& aServerCertHashes,
    Endpoint<PWebTransportParent>&& aParentEndpoint,
    CreateWebTransportParentResolver&& aResolver) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  RefPtr<mozilla::dom::WebTransportParent> webt =
      new mozilla::dom::WebTransportParent();
  webt->Create(aURL, aPrincipal, aBrowsingContextID, aClientInfo, aDedicated,
               aRequireUnreliable, aCongestionControl,
               std::move(aServerCertHashes), std::move(aParentEndpoint),
               std::move(aResolver));
  return IPC_OK();
}

already_AddRefed<PIdleSchedulerParent>
BackgroundParentImpl::AllocPIdleSchedulerParent() {
  AssertIsOnBackgroundThread();
  RefPtr<IdleSchedulerParent> actor = new IdleSchedulerParent();
  return actor.forget();
}

already_AddRefed<dom::PRemoteWorkerControllerParent>
BackgroundParentImpl::AllocPRemoteWorkerControllerParent(
    const dom::RemoteWorkerData& aRemoteWorkerData) {
  if (BackgroundParent::IsOtherProcessActor(this)) {
    return nullptr;
  }
  RefPtr<dom::RemoteWorkerControllerParent> actor =
      new dom::RemoteWorkerControllerParent(aRemoteWorkerData);
  return actor.forget();
}

IPCResult BackgroundParentImpl::RecvPRemoteWorkerControllerConstructor(
    dom::PRemoteWorkerControllerParent* aActor,
    const dom::RemoteWorkerData& aRemoteWorkerData) {
  MOZ_ASSERT(aActor);
  return IPC_OK();
}

mozilla::dom::PSharedWorkerParent*
BackgroundParentImpl::AllocPSharedWorkerParent(
    const mozilla::dom::RemoteWorkerData& aData, const uint64_t& aWindowID,
    const mozilla::dom::MessagePortIdentifier& aPortIdentifier) {
  RefPtr<dom::SharedWorkerParent> agent =
      new mozilla::dom::SharedWorkerParent();
  return agent.forget().take();
}

IPCResult BackgroundParentImpl::RecvPSharedWorkerConstructor(
    PSharedWorkerParent* aActor, const mozilla::dom::RemoteWorkerData& aData,
    const uint64_t& aWindowID,
    const mozilla::dom::MessagePortIdentifier& aPortIdentifier) {
  if (MOZ_UNLIKELY(aData.serviceWorkerData().type() !=
                   OptionalServiceWorkerData::Tvoid_t)) {
    return IPC_FAIL(this, "Invalid worker type for PSharedWorkerParent");
  }

  if (!BackgroundParent::ValidatePrincipalInfo(this,
                                               aData.loadingPrincipalInfo())) {
    return IPC_FAIL(this,
                    "Invalid loadingPrincipalInfo for PSharedWorkerParent");
  }

  mozilla::dom::SharedWorkerParent* actor =
      static_cast<mozilla::dom::SharedWorkerParent*>(aActor);
  actor->Initialize(aData, aWindowID, aPortIdentifier);
  return IPC_OK();
}

bool BackgroundParentImpl::DeallocPSharedWorkerParent(
    mozilla::dom::PSharedWorkerParent* aActor) {
  RefPtr<mozilla::dom::SharedWorkerParent> actor =
      dont_AddRef(static_cast<mozilla::dom::SharedWorkerParent*>(aActor));
  return true;
}

dom::PFileCreatorParent* BackgroundParentImpl::AllocPFileCreatorParent(
    const nsAString& aFullPath, const nsAString& aType, const nsAString& aName,
    const Maybe<int64_t>& aLastModified, const bool& aExistenceCheck,
    const bool& aIsFromNsIFile) {
  RefPtr<dom::FileCreatorParent> actor = new dom::FileCreatorParent();
  return actor.forget().take();
}

mozilla::ipc::IPCResult BackgroundParentImpl::RecvPFileCreatorConstructor(
    dom::PFileCreatorParent* aActor, const nsAString& aFullPath,
    const nsAString& aType, const nsAString& aName,
    const Maybe<int64_t>& aLastModified, const bool& aExistenceCheck,
    const bool& aIsFromNsIFile) {
  bool isFileRemoteType = false;

  RefPtr<ThreadsafeContentParentHandle> parent =
      BackgroundParent::GetContentParentHandle(this);
  if (!parent) {
    isFileRemoteType = true;
  } else {
    isFileRemoteType = parent->GetRemoteType() == FILE_REMOTE_TYPE;
  }

  dom::FileCreatorParent* actor = static_cast<dom::FileCreatorParent*>(aActor);

  if (!isFileRemoteType && !StaticPrefs::dom_file_createInChild()) {
    (void)dom::FileCreatorParent::Send__delete__(
        actor, dom::FileCreationErrorResult(NS_ERROR_DOM_INVALID_STATE_ERR));
    return IPC_OK();
  }

  return actor->CreateAndShareFile(aFullPath, aType, aName, aLastModified,
                                   aExistenceCheck, aIsFromNsIFile);
}

bool BackgroundParentImpl::DeallocPFileCreatorParent(
    dom::PFileCreatorParent* aActor) {
  RefPtr<dom::FileCreatorParent> actor =
      dont_AddRef(static_cast<dom::FileCreatorParent*>(aActor));
  return true;
}

dom::PTemporaryIPCBlobParent*
BackgroundParentImpl::AllocPTemporaryIPCBlobParent() {
  return new dom::TemporaryIPCBlobParent();
}

mozilla::ipc::IPCResult BackgroundParentImpl::RecvPTemporaryIPCBlobConstructor(
    dom::PTemporaryIPCBlobParent* aActor) {
  dom::TemporaryIPCBlobParent* actor =
      static_cast<dom::TemporaryIPCBlobParent*>(aActor);
  return actor->CreateAndShareFile();
}

bool BackgroundParentImpl::DeallocPTemporaryIPCBlobParent(
    dom::PTemporaryIPCBlobParent* aActor) {
  delete aActor;
  return true;
}

already_AddRefed<BackgroundParentImpl::PVsyncParent>
BackgroundParentImpl::AllocPVsyncParent() {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  RefPtr<mozilla::dom::VsyncParent> actor = new mozilla::dom::VsyncParent();

  RefPtr<mozilla::VsyncDispatcher> vsyncDispatcher =
      gfxPlatform::GetPlatform()->GetGlobalVsyncDispatcher();
  actor->UpdateVsyncDispatcher(vsyncDispatcher);
  return actor.forget();
}

mozilla::dom::PBroadcastChannelParent*
BackgroundParentImpl::AllocPBroadcastChannelParent(
    const PrincipalInfo& aPrincipalInfo, const nsACString& aOrigin,
    const nsAString& aChannel) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  nsString originChannelKey;


  originChannelKey.Assign(aChannel);

  originChannelKey.AppendLiteral("|");

  originChannelKey.Append(NS_ConvertUTF8toUTF16(aOrigin));

  return new BroadcastChannelParent(originChannelKey);
}

mozilla::ipc::IPCResult BackgroundParentImpl::RecvPBroadcastChannelConstructor(
    PBroadcastChannelParent* actor, const PrincipalInfo& aPrincipalInfo,
    const nsACString& aOrigin, const nsAString& aChannel) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  auto principalOrErr = PrincipalInfoToPrincipal(aPrincipalInfo);
  if (NS_WARN_IF(principalOrErr.isErr())) {
    return IPC_FAIL(this, "PrincipalInfoToPrincipal failed");
  }
  nsCOMPtr<nsIPrincipal> principal = principalOrErr.unwrap();

  if (!BackgroundParent::ValidatePrincipal(this, principal)) {
    return IPC_FAIL(this, "Invalid principal for BroadcastChannel");
  }

  nsAutoCString origin;
  if (NS_FAILED(principal->GetOrigin(origin))) {
    return IPC_FAIL(this, "principal::GetOrigin failed");
  }

  if (NS_WARN_IF(!aOrigin.Equals(origin))) {
    return IPC_FAIL(this, "origins do not match");
  }

  return IPC_OK();
}

bool BackgroundParentImpl::DeallocPBroadcastChannelParent(
    PBroadcastChannelParent* aActor) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  delete static_cast<BroadcastChannelParent*>(aActor);
  return true;
}

mozilla::dom::PCookieStoreParent*
BackgroundParentImpl::AllocPCookieStoreParent() {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  RefPtr<mozilla::dom::CookieStoreParent> actor =
      new mozilla::dom::CookieStoreParent();
  return actor.forget().take();
}

bool BackgroundParentImpl::DeallocPCookieStoreParent(
    PCookieStoreParent* aActor) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  RefPtr<mozilla::dom::CookieStoreParent> actor =
      dont_AddRef(static_cast<mozilla::dom::CookieStoreParent*>(aActor));
  return true;
}

mozilla::dom::PServiceWorkerManagerParent*
BackgroundParentImpl::AllocPServiceWorkerManagerParent() {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  RefPtr<dom::ServiceWorkerManagerParent> agent =
      new dom::ServiceWorkerManagerParent();
  return agent.forget().take();
}

bool BackgroundParentImpl::DeallocPServiceWorkerManagerParent(
    PServiceWorkerManagerParent* aActor) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  RefPtr<dom::ServiceWorkerManagerParent> parent =
      dont_AddRef(static_cast<dom::ServiceWorkerManagerParent*>(aActor));
  MOZ_ASSERT(parent);
  return true;
}

mozilla::ipc::IPCResult
BackgroundParentImpl::RecvShutdownServiceWorkerRegistrar() {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  if (BackgroundParent::IsOtherProcessActor(this)) {
    return IPC_FAIL_NO_REASON(this);
  }

  RefPtr<dom::ServiceWorkerRegistrar> service =
      dom::ServiceWorkerRegistrar::Get();
  MOZ_ASSERT(service);

  service->Shutdown();
  return IPC_OK();
}

already_AddRefed<PCacheStorageParent>
BackgroundParentImpl::AllocPCacheStorageParent(
    const Namespace& aNamespace, const PrincipalInfo& aPrincipalInfo) {
  if (!BackgroundParent::ValidatePrincipalInfo(this, aPrincipalInfo)) {
    return nullptr;
  }

  return dom::cache::AllocPCacheStorageParent(this, nullptr, aNamespace,
                                              aPrincipalInfo);
}

PMessagePortParent* BackgroundParentImpl::AllocPMessagePortParent(
    const nsID& aUUID, const nsID& aDestinationUUID,
    const uint32_t& aSequenceID) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return new MessagePortParent(aUUID);
}

mozilla::ipc::IPCResult BackgroundParentImpl::RecvPMessagePortConstructor(
    PMessagePortParent* aActor, const nsID& aUUID, const nsID& aDestinationUUID,
    const uint32_t& aSequenceID) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  MessagePortParent* mp = static_cast<MessagePortParent*>(aActor);
  if (!mp->Entangle(aDestinationUUID, aSequenceID)) {
    return IPC_FAIL_NO_REASON(this);
  }
  return IPC_OK();
}

bool BackgroundParentImpl::DeallocPMessagePortParent(
    PMessagePortParent* aActor) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  delete static_cast<MessagePortParent*>(aActor);
  return true;
}

mozilla::ipc::IPCResult BackgroundParentImpl::RecvMessagePortForceClose(
    const nsID& aUUID, const nsID& aDestinationUUID,
    const uint32_t& aSequenceID) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  if (!MessagePortParent::ForceClose(aUUID, aDestinationUUID, aSequenceID)) {
    return IPC_FAIL(this, "MessagePortParent::ForceClose failed.");
  }

  return IPC_OK();
}

already_AddRefed<BackgroundParentImpl::PQuotaParent>
BackgroundParentImpl::AllocPQuotaParent() {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return mozilla::dom::quota::AllocPQuotaParent();
}

mozilla::ipc::IPCResult BackgroundParentImpl::RecvShutdownQuotaManager() {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  if (BackgroundParent::IsOtherProcessActor(this)) {
    return IPC_FAIL_NO_REASON(this);
  }

  if (!mozilla::dom::quota::RecvShutdownQuotaManager()) {
    return IPC_FAIL_NO_REASON(this);
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult
BackgroundParentImpl::RecvShutdownBackgroundSessionStorageManagers() {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  if (BackgroundParent::IsOtherProcessActor(this)) {
    return IPC_FAIL_NO_REASON(this);
  }

  if (!mozilla::dom::RecvShutdownBackgroundSessionStorageManagers()) {
    return IPC_FAIL_NO_REASON(this);
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult
BackgroundParentImpl::RecvPropagateBackgroundSessionStorageManager(
    const uint64_t& aCurrentTopContextId, const uint64_t& aTargetTopContextId) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  if (BackgroundParent::IsOtherProcessActor(this)) {
    return IPC_FAIL(this, "Wrong actor");
  }

  mozilla::dom::RecvPropagateBackgroundSessionStorageManager(
      aCurrentTopContextId, aTargetTopContextId);

  return IPC_OK();
}

mozilla::ipc::IPCResult
BackgroundParentImpl::RecvRemoveBackgroundSessionStorageManager(
    const uint64_t& aTopContextId) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  if (BackgroundParent::IsOtherProcessActor(this)) {
    return IPC_FAIL_NO_REASON(this);
  }

  if (!mozilla::dom::RecvRemoveBackgroundSessionStorageManager(aTopContextId)) {
    return IPC_FAIL_NO_REASON(this);
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult BackgroundParentImpl::RecvGetSessionStorageManagerData(
    const uint64_t& aTopContextId, const uint32_t& aSizeLimit,
    const bool& aCancelSessionStoreTimer,
    GetSessionStorageManagerDataResolver&& aResolver) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  if (BackgroundParent::IsOtherProcessActor(this)) {
    return IPC_FAIL(this, "Wrong actor");
  }

  if (!mozilla::dom::RecvGetSessionStorageData(aTopContextId, aSizeLimit,
                                               aCancelSessionStoreTimer,
                                               std::move(aResolver))) {
    return IPC_FAIL(this, "Couldn't get session storage data");
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult BackgroundParentImpl::RecvLoadSessionStorageManagerData(
    const uint64_t& aTopContextId,
    nsTArray<mozilla::dom::SSCacheCopy>&& aOriginCacheCopy) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  if (BackgroundParent::IsOtherProcessActor(this)) {
    return IPC_FAIL(this, "Wrong actor");
  }

  if (!mozilla::dom::RecvLoadSessionStorageData(aTopContextId,
                                                std::move(aOriginCacheCopy))) {
    return IPC_FAIL_NO_REASON(this);
  }

  return IPC_OK();
}

already_AddRefed<dom::PFileSystemRequestParent>
BackgroundParentImpl::AllocPFileSystemRequestParent(
    const FileSystemParams& aParams) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  RefPtr<FileSystemRequestParent> result = new FileSystemRequestParent();

  if (NS_WARN_IF(!result->Initialize(aParams))) {
    return nullptr;
  }

  return result.forget();
}

mozilla::ipc::IPCResult BackgroundParentImpl::RecvPFileSystemRequestConstructor(
    PFileSystemRequestParent* aActor, const FileSystemParams& params) {
  static_cast<FileSystemRequestParent*>(aActor)->Start();
  return IPC_OK();
}

already_AddRefed<net::PHttpBackgroundChannelParent>
BackgroundParentImpl::AllocPHttpBackgroundChannelParent(
    const uint64_t& aChannelId) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  RefPtr<net::HttpBackgroundChannelParent> actor =
      new net::HttpBackgroundChannelParent();
  return actor.forget();
}

mozilla::ipc::IPCResult
BackgroundParentImpl::RecvPHttpBackgroundChannelConstructor(
    net::PHttpBackgroundChannelParent* aActor, const uint64_t& aChannelId) {
  MOZ_ASSERT(aActor);
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  net::HttpBackgroundChannelParent* aParent =
      static_cast<net::HttpBackgroundChannelParent*>(aActor);

  dom::ContentParentId cpId(BackgroundParent::GetChildID(this));

  if (NS_WARN_IF(NS_FAILED(aParent->Init(cpId, aChannelId)))) {
    return IPC_FAIL_NO_REASON(this);
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult BackgroundParentImpl::RecvCreateBoundStorageKeyParent(
    Endpoint<::mozilla::dom::cache::PBoundStorageKeyParent>&& aEndpoint,
    const Namespace& aNamespace, const PrincipalInfo& aPrincipalInfo) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  if (!BackgroundParent::ValidatePrincipalInfo(this, aPrincipalInfo)) {
    return IPC_FAIL(this, "Invalid principalInfo for BoundStorageKeyParent");
  }

  if (!aEndpoint.IsValid()) {
    return IPC_FAIL(this, "Invalid endpoint for BoundStorageKeyParent");
  }

  auto* pActor = new dom::cache::BoundStorageKeyParent(this);
  if (!aEndpoint.Bind(pActor)) {
    return IPC_FAIL(this,
                    "Failed to bind endpoint for BoundStorageKeyParent actor");
  }

  return IPC_OK();
}

already_AddRefed<mozilla::dom::PClientManagerParent>
BackgroundParentImpl::AllocPClientManagerParent() {
  return mozilla::dom::AllocClientManagerParent();
}

mozilla::ipc::IPCResult BackgroundParentImpl::RecvPClientManagerConstructor(
    mozilla::dom::PClientManagerParent* aActor) {
  mozilla::dom::InitClientManagerParent(aActor);
  return IPC_OK();
}

IPCResult BackgroundParentImpl::RecvStorageActivity(
    const PrincipalInfo& aPrincipalInfo) {
  if (!BackgroundParent::ValidatePrincipalInfo(this, aPrincipalInfo)) {
    return IPC_FAIL(this, "Invalid principalInfo for StorageActivity");
  }

  dom::StorageActivityService::SendActivity(aPrincipalInfo);
  return IPC_OK();
}

IPCResult BackgroundParentImpl::RecvPServiceWorkerManagerConstructor(
    PServiceWorkerManagerParent* const aActor) {
  if (BackgroundParent::IsOtherProcessActor(this)) {
    return IPC_FAIL_NO_REASON(aActor);
  }
  return IPC_OK();
}

already_AddRefed<PServiceWorkerParent>
BackgroundParentImpl::AllocPServiceWorkerParent(
    const IPCServiceWorkerDescriptor&) {
  return MakeAndAddRef<ServiceWorkerParent>();
}

IPCResult BackgroundParentImpl::RecvPServiceWorkerConstructor(
    PServiceWorkerParent* aActor,
    const IPCServiceWorkerDescriptor& aDescriptor) {
  dom::InitServiceWorkerParent(aActor, aDescriptor);
  return IPC_OK();
}

already_AddRefed<PServiceWorkerContainerParent>
BackgroundParentImpl::AllocPServiceWorkerContainerParent() {
  return MakeAndAddRef<mozilla::dom::ServiceWorkerContainerParent>();
}

mozilla::ipc::IPCResult
BackgroundParentImpl::RecvPServiceWorkerContainerConstructor(
    PServiceWorkerContainerParent* aActor) {
  dom::InitServiceWorkerContainerParent(aActor);
  return IPC_OK();
}

already_AddRefed<PServiceWorkerRegistrationParent>
BackgroundParentImpl::AllocPServiceWorkerRegistrationParent(
    const IPCServiceWorkerRegistrationDescriptor&, const IPCClientInfo&) {
  return MakeAndAddRef<mozilla::dom::ServiceWorkerRegistrationParent>();
}

mozilla::ipc::IPCResult
BackgroundParentImpl::RecvPServiceWorkerRegistrationConstructor(
    PServiceWorkerRegistrationParent* aActor,
    const IPCServiceWorkerRegistrationDescriptor& aDescriptor,
    const IPCClientInfo& aForClient) {
  dom::InitServiceWorkerRegistrationParent(aActor, aDescriptor, aForClient);
  return IPC_OK();
}

mozilla::ipc::IPCResult
BackgroundParentImpl::RecvEnsureRDDProcessAndCreateBridge(
    EnsureRDDProcessAndCreateBridgeResolver&& aResolver) {
  using Type = std::tuple<const nsresult&,
                          Endpoint<mozilla::PRemoteMediaManagerChild>&&>;

  RefPtr<ThreadsafeContentParentHandle> parent =
      BackgroundParent::GetContentParentHandle(this);
  if (NS_WARN_IF(!parent)) {
    aResolver(
        Type(NS_ERROR_NOT_AVAILABLE, Endpoint<PRemoteMediaManagerChild>()));
    return IPC_OK();
  }

  RDDProcessManager* rdd = RDDProcessManager::Get();
  if (!rdd) {
    aResolver(
        Type(NS_ERROR_NOT_AVAILABLE, Endpoint<PRemoteMediaManagerChild>()));
    return IPC_OK();
  }

  rdd->EnsureRDDProcessAndCreateBridge(OtherEndpointProcInfo(),
                                       parent->ChildID())
      ->Then(GetCurrentSerialEventTarget(), __func__,
             [resolver = std::move(aResolver)](
                 mozilla::RDDProcessManager::EnsureRDDPromise::
                     ResolveOrRejectValue&& aValue) mutable {
               if (aValue.IsReject()) {
                 resolver(Type(aValue.RejectValue(),
                               Endpoint<PRemoteMediaManagerChild>()));
                 return;
               }
               resolver(Type(NS_OK, std::move(aValue.ResolveValue())));
             });
  return IPC_OK();
}

mozilla::ipc::IPCResult
BackgroundParentImpl::RecvEnsureUtilityProcessAndCreateBridge(
    const RemoteMediaIn& aLocation,
    EnsureUtilityProcessAndCreateBridgeResolver&& aResolver) {
  EndpointProcInfo otherProcInfo = OtherEndpointProcInfo();
  RefPtr<ThreadsafeContentParentHandle> parent =
      BackgroundParent::GetContentParentHandle(this);
  if (NS_WARN_IF(!parent)) {
    return IPC_FAIL_NO_REASON(this);
  }
  dom::ContentParentId childId = parent->ChildID();
  nsCOMPtr<nsISerialEventTarget> managerThread = GetCurrentSerialEventTarget();
  if (!managerThread) {
    return IPC_FAIL_NO_REASON(this);
  }
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "BackgroundParentImpl::RecvEnsureUtilityProcessAndCreateBridge()",
      [aResolver = std::move(aResolver), managerThread, otherProcInfo, childId,
       aLocation]() {
        RefPtr<UtilityProcessManager> upm =
            UtilityProcessManager::GetSingleton();
        using Type = std::tuple<const nsresult&,
                                Endpoint<mozilla::PRemoteMediaManagerChild>&&>;
        if (!upm) {
          managerThread->Dispatch(NS_NewRunnableFunction(
              "BackgroundParentImpl::RecvEnsureUtilityProcessAndCreateBridge::"
              "Failure",
              [aResolver]() {
                aResolver(Type(NS_ERROR_NOT_AVAILABLE,
                               Endpoint<PRemoteMediaManagerChild>()));
              }));
        } else {
          UtilityProcessKind sbKind = GetUtilityProcessKindFromLocation(aLocation);
          upm->StartProcessForRemoteMediaDecoding(otherProcInfo, childId,
                                                  sbKind)
              ->Then(managerThread, __func__,
                     [resolver = aResolver](
                         mozilla::ipc::UtilityProcessManager::
                             StartRemoteDecodingUtilityPromise::
                                 ResolveOrRejectValue&& aValue) mutable {
                       if (aValue.IsReject()) {
                         resolver(Type(NS_ERROR_FAILURE,
                                       Endpoint<PRemoteMediaManagerChild>()));
                         return;
                       }
                       resolver(Type(NS_OK, std::move(aValue.ResolveValue())));
                     });
        }
      }));
  return IPC_OK();
}

mozilla::ipc::IPCResult BackgroundParentImpl::RecvPLockManagerConstructor(
    PLockManagerParent* aActor, mozilla::NotNull<nsIPrincipal*> aPrincipalInfo,
    const Maybe<nsID>& aClientId) {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  if (aPrincipalInfo->IsSystemPrincipal() &&
      BackgroundParent::IsOtherProcessActor(this)) {
    return IPC_FAIL_NO_REASON(this);
  }

  return IPC_OK();
}

already_AddRefed<dom::locks::PLockManagerParent>
BackgroundParentImpl::AllocPLockManagerParent(NotNull<nsIPrincipal*> aPrincipal,
                                              const Maybe<nsID>& aClientId) {
  return MakeAndAddRef<dom::locks::LockManagerParent>(aPrincipal, aClientId);
}

already_AddRefed<dom::PFetchParent> BackgroundParentImpl::AllocPFetchParent() {
  return MakeAndAddRef<dom::FetchParent>();
}

}  
