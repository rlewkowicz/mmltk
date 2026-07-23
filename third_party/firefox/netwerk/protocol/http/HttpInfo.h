/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http:mozilla.org/MPL/2.0/. */

#ifndef nsHttpInfo_
#define nsHttpInfo_

#include "nsTArrayForwardDeclare.h"

namespace mozilla {
namespace net {

struct HttpRetParams;

class HttpInfo {
 public:
  static void GetHttpConnectionData(nsTArray<HttpRetParams>*);
  static void GetHttp3ConnectionStatsData(
      nsTArray<Http3ConnectionStatsParams>*);
};

}  
}  

#endif  // nsHttpInfo_
