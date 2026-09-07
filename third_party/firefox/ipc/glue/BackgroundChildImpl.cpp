/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BackgroundChildImpl.h"

#include "BroadcastChannelChild.h"
#include "mozilla/Assertions.h"
#include "mozilla/SchedulerGroup.h"
#include "mozilla/dom/ClientManagerActors.h"
#include "mozilla/dom/FileCreatorChild.h"
#include "mozilla/dom/PBackgroundLSDatabaseChild.h"
#include "mozilla/dom/PBackgroundLSObserverChild.h"
#include "mozilla/dom/PBackgroundLSRequestChild.h"
#include "mozilla/dom/PBackgroundLSSimpleRequestChild.h"
#include "mozilla/dom/PBackgroundSDBConnectionChild.h"
#include "mozilla/dom/CookieStoreChild.h"
#include "mozilla/dom/PFileSystemRequestChild.h"
#include "mozilla/dom/PVsync.h"
#include "mozilla/dom/TemporaryIPCBlobChild.h"
#include "mozilla/dom/cache/ActorUtils.h"
#include "mozilla/dom/indexedDB/PBackgroundIndexedDBUtilsChild.h"
#include "mozilla/dom/indexedDB/ThreadLocal.h"
#include "mozilla/dom/quota/PQuotaChild.h"
#include "mozilla/dom/RemoteWorkerControllerChild.h"
#include "mozilla/dom/RemoteWorkerServiceChild.h"
#include "mozilla/dom/ServiceWorkerChild.h"
#include "mozilla/dom/SharedWorkerChild.h"
#include "mozilla/dom/StorageIPC.h"
#include "mozilla/dom/MessagePortChild.h"
#include "mozilla/dom/ServiceWorkerContainerChild.h"
#include "mozilla/dom/ServiceWorkerManagerChild.h"
#include "nsID.h"

