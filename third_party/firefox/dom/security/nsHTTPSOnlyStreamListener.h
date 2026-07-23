/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsHTTPSOnlyStreamListener_h_
#define nsHTTPSOnlyStreamListener_h_

#include "nsCOMPtr.h"
#include "nsIStreamListener.h"

class nsILoadInfo;

class nsHTTPSOnlyStreamListener : public nsIStreamListener {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIREQUESTOBSERVER
  NS_DECL_NSISTREAMLISTENER

  explicit nsHTTPSOnlyStreamListener(nsIStreamListener* aListener,
                                     nsILoadInfo* aLoadInfo);

 private:
  virtual ~nsHTTPSOnlyStreamListener() = default;

  void LogUpgradeFailure(nsIRequest* request, nsresult aStatus);

  nsCOMPtr<nsIStreamListener> mListener;
};

#endif /* nsHTTPSOnlyStreamListener_h_ */
