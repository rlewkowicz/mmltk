/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsBaseParentChannel.h"

NS_IMPL_ISUPPORTS(nsBaseParentChannel, nsIParentChannel, nsIStreamListener,
                  nsIRequestObserver)

NS_IMETHODIMP
nsBaseParentChannel::SetParentListener(
    mozilla::net::ParentChannelListener* aListener) {
  return NS_OK;
}

NS_IMETHODIMP
nsBaseParentChannel::Delete() {
  return NS_OK;
}

NS_IMETHODIMP
nsBaseParentChannel::GetRemoteType(nsACString& aRemoteType) {
  aRemoteType = mRemoteType;
  return NS_OK;
}

NS_IMETHODIMP
nsBaseParentChannel::OnStartRequest(nsIRequest* aRequest) {
  return NS_BINDING_ABORTED;
}

NS_IMETHODIMP
nsBaseParentChannel::OnStopRequest(nsIRequest* aRequest, nsresult aStatusCode) {
  MOZ_ASSERT(NS_FAILED(aStatusCode));
  return NS_OK;
}

NS_IMETHODIMP
nsBaseParentChannel::OnDataAvailable(nsIRequest* aRequest,
                                     nsIInputStream* aInputStream,
                                     uint64_t aOffset, uint32_t aCount) {
  MOZ_CRASH("Should never be called unless overridden");
}
