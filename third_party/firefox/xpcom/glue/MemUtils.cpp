/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/MemUtils.h"

#  include <sys/mman.h>


bool mozilla::CanPrefetchMemory() {
#if 0 || defined(XP_UNIX)
  return true;
#else
  return false;
#endif
}

void mozilla::PrefetchMemory(uint8_t* aStart, size_t aNumBytes) {
  if (aNumBytes == 0) {
    return;
  }

#if defined(XP_UNIX)
  madvise(aStart, aNumBytes, MADV_WILLNEED);
#endif
}
