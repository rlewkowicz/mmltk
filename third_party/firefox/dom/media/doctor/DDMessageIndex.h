/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DDMessageIndex_h_
#define DDMessageIndex_h_

#include "RollingNumber.h"

namespace mozilla {

using DDMessageIndex = RollingNumber<uint32_t>;

#define PRImi PRIu32

}  

#endif  // DDMessageIndex_h_
