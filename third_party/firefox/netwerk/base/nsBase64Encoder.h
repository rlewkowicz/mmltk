/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NSBASE64ENCODER_H_
#define NSBASE64ENCODER_H_

#include "nsIOutputStream.h"
#include "nsString.h"

class nsBase64Encoder final : public nsIOutputStream {
 public:
  nsBase64Encoder() = default;

  NS_DECL_ISUPPORTS
  NS_DECL_NSIOUTPUTSTREAM

  nsresult Finish(nsACString& _result);

 private:
  ~nsBase64Encoder() = default;

  nsCString mData;
};

#endif
