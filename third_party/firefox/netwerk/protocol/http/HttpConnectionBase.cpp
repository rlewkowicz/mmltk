/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HttpLog.h"

#undef LOG
#define LOG(args) LOG5(args)
#undef LOG_ENABLED
#define LOG_ENABLED() LOG5_ENABLED()

#define TLS_EARLY_DATA_NOT_AVAILABLE 0
#define TLS_EARLY_DATA_AVAILABLE_BUT_NOT_USED 1
#define TLS_EARLY_DATA_AVAILABLE_AND_USED 2

#include "HttpConnectionBase.h"
#include "nsHttpHandler.h"
#include "nsIClassOfService.h"
#include "nsIOService.h"
#include "nsISocketTransport.h"
#include "ConnectionEntry.h"

namespace mozilla {
namespace net {


HttpConnectionBase::HttpConnectionBase() {
  LOG(("Creating HttpConnectionBase @%p\n", this));
}

void HttpConnectionBase::BootstrapTimings(TimingStruct times) {
  mBootstrappedTimingsSet = true;
  mBootstrappedTimings = times;
}

void HttpConnectionBase::SetDnsBootstrapTimings(TimeStamp domainLookupStart,
                                                TimeStamp domainLookupEnd) {
  mBootstrappedTimingsSet = true;
  mBootstrappedTimings.domainLookupStart = domainLookupStart;
  mBootstrappedTimings.domainLookupEnd = domainLookupEnd;
}

void HttpConnectionBase::SetConnectBootstrapTimings(
    TimeStamp connectStart, TimeStamp tcpConnectEnd,
    TimeStamp secureConnectionStart, TimeStamp connectEnd) {
  mBootstrappedTimingsSet = true;
  mBootstrappedTimings.connectStart = connectStart;
  if (!tcpConnectEnd.IsNull()) {
    mBootstrappedTimings.tcpConnectEnd = tcpConnectEnd;
  }
  if (!secureConnectionStart.IsNull()) {
    mBootstrappedTimings.secureConnectionStart = secureConnectionStart;
  }
  if (!connectEnd.IsNull()) {
    mBootstrappedTimings.connectEnd = connectEnd;
  }
}

void HttpConnectionBase::SetSecurityCallbacks(
    nsIInterfaceRequestor* aCallbacks) {
  MutexAutoLock lock(mCallbacksLock);
  mCallbacks = new nsMainThreadPtrHolder<nsIInterfaceRequestor>(
      "nsHttpConnection::mCallbacks", aCallbacks, false);
}

void HttpConnectionBase::SetTrafficCategory(HttpTrafficCategory aCategory) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");
  if (aCategory == HttpTrafficCategory::eInvalid ||
      mTrafficCategory.Contains(aCategory)) {
    return;
  }
  (void)mTrafficCategory.AppendElement(aCategory);
}

void HttpConnectionBase::ChangeConnectionState(ConnectionState aState) {
  LOG(("HttpConnectionBase::ChangeConnectionState this=%p (%d->%d)", this,
       static_cast<uint32_t>(mConnectionState), static_cast<uint32_t>(aState)));

  if (aState <= mConnectionState) {
    return;
  }

  mConnectionState = aState;
}

void HttpConnectionBase::ChangeState(HttpConnectionState newState) {
  LOG(("HttpConnectionBase::ChangeState %d -> %d [this=%p]", mState, newState,
       this));
  mState = newState;
}

nsresult HttpConnectionBase::CheckTunnelIsNeeded(
    nsAHttpTransaction* aTransaction) {
  switch (mState) {
    case HttpConnectionState::UNINITIALIZED: {
      if (!aTransaction->ConnectionInfo()->UsingConnect()) {
        ChangeState(HttpConnectionState::REQUEST);
        return NS_OK;
      }
      ChangeState(HttpConnectionState::SETTING_UP_TUNNEL);
    }
      [[fallthrough]];
    case HttpConnectionState::SETTING_UP_TUNNEL: {
      nsresult rv = SetupProxyConnectStream();
      if (NS_FAILED(rv)) {
        ChangeState(HttpConnectionState::UNINITIALIZED);
      }
      return rv;
    }
    case HttpConnectionState::REQUEST:
      return NS_OK;
  }
  return NS_OK;
}

void HttpConnectionBase::SetOwner(ConnectionEntry* aEntry) {
  mOwnerEntry = aEntry;
}

ConnectionEntry* HttpConnectionBase::OwnerEntry() const {
  return mOwnerEntry.get();
}

}  
}  
