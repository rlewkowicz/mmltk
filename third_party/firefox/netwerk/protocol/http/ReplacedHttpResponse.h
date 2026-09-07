/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NETWERK_PROTOCOL_HTTP_REPLACEDHTTPRESPONSE_H_
#define NETWERK_PROTOCOL_HTTP_REPLACEDHTTPRESPONSE_H_

#include "nsString.h"
#include "nsHttpHeaderArray.h"
#include "nsIReplacedHttpResponse.h"
#include "mozilla/Atomics.h"

namespace mozilla::net {

class ReplacedHttpResponse : nsIReplacedHttpResponse {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIREPLACEDHTTPRESPONSE

 private:
  virtual ~ReplacedHttpResponse() = default;
  uint16_t mResponseStatus = 0;
  nsCString mResponseStatusText;
  nsCString mResponseBody;
  nsHttpHeaderArray mResponseHeaders;
  Atomic<uint32_t> mInVisitHeaders{0};
};

}  

#endif  // NETWERK_PROTOCOL_HTTP_REPLACEDHTTPRESPONSE_H_
