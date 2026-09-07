/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/mozalloc_abort.h"

#include <stdio.h>
#include <string.h>

#include "mozilla/Assertions.h"
#include "mozilla/Sprintf.h"

void mozalloc_abort(const char* const msg) {
  fputs(msg, stderr);
  fputs("\n", stderr);


  MOZ_CRASH_UNSAFE(msg);
}


#if defined(XP_UNIX) && !defined(MOZ_ASAN) && !defined(MOZ_TSAN) &&    \
    !defined(MOZ_UBSAN) && !0 && !0 && \
    !0
extern "C" void abort(void) {
  const char* const msg = "Redirecting call to abort() to mozalloc_abort\n";

  mozalloc_abort(msg);

  MOZ_ASSUME_UNREACHABLE_MARKER();
}
#endif
