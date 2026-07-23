/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_serviceworkerprivate_h
#define mozilla_dom_serviceworkerprivate_h

#include <functional>

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"
#include "mozilla/MozPromise.h"
#include "mozilla/RefPtr.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/dom/FetchService.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/RemoteWorkerController.h"
#include "mozilla/dom/RemoteWorkerTypes.h"
#include "mozilla/dom/ServiceWorkerLifetimeExtension.h"
#include "mozilla/dom/ServiceWorkerOpArgs.h"
#include "nsCOMPtr.h"
#include "nsISupportsImpl.h"
#include "nsTArray.h"

class nsIInterceptedChannel;
class nsIWorkerDebugger;

namespace mozilla {

template <typename T>
class Maybe;

class JSObjectHolder;

namespace net {
class CookieStruct;
}

namespace dom {

class PostMessageSource;
class RemoteWorkerControllerChild;
class ServiceWorkerInfo;
class ServiceWorkerPrivate;
class ServiceWorkerRegistrationInfo;
struct CookieListItem;

namespace ipc {
class StructuredCloneData;
}  

class LifeCycleEventCallback : public Runnable {
 public:
  LifeCycleEventCallback() : Runnable("dom::LifeCycleEventCallback") {}

  virtual void SetResult(bool aResult) = 0;
};

class KeepAliveToken final : public nsISupports {
 public:
  NS_DECL_ISUPPORTS

  explicit KeepAliveToken(ServiceWorkerPrivate* aPrivate);

 private:
  ~KeepAliveToken();

  RefPtr<ServiceWorkerPrivate> mPrivate;
};

class ServiceWorkerPrivate final : public RemoteWorkerObserver {
  friend class KeepAliveToken;

 public:
  NS_INLINE_DECL_REFCOUNTING(ServiceWorkerPrivate, override);


 public:
  explicit ServiceWorkerPrivate(ServiceWorkerInfo* aInfo);

  Maybe<ClientInfo> GetClientInfo() { return mClientInfo; }

  nsresult SendMessageEvent(
      ipc::StructuredCloneData* aData,
      const ServiceWorkerLifetimeExtension& aLifetimeExtension,
      const PostMessageSource& aSource);

  nsresult CheckScriptEvaluation(
      const ServiceWorkerLifetimeExtension& aLifetimeExtension,
      RefPtr<LifeCycleEventCallback> aCallback);

  nsresult SendLifeCycleEvent(
      const nsAString& aEventType,
      const ServiceWorkerLifetimeExtension& aLifetimeExtension,
      const RefPtr<LifeCycleEventCallback>& aCallback);

  nsresult SendCookieChangeEvent(
      const net::CookieStruct& aCookie, bool aCookieDeleted,
      RefPtr<ServiceWorkerRegistrationInfo> aRegistration);

  nsresult SendFetchEvent(nsCOMPtr<nsIInterceptedChannel> aChannel,
                          nsILoadGroup* aLoadGroup, const nsAString& aClientId,
                          const nsAString& aResultingClientId);

  void TerminateWorker(Maybe<RefPtr<Promise>> aMaybePromise = Nothing());

  void NoteDeadServiceWorkerInfo();

  void NoteStoppedControllingDocuments();

  void UpdateState(ServiceWorkerState aState);

  void UpdateIsOnContentBlockingAllowList(bool aOnContentBlockingAllowList);

  nsresult GetDebugger(nsIWorkerDebugger** aResult);

  nsresult AttachDebugger();

  nsresult DetachDebugger();

  TimeStamp GetLifetimeDeadline() { return mIdleDeadline; }

  uint32_t GetLaunchCount() { return mLaunchCount; }

  bool IsIdle() const;

  RefPtr<GenericPromise> GetIdlePromise();

  void SetHandlesFetch(bool aValue);

  RefPtr<GenericPromise> SetSkipWaitingFlag();

  static void RunningShutdown() {
    UpdateRunning(0, 0);
    MOZ_ASSERT(sRunningServiceWorkers == 0);
    MOZ_ASSERT(sRunningServiceWorkersFetch == 0);
  }

  static void UpdateRunning(int32_t aDelta, int32_t aFetchDelta);

 private:
  void NoteIdleWorkerCallback(nsITimer* aTimer);

  void TerminateWorkerCallback(nsITimer* aTimer);

  void RenewKeepAliveToken(
      const ServiceWorkerLifetimeExtension& aLifetimeExtension);

  void ResetIdleTimeout(
      const ServiceWorkerLifetimeExtension& aLifetimeExtension);

  void AddToken();

  void ReleaseToken();

  already_AddRefed<KeepAliveToken> CreateEventKeepAliveToken();

  nsresult SpawnWorkerIfNeeded(
      const ServiceWorkerLifetimeExtension& aLifetimeExtension);

  ~ServiceWorkerPrivate();

  nsresult Initialize();

  void RegenerateClientInfo();

  void CreationFailed() override;

  void CreationSucceeded() override;

  void ErrorReceived(const ErrorValue& aError) override;

  void LockNotified(bool aCreated) final {
  }

  void WebTransportNotified(bool aCreated) final {
  }

  void Terminated() override;

  void RefreshRemoteWorkerData(
      const RefPtr<ServiceWorkerRegistrationInfo>& aRegistration);

