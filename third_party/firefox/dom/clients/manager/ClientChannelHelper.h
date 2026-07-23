/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _mozilla_dom_ClientChannelHelper_h
#define _mozilla_dom_ClientChannelHelper_h

#include "mozilla/Maybe.h"
#include "nsError.h"

class nsIChannel;
class nsISerialEventTarget;

namespace mozilla::dom {

class ClientInfo;

nsresult AddClientChannelHelper(nsIChannel* aChannel,
                                Maybe<ClientInfo>&& aReservedClientInfo,
                                Maybe<ClientInfo>&& aInitialClientInfo,
                                nsISerialEventTarget* aEventTarget);

nsresult AddClientChannelHelperInChild(nsIChannel* aChannel,
                                       nsISerialEventTarget* aEventTarget);

nsresult AddClientChannelHelperInParent(nsIChannel* aChannel,
                                        Maybe<ClientInfo>&& aInitialClientInfo);

void CreateReservedSourceIfNeeded(nsIChannel* aChannel,
                                  nsISerialEventTarget* aEventTarget);

}  

#endif  // _mozilla_dom_ClientChannelHelper_h
