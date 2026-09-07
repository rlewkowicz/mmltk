/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DDMediaLog_h_
#define DDMediaLog_h_

#include "DDLogMessage.h"
#include "nsTArray.h"

namespace mozilla {

namespace dom {
class HTMLMediaElement;
}  

class DDLifetimes;

struct DDMediaLog {
  const dom::HTMLMediaElement* mMediaElement;

  int32_t mLifetimeCount = 0;

  using LogMessages = nsTArray<DDLogMessage>;
  LogMessages mMessages;

  size_t SizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const;
};

}  

#endif  // DDMediaLog_h_
