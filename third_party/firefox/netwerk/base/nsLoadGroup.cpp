/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/DebugOnly.h"

#include "nsLoadGroup.h"

#include "nsArrayEnumerator.h"
#include "nsCOMArray.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "mozilla/Logging.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsIInterfaceRequestor.h"
#include "nsIRequestObserver.h"
#include "CacheObserver.h"
#include "MainThreadUtils.h"
#include "RequestContextService.h"
#include "mozilla/StoragePrincipalHelper.h"
#include "mozilla/net/NeckoCommon.h"
#include "mozilla/net/NeckoChild.h"
#include "mozilla/StaticPrefs_network.h"

namespace mozilla {
namespace net {

static LazyLogModule gLoadGroupLog("LoadGroup");
#undef LOG
#define LOG(args) MOZ_LOG(gLoadGroupLog, mozilla::LogLevel::Debug, args)


static void RescheduleRequest(nsIRequest* aRequest, int32_t delta) {
  nsCOMPtr<nsISupportsPriority> p = do_QueryInterface(aRequest);
  if (p) p->AdjustPriority(delta);
}

nsLoadGroup::nsLoadGroup() { LOG(("LOADGROUP [%p]: Created.\n", this)); }

nsLoadGroup::~nsLoadGroup() {
  DebugOnly<nsresult> rv =
      CancelWithReason(NS_BINDING_ABORTED, "nsLoadGroup::~nsLoadGroup"_ns);
  NS_ASSERTION(NS_SUCCEEDED(rv), "Cancel failed");

  mDefaultLoadRequest = nullptr;

  if (mRequestContext && !mExternalRequestContext) {
    mRequestContextService->RemoveRequestContext(mRequestContext->GetID());
    if (IsNeckoChild() && gNeckoChild && gNeckoChild->CanSend()) {
      gNeckoChild->SendRemoveRequestContext(mRequestContext->GetID());
    }
  }

  nsCOMPtr<nsIObserverService> os = mozilla::services::GetObserverService();
  if (os) {
    (void)os->RemoveObserver(this, "last-pb-context-exited");
  }

  LOG(("LOADGROUP [%p]: Destroyed.\n", this));
}


NS_IMPL_ISUPPORTS(nsLoadGroup, nsILoadGroup, nsILoadGroupChild, nsIRequest,
                  nsISupportsPriority, nsISupportsWeakReference, nsIObserver)


NS_IMETHODIMP
nsLoadGroup::GetName(nsACString& result) {

  if (!mDefaultLoadRequest) {
    result.Truncate();
    return NS_OK;
  }

  return mDefaultLoadRequest->GetName(result);
}

NS_IMETHODIMP
nsLoadGroup::IsPending(bool* aResult) {
  *aResult = mForegroundCount > 0;
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::GetStatus(nsresult* status) {
  if (NS_SUCCEEDED(mStatus) && mDefaultLoadRequest) {
    return mDefaultLoadRequest->GetStatus(status);
  }

  *status = mStatus;
  return NS_OK;
}

NS_IMETHODIMP nsLoadGroup::SetCanceledReason(const nsACString& aReason) {
  return SetCanceledReasonImpl(aReason);
}

NS_IMETHODIMP nsLoadGroup::GetCanceledReason(nsACString& aReason) {
  return GetCanceledReasonImpl(aReason);
}

NS_IMETHODIMP nsLoadGroup::CancelWithReason(nsresult aStatus,
                                            const nsACString& aReason) {
  return CancelWithReasonImpl(aStatus, aReason);
}

NS_IMETHODIMP
nsLoadGroup::Cancel(nsresult status) {
  MOZ_ASSERT(NS_IsMainThread());

  NS_ASSERTION(NS_FAILED(status), "shouldn't cancel with a success code");
  nsresult rv;
  uint32_t count = mRequests.Count();

  auto requests = ToTArray<AutoTArray<nsCOMPtr<nsIRequest>, 8>>(mRequests);
  MOZ_ASSERT(requests.Length() == count);

  mStatus = status;

  mIsCanceling = true;

  nsresult firstError = NS_OK;
  while (count > 0) {
    nsCOMPtr<nsIRequest> request = requests.ElementAt(--count);

    NS_ASSERTION(request, "NULL request found in list.");

    if (!mRequests.Contains(request)) {
      requests.ElementAt(count) = nullptr;

      continue;
    }

    if (MOZ_LOG_TEST(gLoadGroupLog, LogLevel::Debug)) {
      nsAutoCString nameStr;
      request->GetName(nameStr);
      LOG(("LOADGROUP [%p]: Canceling request %p %s.\n", this, request.get(),
           nameStr.get()));
    }

    rv = request->CancelWithReason(status, mCanceledReason);

    if (NS_FAILED(rv) && NS_SUCCEEDED(firstError)) firstError = rv;

    if (NS_FAILED(RemoveRequestFromHashtable(request, status))) {
      requests.ElementAt(count) = nullptr;

      continue;
    }
  }

  for (count = requests.Length(); count > 0;) {
    nsCOMPtr<nsIRequest> request = requests.ElementAt(--count).forget();
    (void)NotifyRemovalObservers(request, status);
  }

  if (mRequestContext) {
    (void)mRequestContext->CancelTailPendingRequests(status);
  }

#if defined(DEBUG)
  NS_ASSERTION(mRequests.IsEmpty(), "Request list is not empty.");
  NS_ASSERTION(mForegroundCount == 0, "Foreground URLs are active.");
#endif

  mStatus = NS_OK;
  mIsCanceling = false;
  mCanceledReason.Truncate();

  return firstError;
}

nsresult nsLoadGroup::CancelRequest(nsIRequest* aRequest,
                                    const nsACString& aReason,
                                    nsresult aStatus) {
  MOZ_ASSERT(NS_FAILED(aStatus));
  mStatus = aStatus;
  mIsCanceling = true;
  MOZ_ASSERT(mRequests.Contains(aRequest));
  nsresult result = aRequest->CancelWithReason(aStatus, aReason);
  if (NS_SUCCEEDED(RemoveRequestFromHashtable(aRequest, aStatus))) {
    (void)NotifyRemovalObservers(aRequest, aStatus);
  }
  mIsCanceling = false;
  mStatus = NS_OK;
  return result;
}

NS_IMETHODIMP
nsLoadGroup::Suspend() {
  nsresult rv, firstError;
  uint32_t count = mRequests.Count();

  auto requests = ToTArray<AutoTArray<nsCOMPtr<nsIRequest>, 8>>(mRequests);

  firstError = NS_OK;
  while (count > 0) {
    nsCOMPtr<nsIRequest> request = requests.ElementAt(--count).forget();

    NS_ASSERTION(request, "NULL request found in list.");
    if (!request) continue;

    if (MOZ_LOG_TEST(gLoadGroupLog, LogLevel::Debug)) {
      nsAutoCString nameStr;
      request->GetName(nameStr);
      LOG(("LOADGROUP [%p]: Suspending request %p %s.\n", this, request.get(),
           nameStr.get()));
    }

    rv = request->Suspend();

    if (NS_FAILED(rv) && NS_SUCCEEDED(firstError)) firstError = rv;
  }

  return firstError;
}

NS_IMETHODIMP
nsLoadGroup::Resume() {
  nsresult rv, firstError;
  uint32_t count = mRequests.Count();

  auto requests = ToTArray<AutoTArray<nsCOMPtr<nsIRequest>, 8>>(mRequests);

  firstError = NS_OK;
  while (count > 0) {
    nsCOMPtr<nsIRequest> request = requests.ElementAt(--count).forget();

    NS_ASSERTION(request, "NULL request found in list.");
    if (!request) continue;

    if (MOZ_LOG_TEST(gLoadGroupLog, LogLevel::Debug)) {
      nsAutoCString nameStr;
      request->GetName(nameStr);
      LOG(("LOADGROUP [%p]: Resuming request %p %s.\n", this, request.get(),
           nameStr.get()));
    }

    rv = request->Resume();

    if (NS_FAILED(rv) && NS_SUCCEEDED(firstError)) firstError = rv;
  }

  return firstError;
}

NS_IMETHODIMP
nsLoadGroup::GetLoadFlags(uint32_t* aLoadFlags) {
  *aLoadFlags = mLoadFlags;
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::SetLoadFlags(uint32_t aLoadFlags) {
  mLoadFlags = aLoadFlags;
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::GetTRRMode(nsIRequest::TRRMode* aTRRMode) {
  return GetTRRModeImpl(aTRRMode);
}

NS_IMETHODIMP
nsLoadGroup::SetTRRMode(nsIRequest::TRRMode aTRRMode) {
  return SetTRRModeImpl(aTRRMode);
}

NS_IMETHODIMP
nsLoadGroup::GetLoadGroup(nsILoadGroup** loadGroup) {
  nsCOMPtr<nsILoadGroup> result = mLoadGroup;
  result.forget(loadGroup);
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::SetLoadGroup(nsILoadGroup* loadGroup) {
  mLoadGroup = loadGroup;
  return NS_OK;
}


NS_IMETHODIMP
nsLoadGroup::GetDefaultLoadRequest(nsIRequest** aRequest) {
  nsCOMPtr<nsIRequest> result = mDefaultLoadRequest;
  result.forget(aRequest);
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::SetDefaultLoadRequest(nsIRequest* aRequest) {
  LOG(("nsLoadGroup::SetDefaultLoadRequest this=%p default-request=%p", this,
       aRequest));

  mDefaultLoadRequest = aRequest;
  if (mDefaultLoadRequest) {
    mDefaultLoadRequest->GetLoadFlags(&mLoadFlags);
    mLoadFlags &= nsIRequest::LOAD_INHERIT_MASK;

  }
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::AddRequest(nsIRequest* request, nsISupports* ctxt) {
  if (MOZ_LOG_TEST(gLoadGroupLog, LogLevel::Debug)) {
    nsAutoCString nameStr;
    request->GetName(nameStr);
    LOG(("LOADGROUP [%p]: Adding request %p %s (count=%d).\n", this, request,
         nameStr.get(), mRequests.Count()));
  }

  NS_ASSERTION(!mRequests.Contains(request),
               "Entry added to loadgroup twice, don't do that");

  if (mIsCanceling) {
    LOG(
        ("LOADGROUP [%p]: AddChannel() ABORTED because LoadGroup is"
         " being canceled!!\n",
         this));

    return NS_BINDING_ABORTED;
  }

  nsresult rv;
  nsLoadFlags flags;
  if (mDefaultLoadRequest == request || !mDefaultLoadRequest) {
    rv = MergeDefaultLoadFlags(request, flags);
  } else {
    rv = MergeLoadFlags(request, flags);
  }
  if (NS_FAILED(rv)) return rv;


  mRequests.Insert(request);

  if (mPriority != 0) RescheduleRequest(request, mPriority);

  bool foreground = !(flags & nsIRequest::LOAD_BACKGROUND);
  if (foreground) {
    mForegroundCount += 1;
  }

  if (foreground || mNotifyObserverAboutBackgroundRequests) {
    nsCOMPtr<nsIRequestObserver> observer = do_QueryReferent(mObserver);
    RefPtr<nsLoadGroup> self{this};
    if (observer) {
      LOG(
          ("LOADGROUP [%p]: Firing OnStartRequest for request %p."
           "(foreground count=%d).\n",
           this, request, mForegroundCount));

      rv = observer->OnStartRequest(request);
      if (NS_FAILED(rv)) {
        LOG(("LOADGROUP [%p]: OnStartRequest for request %p FAILED.\n", this,
             request));

        mRequests.Remove(request);

        rv = NS_OK;

        if (foreground) {
          mForegroundCount -= 1;
        }
      }
    }

    if (foreground && mForegroundCount == 1 && mLoadGroup) {
      mLoadGroup->AddRequest(this, nullptr);
    }
  }

  return rv;
}

NS_IMETHODIMP
nsLoadGroup::RemoveRequest(nsIRequest* request, nsISupports* ctxt,
                           nsresult aStatus) {
  nsCOMPtr<nsIRequest> kungFuDeathGrip(request);

  nsresult rv = RemoveRequestFromHashtable(request, aStatus);
  if (NS_FAILED(rv)) {
    return rv;
  }

  return NotifyRemovalObservers(request, aStatus);
}

nsresult nsLoadGroup::RemoveRequestFromHashtable(nsIRequest* request,
                                                 nsresult aStatus) {
  NS_ENSURE_ARG_POINTER(request);

  if (MOZ_LOG_TEST(gLoadGroupLog, LogLevel::Debug)) {
    nsAutoCString nameStr;
    request->GetName(nameStr);
    LOG(("LOADGROUP [%p]: Removing request %p %s status %" PRIx32
         " (count=%d).\n",
         this, request, nameStr.get(), static_cast<uint32_t>(aStatus),
         mRequests.Count() - 1));
  }

  bool found = mRequests.EnsureRemoved(request);

  if (!found) {
    LOG(("LOADGROUP [%p]: Unable to remove request %p. Not in group!\n", this,
         request));

    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

nsresult nsLoadGroup::NotifyRemovalObservers(nsIRequest* request,
                                             nsresult aStatus) {
  NS_ENSURE_ARG_POINTER(request);
  if (mPriority != 0) RescheduleRequest(request, -mPriority);

  nsLoadFlags flags;
  nsresult rv = request->GetLoadFlags(&flags);
  if (NS_FAILED(rv)) return rv;

  bool foreground = !(flags & nsIRequest::LOAD_BACKGROUND);
  if (foreground) {
    NS_ASSERTION(mForegroundCount > 0, "ForegroundCount messed up");
    mForegroundCount -= 1;
  }

  if (foreground || mNotifyObserverAboutBackgroundRequests) {
    nsCOMPtr<nsIRequestObserver> observer = do_QueryReferent(mObserver);
    RefPtr<nsLoadGroup> self{this};
    if (observer) {
      LOG(
          ("LOADGROUP [%p]: Firing OnStopRequest for request %p."
           "(foreground count=%d).\n",
           this, request, mForegroundCount));

      rv = observer->OnStopRequest(request, aStatus);

      if (NS_FAILED(rv)) {
        LOG(("LOADGROUP [%p]: OnStopRequest for request %p FAILED.\n", this,
             request));
      }
    }

    if (foreground && mForegroundCount == 0 && mLoadGroup) {
      mLoadGroup->RemoveRequest(this, nullptr, aStatus);
    }
  }

  return rv;
}

NS_IMETHODIMP
nsLoadGroup::GetRequests(nsISimpleEnumerator** aRequests) {
  nsCOMArray<nsIRequest> requests;
  requests.SetCapacity(mRequests.Count());

  for (nsIRequest* request : mRequests) {
    requests.AppendObject(request);
  }

  return NS_NewArrayEnumerator(aRequests, requests, NS_GET_IID(nsIRequest));
}

NS_IMETHODIMP
nsLoadGroup::GetTotalKeepAliveBytes(uint64_t* aTotalKeepAliveBytes) {
  MOZ_ASSERT(aTotalKeepAliveBytes);
  *aTotalKeepAliveBytes = mPendingKeepaliveRequestSize;
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::SetTotalKeepAliveBytes(uint64_t aTotalKeepAliveBytes) {
  mPendingKeepaliveRequestSize = aTotalKeepAliveBytes;
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::SetGroupObserver(nsIRequestObserver* aObserver) {
  SetGroupObserver(aObserver, false);
  return NS_OK;
}

void nsLoadGroup::SetGroupObserver(nsIRequestObserver* aObserver,
                                   bool aIncludeBackgroundRequests) {
  mObserver = do_GetWeakReference(aObserver);
  mNotifyObserverAboutBackgroundRequests = aIncludeBackgroundRequests;
}

NS_IMETHODIMP
nsLoadGroup::GetGroupObserver(nsIRequestObserver** aResult) {
  nsCOMPtr<nsIRequestObserver> observer = do_QueryReferent(mObserver);
  observer.forget(aResult);
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::GetActiveCount(uint32_t* aResult) {
  *aResult = mForegroundCount;
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::GetNotificationCallbacks(nsIInterfaceRequestor** aCallbacks) {
  NS_ENSURE_ARG_POINTER(aCallbacks);
  nsCOMPtr<nsIInterfaceRequestor> callbacks = mCallbacks;
  callbacks.forget(aCallbacks);
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::SetNotificationCallbacks(nsIInterfaceRequestor* aCallbacks) {
  mCallbacks = aCallbacks;
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::GetRequestContextID(uint64_t* aRCID) {
  if (!mRequestContext) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  *aRCID = mRequestContext->GetID();
  return NS_OK;
}


NS_IMETHODIMP
nsLoadGroup::GetParentLoadGroup(nsILoadGroup** aParentLoadGroup) {
  *aParentLoadGroup = nullptr;
  nsCOMPtr<nsILoadGroup> parent = do_QueryReferent(mParentLoadGroup);
  if (!parent) return NS_OK;
  parent.forget(aParentLoadGroup);
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::SetParentLoadGroup(nsILoadGroup* aParentLoadGroup) {
  mParentLoadGroup = do_GetWeakReference(aParentLoadGroup);
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::GetChildLoadGroup(nsILoadGroup** aChildLoadGroup) {
  *aChildLoadGroup = do_AddRef(this).take();
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::GetRootLoadGroup(nsILoadGroup** aRootLoadGroup) {
  nsCOMPtr<nsILoadGroupChild> ancestor = do_QueryReferent(mParentLoadGroup);
  if (ancestor) return ancestor->GetRootLoadGroup(aRootLoadGroup);

  ancestor = do_QueryInterface(mLoadGroup);
  if (ancestor) return ancestor->GetRootLoadGroup(aRootLoadGroup);

  *aRootLoadGroup = do_AddRef(this).take();
  return NS_OK;
}


NS_IMETHODIMP
nsLoadGroup::GetPriority(int32_t* aValue) {
  *aValue = mPriority;
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::SetPriority(int32_t aValue) {
  return AdjustPriority(aValue - mPriority);
}

NS_IMETHODIMP
nsLoadGroup::AdjustPriority(int32_t aDelta) {
  if (aDelta != 0) {
    mPriority += aDelta;
    for (nsIRequest* request : mRequests) {
      RescheduleRequest(request, aDelta);
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::GetDefaultLoadFlags(uint32_t* aFlags) {
  *aFlags = mDefaultLoadFlags;
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::SetDefaultLoadFlags(uint32_t aFlags) {
  mDefaultLoadFlags = aFlags;
  return NS_OK;
}



nsresult nsLoadGroup::MergeLoadFlags(nsIRequest* aRequest,
                                     nsLoadFlags& outFlags) {
  nsresult rv;
  nsLoadFlags flags, oldFlags;

  rv = aRequest->GetLoadFlags(&flags);
  if (NS_FAILED(rv)) {
    return rv;
  }

  oldFlags = flags;

  flags |= mLoadFlags & kInheritedLoadFlags;

  flags |= mDefaultLoadFlags;

  if (flags != oldFlags) {
    rv = aRequest->SetLoadFlags(flags);
  }

  outFlags = flags;
  return rv;
}

nsresult nsLoadGroup::MergeDefaultLoadFlags(nsIRequest* aRequest,
                                            nsLoadFlags& outFlags) {
  nsresult rv;
  nsLoadFlags flags, oldFlags;

  rv = aRequest->GetLoadFlags(&flags);
  if (NS_FAILED(rv)) {
    return rv;
  }

  oldFlags = flags;
  flags |= mDefaultLoadFlags;

  if (flags != oldFlags) {
    rv = aRequest->SetLoadFlags(flags);
  }
  outFlags = flags;
  return rv;
}

nsresult nsLoadGroup::Init() {
  mRequestContextService = RequestContextService::GetOrCreate();
  if (mRequestContextService) {
    (void)mRequestContextService->NewRequestContext(
        getter_AddRefs(mRequestContext));
  }

  nsCOMPtr<nsIObserverService> os = mozilla::services::GetObserverService();
  NS_ENSURE_STATE(os);

  (void)os->AddObserver(this, "last-pb-context-exited", true);

  return NS_OK;
}

nsresult nsLoadGroup::InitWithRequestContextId(
    const uint64_t& aRequestContextId) {
  mRequestContextService = RequestContextService::GetOrCreate();
  if (mRequestContextService) {
    (void)mRequestContextService->GetRequestContext(
        aRequestContextId, getter_AddRefs(mRequestContext));
  }
  mExternalRequestContext = true;

  nsCOMPtr<nsIObserverService> os = mozilla::services::GetObserverService();
  NS_ENSURE_STATE(os);

  (void)os->AddObserver(this, "last-pb-context-exited", true);

  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::Observe(nsISupports* aSubject, const char* aTopic,
                     const char16_t* aData) {
  MOZ_ASSERT(!strcmp(aTopic, "last-pb-context-exited"));

  OriginAttributes attrs;
  StoragePrincipalHelper::GetRegularPrincipalOriginAttributes(this, attrs);
  if (!attrs.IsPrivateBrowsing()) {
    return NS_OK;
  }

  mBrowsingContextDiscarded = true;
  return NS_OK;
}

NS_IMETHODIMP
nsLoadGroup::GetIsBrowsingContextDiscarded(bool* aIsBrowsingContextDiscarded) {
  *aIsBrowsingContextDiscarded = mBrowsingContextDiscarded;
  return NS_OK;
}

}  
}  

#undef LOG
