/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsIMemoryReporter.h"

extern "C" size_t cert_storage_malloc_size_of(void* aPtr) {
  MOZ_REPORT(aPtr);
  return moz_malloc_size_of(aPtr);
}
