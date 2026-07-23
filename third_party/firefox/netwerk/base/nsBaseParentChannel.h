/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsBaseParentChannel_h
#define nsBaseParentChannel_h

#include "nsIParentChannel.h"

class nsBaseParentChannel : public nsIParentChannel {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIPARENTCHANNEL
  NS_DECL_NSIREQUESTOBSERVER
  NS_DECL_NSISTREAMLISTENER

  explicit nsBaseParentChannel(const nsACString& aRemoteType)
      : mRemoteType(aRemoteType) {}

 protected:
  virtual ~nsBaseParentChannel() = default;

  nsCString mRemoteType;
};

#endif  // nsBaseParentChannel_h
