/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ParentChannelWrapper.h"
#include "mozilla/net/HttpBaseChannel.h"
#include "mozilla/net/RedirectChannelRegistrar.h"
#include "nsIViewSourceChannel.h"
#include "nsNetUtil.h"
#include "nsQueryObject.h"
#include "mozilla/dom/RemoteType.h"

namespace mozilla {
namespace net {

NS_IMPL_ISUPPORTS(ParentChannelWrapper, nsIParentChannel, nsIStreamListener,
                  nsIRequestObserver);

void ParentChannelWrapper::Register(uint64_t aRegistrarId,
                                    uint64_t aContentParentId) {
  nsCOMPtr<nsIRedirectChannelRegistrar> registrar =
      RedirectChannelRegistrar::GetOrCreate();
  if (!registrar) {
    return;
  }
  nsCOMPtr<nsIChannel> dummy;
  MOZ_ALWAYS_SUCCEEDS(NS_LinkRedirectChannels(aRegistrarId, aContentParentId,
                                              this, getter_AddRefs(dummy)));

#ifdef DEBUG
  if (nsCOMPtr<nsIViewSourceChannel> viewSource = do_QueryInterface(mChannel)) {
    MOZ_ASSERT(dummy == viewSource->GetInnerChannel());
  } else {
    MOZ_ASSERT(dummy == mChannel);
  }
#endif
}


NS_IMETHODIMP
ParentChannelWrapper::SetParentListener(
    mozilla::net::ParentChannelListener* listener) {
  return NS_OK;
}

NS_IMETHODIMP
ParentChannelWrapper::Delete() { return NS_OK; }

NS_IMETHODIMP
ParentChannelWrapper::GetRemoteType(nsACString& aRemoteType) {
  aRemoteType = NOT_REMOTE_TYPE;
  return NS_OK;
}

}  
}  

#undef LOG
