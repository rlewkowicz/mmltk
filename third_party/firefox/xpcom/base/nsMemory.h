/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsMemory_h_
#define nsMemory_h_

#include "nsError.h"



namespace nsMemory {

nsresult HeapMinimize(bool aImmediate);

bool IsLowMemoryPlatform();
}  

#endif  // nsMemory_h_
