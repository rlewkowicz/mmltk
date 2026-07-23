/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsHTTPSOnlyStreamListener.h"

#include "NSSErrorsService.h"
#include "mozilla/dom/WindowGlobalParent.h"
#include "mozpkix/pkixnss.h"
#include "nsCOMPtr.h"
#include "nsHTTPSOnlyUtils.h"
#include "nsIChannel.h"
#include "nsIRequest.h"
#include "nsITransportSecurityInfo.h"
#include "nsIURI.h"
#include "nsIWebProgressListener.h"
#include "nsPrintfCString.h"
#include "secerr.h"
#include "sslerr.h"

NS_IMPL_ISUPPORTS(nsHTTPSOnlyStreamListener, nsIStreamListener,
                  nsIRequestObserver)

nsHTTPSOnlyStreamListener::nsHTTPSOnlyStreamListener(
    nsIStreamListener* aListener, nsILoadInfo* aLoadInfo)
    : mListener(aListener) {
  RefPtr<mozilla::dom::WindowGlobalParent> wgp =
      mozilla::dom::WindowGlobalParent::GetByInnerWindowId(
          aLoadInfo->GetInnerWindowID());
  if (wgp) {
    wgp->TopWindowContext()->AddSecurityState(
        nsIWebProgressListener::STATE_HTTPS_ONLY_MODE_UPGRADED);
  }
}

NS_IMETHODIMP
nsHTTPSOnlyStreamListener::OnDataAvailable(nsIRequest* aRequest,
                                           nsIInputStream* aInputStream,
                                           uint64_t aOffset, uint32_t aCount) {
  nsCOMPtr<nsIStreamListener> listener = mListener;
  return listener->OnDataAvailable(aRequest, aInputStream, aOffset, aCount);
}

NS_IMETHODIMP
nsHTTPSOnlyStreamListener::OnStartRequest(nsIRequest* request) {
  nsCOMPtr<nsIStreamListener> listener = mListener;
  return listener->OnStartRequest(request);
}

NS_IMETHODIMP
nsHTTPSOnlyStreamListener::OnStopRequest(nsIRequest* request,
                                         nsresult aStatus) {
  nsCOMPtr<nsIChannel> channel = do_QueryInterface(request);

  if (nsHTTPSOnlyUtils::CouldBeHttpsOnlyError(channel, aStatus)) {
    LogUpgradeFailure(request, aStatus);

    if (NS_FAILED(aStatus)) {
      nsCOMPtr<nsILoadInfo> loadInfo = channel->LoadInfo();
      RefPtr<mozilla::dom::WindowGlobalParent> wgp =
          mozilla::dom::WindowGlobalParent::GetByInnerWindowId(
              loadInfo->GetInnerWindowID());

      if (wgp) {
        wgp->TopWindowContext()->AddSecurityState(
            nsIWebProgressListener::STATE_HTTPS_ONLY_MODE_UPGRADE_FAILED);
      }
    }
  }

  nsCOMPtr<nsIStreamListener> listener = mListener;
  return listener->OnStopRequest(request, aStatus);
}

void nsHTTPSOnlyStreamListener::LogUpgradeFailure(nsIRequest* request,
                                                  nsresult aStatus) {
  if (NS_SUCCEEDED(aStatus)) {
    return;
  }
  nsresult rv;
  nsCOMPtr<nsIChannel> channel = do_QueryInterface(request, &rv);
  if (NS_FAILED(rv)) {
    return;
  }

  nsCOMPtr<nsIURI> uri;
  rv = channel->GetURI(getter_AddRefs(uri));
  if (NS_FAILED(rv)) {
    return;
  }
  AutoTArray<nsString, 2> params = {
      NS_ConvertUTF8toUTF16(uri->GetSpecOrDefault()),
      NS_ConvertUTF8toUTF16(nsPrintfCString("M%u-C%u",
                                            NS_ERROR_GET_MODULE(aStatus),
                                            NS_ERROR_GET_CODE(aStatus)))};

  nsCOMPtr<nsILoadInfo> loadInfo = channel->LoadInfo();
  nsHTTPSOnlyUtils::LogLocalizedString("HTTPSOnlyFailedRequest", params,
                                       nsIScriptError::errorFlag, loadInfo,
                                       uri);
}
