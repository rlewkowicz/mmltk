/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaElementEventRunners.h"

#include <stdint.h>

#include "mozilla/dom/HTMLMediaElement.h"

extern mozilla::LazyLogModule gMediaElementEventsLog;
#define LOG_EVENT(type, msg) \
  MOZ_LOG_FMT(gMediaElementEventsLog, type, MOZ_LOG_EXPAND_ARGS msg)

namespace mozilla::dom {

nsMediaEventRunner::nsMediaEventRunner(const char* aName,
                                       HTMLMediaElement* aElement,
                                       const nsAString& aEventName)
    : mElement(aElement),
      mName(aName),
      mEventName(aEventName),
      mLoadID(mElement->GetCurrentLoadID()) {}

bool nsMediaEventRunner::IsCancelled() const {
  return !mElement || mElement->GetCurrentLoadID() != mLoadID;
}

nsresult nsMediaEventRunner::FireEvent(const nsAString& aName) {
  nsresult rv = NS_OK;
  if (mElement) {
    rv = RefPtr{mElement}->FireEvent(aName);
  }
  return rv;
}

NS_IMPL_CYCLE_COLLECTION(nsMediaEventRunner, mElement)
NS_IMPL_CYCLE_COLLECTING_ADDREF(nsMediaEventRunner)
NS_IMPL_CYCLE_COLLECTING_RELEASE(nsMediaEventRunner)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(nsMediaEventRunner)
  NS_INTERFACE_MAP_ENTRY(nsINamed)
  NS_INTERFACE_MAP_ENTRY(nsIRunnable)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIRunnable)
NS_INTERFACE_MAP_END

NS_IMETHODIMP nsAsyncEventRunner::Run() {
  return IsCancelled() ? NS_OK : FireEvent(mEventName);
}

nsResolveOrRejectPendingPlayPromisesRunner::
    nsResolveOrRejectPendingPlayPromisesRunner(
        HTMLMediaElement* aElement, nsTArray<RefPtr<PlayPromise>>&& aPromises,
        nsresult aError)
    : nsMediaEventRunner("nsResolveOrRejectPendingPlayPromisesRunner",
                         aElement),
      mPromises(std::move(aPromises)),
      mError(aError) {
  mElement->mPendingPlayPromisesRunners.AppendElement(this);
}

void nsResolveOrRejectPendingPlayPromisesRunner::ResolveOrReject() {
  if (NS_SUCCEEDED(mError)) {
    PlayPromise::ResolvePromisesWithUndefined(mPromises);
  } else {
    PlayPromise::RejectPromises(mPromises, mError);
  }
}

NS_IMETHODIMP nsResolveOrRejectPendingPlayPromisesRunner::Run() {
  if (!IsCancelled()) {
    ResolveOrReject();
  }

  mElement->mPendingPlayPromisesRunners.RemoveElement(this);
  return NS_OK;
}

NS_IMETHODIMP nsNotifyAboutPlayingRunner::Run() {
  if (!IsCancelled()) {
    FireEvent(u"playing"_ns);
  }
  return nsResolveOrRejectPendingPlayPromisesRunner::Run();
}

NS_IMPL_CYCLE_COLLECTION_INHERITED(nsResolveOrRejectPendingPlayPromisesRunner,
                                   nsMediaEventRunner, mPromises)
NS_IMPL_ADDREF_INHERITED(nsResolveOrRejectPendingPlayPromisesRunner,
                         nsMediaEventRunner)
NS_IMPL_RELEASE_INHERITED(nsResolveOrRejectPendingPlayPromisesRunner,
                          nsMediaEventRunner)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(
    nsResolveOrRejectPendingPlayPromisesRunner)
NS_INTERFACE_MAP_END_INHERITING(nsMediaEventRunner)

NS_IMETHODIMP nsSourceErrorEventRunner::Run() {
  if (IsCancelled()) {
    return NS_OK;
  }
  LOG_EVENT(LogLevel::Debug, ("{} Dispatching simple event source error",
                              fmt::ptr(mElement.get())));
  return nsContentUtils::DispatchTrustedEvent(mElement->OwnerDoc(), mSource,
                                              u"error"_ns, CanBubble::eNo,
                                              Cancelable::eNo);
}

NS_IMPL_CYCLE_COLLECTION_INHERITED(nsSourceErrorEventRunner, nsMediaEventRunner,
                                   mSource)
NS_IMPL_ADDREF_INHERITED(nsSourceErrorEventRunner, nsMediaEventRunner)
NS_IMPL_RELEASE_INHERITED(nsSourceErrorEventRunner, nsMediaEventRunner)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(nsSourceErrorEventRunner)
NS_INTERFACE_MAP_END_INHERITING(nsMediaEventRunner)

NS_IMETHODIMP nsTimeupdateRunner::Run() {
  if (IsCancelled() || !ShouldDispatchTimeupdate()) {
    return NS_OK;
  }
  nsresult rv = FireEvent(mEventName);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    LOG_EVENT(LogLevel::Debug,
              ("{} Failed to dispatch 'timeupdate'", fmt::ptr(mElement.get())));
  } else {
    mElement->UpdateLastTimeupdateDispatchTime();
  }
  return rv;
}

bool nsTimeupdateRunner::ShouldDispatchTimeupdate() const {
  if (mIsMandatory) {
    return true;
  }

  const TimeStamp& lastTime = mElement->LastTimeupdateDispatchTime();
  return lastTime.IsNull() || TimeStamp::Now() - lastTime >
                                  TimeDuration::FromMilliseconds(TIMEUPDATE_MS);
}

#undef LOG_EVENT
}  