namespace mozilla::ipc {

using mozilla::dom::PServiceWorkerChild;
using mozilla::dom::PServiceWorkerContainerChild;
using mozilla::dom::PServiceWorkerRegistrationChild;
using mozilla::dom::StorageDBChild;
using mozilla::dom::cache::PCacheChild;
using mozilla::dom::cache::PCacheStreamControlChild;



BackgroundChildImpl::ThreadLocal::ThreadLocal() : mCurrentFileHandle(nullptr) {
  MOZ_COUNT_CTOR(mozilla::ipc::BackgroundChildImpl::ThreadLocal);
}

BackgroundChildImpl::ThreadLocal::~ThreadLocal() {
  MOZ_COUNT_DTOR(mozilla::ipc::BackgroundChildImpl::ThreadLocal);
}


BackgroundChildImpl::BackgroundChildImpl() {
  MOZ_COUNT_CTOR(mozilla::ipc::BackgroundChildImpl);
}

BackgroundChildImpl::~BackgroundChildImpl() {
  MOZ_COUNT_DTOR(mozilla::ipc::BackgroundChildImpl);
}

void BackgroundChildImpl::ProcessingError(Result aCode, const char* aReason) {

  nsAutoCString abortMessage;

  switch (aCode) {
    case MsgDropped:
      return;

#define HANDLE_CASE(_result)              \
  case _result:                           \
    abortMessage.AssignLiteral(#_result); \
    break

      HANDLE_CASE(MsgNotKnown);
      HANDLE_CASE(MsgNotAllowed);
      HANDLE_CASE(MsgPayloadError);
      HANDLE_CASE(MsgProcessingError);
      HANDLE_CASE(MsgValueError);

#undef HANDLE_CASE

    default:
      MOZ_CRASH("Unknown error code!");
  }

  MOZ_CRASH_UNSAFE_PRINTF("%s: %s", abortMessage.get(), aReason);
}

void BackgroundChildImpl::ActorDestroy(ActorDestroyReason aWhy) {
}

BackgroundChildImpl::PBackgroundIndexedDBUtilsChild*
BackgroundChildImpl::AllocPBackgroundIndexedDBUtilsChild() {
  MOZ_CRASH(
      "PBackgroundIndexedDBUtilsChild actors should be manually "
      "constructed!");
}

bool BackgroundChildImpl::DeallocPBackgroundIndexedDBUtilsChild(
    PBackgroundIndexedDBUtilsChild* aActor) {
  MOZ_ASSERT(aActor);

  delete aActor;
  return true;
}

BackgroundChildImpl::PBackgroundLSObserverChild*
BackgroundChildImpl::AllocPBackgroundLSObserverChild(
    const uint64_t& aObserverId) {
  MOZ_CRASH("PBackgroundLSObserverChild actor should be manually constructed!");
}

bool BackgroundChildImpl::DeallocPBackgroundLSObserverChild(
    PBackgroundLSObserverChild* aActor) {
  MOZ_ASSERT(aActor);

  delete aActor;
  return true;
}

BackgroundChildImpl::PBackgroundLSRequestChild*
BackgroundChildImpl::AllocPBackgroundLSRequestChild(
    const LSRequestParams& aParams) {
  MOZ_CRASH("PBackgroundLSRequestChild actor should be manually constructed!");
}

bool BackgroundChildImpl::DeallocPBackgroundLSRequestChild(
    PBackgroundLSRequestChild* aActor) {
  MOZ_ASSERT(aActor);

  delete aActor;
  return true;
}

BackgroundChildImpl::PBackgroundLocalStorageCacheChild*
BackgroundChildImpl::AllocPBackgroundLocalStorageCacheChild(
    const PrincipalInfo& aPrincipalInfo, const nsACString& aOriginKey,
    const uint32_t& aPrivateBrowsingId) {
  MOZ_CRASH(
      "PBackgroundLocalStorageChild actors should be manually "
      "constructed!");
}

bool BackgroundChildImpl::DeallocPBackgroundLocalStorageCacheChild(
    PBackgroundLocalStorageCacheChild* aActor) {
  MOZ_ASSERT(aActor);

  delete aActor;
  return true;
}

BackgroundChildImpl::PBackgroundLSSimpleRequestChild*
BackgroundChildImpl::AllocPBackgroundLSSimpleRequestChild(
    const LSSimpleRequestParams& aParams) {
  MOZ_CRASH(
      "PBackgroundLSSimpleRequestChild actor should be manually "
      "constructed!");
}

bool BackgroundChildImpl::DeallocPBackgroundLSSimpleRequestChild(
    PBackgroundLSSimpleRequestChild* aActor) {
  MOZ_ASSERT(aActor);

  delete aActor;
  return true;
}

BackgroundChildImpl::PBackgroundStorageChild*
BackgroundChildImpl::AllocPBackgroundStorageChild(
    const nsAString& aProfilePath, const uint32_t& aPrivateBrowsingId) {
  MOZ_CRASH("PBackgroundStorageChild actors should be manually constructed!");
}

bool BackgroundChildImpl::DeallocPBackgroundStorageChild(
    PBackgroundStorageChild* aActor) {
  MOZ_ASSERT(aActor);

  StorageDBChild* child = static_cast<StorageDBChild*>(aActor);
  child->ReleaseIPDLReference();
  return true;
}

dom::PSharedWorkerChild* BackgroundChildImpl::AllocPSharedWorkerChild(
    const dom::RemoteWorkerData& aData, const uint64_t& aWindowID,
    const dom::MessagePortIdentifier& aPortIdentifier) {
  RefPtr<dom::SharedWorkerChild> agent = new dom::SharedWorkerChild();
  return agent.forget().take();
}

bool BackgroundChildImpl::DeallocPSharedWorkerChild(
    dom::PSharedWorkerChild* aActor) {
  RefPtr<dom::SharedWorkerChild> actor =
      dont_AddRef(static_cast<dom::SharedWorkerChild*>(aActor));
  return true;
}

dom::PTemporaryIPCBlobChild*
BackgroundChildImpl::AllocPTemporaryIPCBlobChild() {
  MOZ_CRASH("This is not supposed to be called.");
  return nullptr;
}

bool BackgroundChildImpl::DeallocPTemporaryIPCBlobChild(
    dom::PTemporaryIPCBlobChild* aActor) {
  RefPtr<dom::TemporaryIPCBlobChild> actor =
      dont_AddRef(static_cast<dom::TemporaryIPCBlobChild*>(aActor));
  return true;
}

dom::PFileCreatorChild* BackgroundChildImpl::AllocPFileCreatorChild(
    const nsAString& aFullPath, const nsAString& aType, const nsAString& aName,
    const Maybe<int64_t>& aLastModified, const bool& aExistenceCheck,
    const bool& aIsFromNsIFile) {
  return new dom::FileCreatorChild();
}

bool BackgroundChildImpl::DeallocPFileCreatorChild(PFileCreatorChild* aActor) {
  delete static_cast<dom::FileCreatorChild*>(aActor);
  return true;
}


dom::PBroadcastChannelChild* BackgroundChildImpl::AllocPBroadcastChannelChild(
    const PrincipalInfo& aPrincipalInfo, const nsACString& aOrigin,
    const nsAString& aChannel) {
  RefPtr<dom::BroadcastChannelChild> agent = new dom::BroadcastChannelChild();
  return agent.forget().take();
}

bool BackgroundChildImpl::DeallocPBroadcastChannelChild(
    PBroadcastChannelChild* aActor) {
  RefPtr<dom::BroadcastChannelChild> child =
      dont_AddRef(static_cast<dom::BroadcastChannelChild*>(aActor));
  MOZ_ASSERT(child);
  return true;
}


dom::PCookieStoreChild* BackgroundChildImpl::AllocPCookieStoreChild() {
  RefPtr<dom::CookieStoreChild> child = new dom::CookieStoreChild();
  return child.forget().take();
}

bool BackgroundChildImpl::DeallocPCookieStoreChild(PCookieStoreChild* aActor) {
  RefPtr<dom::CookieStoreChild> child =
      dont_AddRef(static_cast<dom::CookieStoreChild*>(aActor));
  MOZ_ASSERT(child);
  return true;
}


dom::PServiceWorkerManagerChild*
BackgroundChildImpl::AllocPServiceWorkerManagerChild() {
  RefPtr<dom::ServiceWorkerManagerChild> agent =
      new dom::ServiceWorkerManagerChild();
  return agent.forget().take();
}

bool BackgroundChildImpl::DeallocPServiceWorkerManagerChild(
    PServiceWorkerManagerChild* aActor) {
  RefPtr<dom::ServiceWorkerManagerChild> child =
      dont_AddRef(static_cast<dom::ServiceWorkerManagerChild*>(aActor));
  MOZ_ASSERT(child);
  return true;
}


already_AddRefed<PCacheChild> BackgroundChildImpl::AllocPCacheChild() {
  return dom::cache::AllocPCacheChild();
}

already_AddRefed<PCacheStreamControlChild>
BackgroundChildImpl::AllocPCacheStreamControlChild() {
  return dom::cache::AllocPCacheStreamControlChild();
}


dom::PMessagePortChild* BackgroundChildImpl::AllocPMessagePortChild(
    const nsID& aUUID, const nsID& aDestinationUUID,
    const uint32_t& aSequenceID) {
  RefPtr<dom::MessagePortChild> agent = new dom::MessagePortChild();
  return agent.forget().take();
}

bool BackgroundChildImpl::DeallocPMessagePortChild(PMessagePortChild* aActor) {
  RefPtr<dom::MessagePortChild> child =
      dont_AddRef(static_cast<dom::MessagePortChild*>(aActor));
  MOZ_ASSERT(child);
  return true;
}

already_AddRefed<PServiceWorkerChild>
BackgroundChildImpl::AllocPServiceWorkerChild(
    const IPCServiceWorkerDescriptor&) {
  MOZ_CRASH("Shouldn't be called.");
  return {};
}

already_AddRefed<PServiceWorkerContainerChild>
BackgroundChildImpl::AllocPServiceWorkerContainerChild() {
  return mozilla::dom::ServiceWorkerContainerChild::Create();
}

already_AddRefed<PServiceWorkerRegistrationChild>
BackgroundChildImpl::AllocPServiceWorkerRegistrationChild(
    const IPCServiceWorkerRegistrationDescriptor&) {
  MOZ_CRASH("Shouldn't be called.");
  return {};
}

}  
