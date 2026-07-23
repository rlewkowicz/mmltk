/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js-confdefs.h"
#include "mozilla/Assertions.h"
#include "mozilla/Types.h"
#include "mozilla/mozalloc_oom.h"

extern "C" void RustMozCrash(const char* aFilename, int aLine,
                             const char* aReason) {
  MOZ_Crash(aFilename, aLine, aReason);
}

extern "C" void RustHandleOOM(size_t size) { mozalloc_handle_oom(size); }
