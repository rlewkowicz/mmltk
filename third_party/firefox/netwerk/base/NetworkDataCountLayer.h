/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NetworkDataCountLayer_h_
#define NetworkDataCountLayer_h_

#include "prerror.h"
#include "prio.h"
#include "ErrorList.h"

namespace mozilla {
namespace net {

nsresult AttachNetworkDataCountLayer(PRFileDesc* fd);

void NetworkDataCountSent(PRFileDesc* fd, uint64_t& sentBytes);

void NetworkDataCountReceived(PRFileDesc* fd, uint64_t& receivedBytes);

}  
}  

#endif  // NetworkDataCountLayer_h_