  nsresult SendCookieChangeEventInternal(
      RefPtr<ServiceWorkerRegistrationInfo>&& aRegistration,
      ServiceWorkerCookieChangeEventOpArgs&& aArgs);

  RefPtr<FetchServicePromises> SetupNavigationPreload(
      nsCOMPtr<nsIInterceptedChannel>& aChannel,
      const RefPtr<ServiceWorkerRegistrationInfo>& aRegistration);

  nsresult SendFetchEventInternal(
      RefPtr<ServiceWorkerRegistrationInfo>&& aRegistration,
      ParentToParentServiceWorkerFetchEventOpArgs&& aArgs,
      nsCOMPtr<nsIInterceptedChannel>&& aChannel,
      RefPtr<FetchServicePromises>&& aPreloadResponseReadyPromises);

  void Shutdown(Maybe<RefPtr<Promise>>&& aMaybePromise = Nothing());

  RefPtr<GenericNonExclusivePromise> ShutdownInternal(
      uint32_t aShutdownStateId);

  nsresult ExecServiceWorkerOp(
      ServiceWorkerOpArgs&& aArgs,
      const ServiceWorkerLifetimeExtension& aLifetimeExtension,
      std::function<void(ServiceWorkerOpResult&&)>&& aSuccessCallback,
      std::function<void()>&& aFailureCallback = [] {});

  class PendingFunctionalEvent {
   public:
    PendingFunctionalEvent(
        ServiceWorkerPrivate* aOwner,
        RefPtr<ServiceWorkerRegistrationInfo>&& aRegistration);

    virtual ~PendingFunctionalEvent();

    virtual nsresult Send() = 0;

   protected:
    ServiceWorkerPrivate* const MOZ_NON_OWNING_REF mOwner;
    RefPtr<ServiceWorkerRegistrationInfo> mRegistration;
  };

  class PendingCookieChangeEvent final : public PendingFunctionalEvent {
   public:
    PendingCookieChangeEvent(
        ServiceWorkerPrivate* aOwner,
        RefPtr<ServiceWorkerRegistrationInfo>&& aRegistration,
        ServiceWorkerCookieChangeEventOpArgs&& aArgs);

    nsresult Send() override;

   private:
    ServiceWorkerCookieChangeEventOpArgs mArgs;
  };

  class PendingFetchEvent final : public PendingFunctionalEvent {
   public:
    PendingFetchEvent(
        ServiceWorkerPrivate* aOwner,
        RefPtr<ServiceWorkerRegistrationInfo>&& aRegistration,
        ParentToParentServiceWorkerFetchEventOpArgs&& aArgs,
        nsCOMPtr<nsIInterceptedChannel>&& aChannel,
        RefPtr<FetchServicePromises>&& aPreloadResponseReadyPromises);

    nsresult Send() override;

    ~PendingFetchEvent();

   private:
    ParentToParentServiceWorkerFetchEventOpArgs mArgs;
    nsCOMPtr<nsIInterceptedChannel> mChannel;
    RefPtr<FetchServicePromises> mPreloadResponseReadyPromises;
  };

  nsTArray<UniquePtr<PendingFunctionalEvent>> mPendingFunctionalEvents;

  class RAIIActorPtrHolder final {
   public:
    NS_INLINE_DECL_REFCOUNTING(RAIIActorPtrHolder)

    explicit RAIIActorPtrHolder(
        already_AddRefed<RemoteWorkerControllerChild> aActor);

    RAIIActorPtrHolder(const RAIIActorPtrHolder& aOther) = delete;
    RAIIActorPtrHolder& operator=(const RAIIActorPtrHolder& aOther) = delete;

    RAIIActorPtrHolder(RAIIActorPtrHolder&& aOther) = delete;
    RAIIActorPtrHolder& operator=(RAIIActorPtrHolder&& aOther) = delete;

    RemoteWorkerControllerChild* operator->() const
        MOZ_NO_ADDREF_RELEASE_ON_RETURN;

    RemoteWorkerControllerChild* get() const;

    RefPtr<GenericPromise> OnDestructor();

   private:
    ~RAIIActorPtrHolder();

    MozPromiseHolder<GenericPromise> mDestructorPromiseHolder;

    const RefPtr<RemoteWorkerControllerChild> mActor;
  };

  RefPtr<RAIIActorPtrHolder> mControllerChild;

  RemoteWorkerData mRemoteWorkerData;
  Maybe<ClientInfo> mClientInfo;

  TimeStamp mServiceWorkerLaunchTimeStart;

  static uint32_t sRunningServiceWorkers;
  static uint32_t sRunningServiceWorkersFetch;

  enum { Unknown, Enabled, Disabled } mHandlesFetch{Unknown};

  ServiceWorkerInfo* MOZ_NON_OWNING_REF mInfo;

  nsCOMPtr<nsITimer> mIdleWorkerTimer;

  ServiceWorkerLifetimeExtension mPendingSpawnLifetime;

  TimeStamp mIdleDeadline;

  RefPtr<KeepAliveToken> mIdleKeepAliveToken;

  uint64_t mDebuggerCount;

  uint64_t mTokenCount;

  uint32_t mLaunchCount;

  MozPromiseHolder<GenericPromise> mIdlePromiseHolder;

#ifdef DEBUG
  bool mIdlePromiseObtained = false;
#endif
};

}  
}  

#endif  // mozilla_dom_serviceworkerprivate_h
