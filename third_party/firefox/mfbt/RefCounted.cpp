/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/RefCounted.h"

namespace mozilla::detail {

#ifdef MOZ_REFCOUNTED_LEAK_CHECKING
MFBT_DATA LogAddRefFunc gLogAddRefFunc = nullptr;
MFBT_DATA LogReleaseFunc gLogReleaseFunc = nullptr;
MFBT_DATA size_t gNumStaticCtors = 0;
MFBT_DATA const char* gLastStaticCtorTypeName = nullptr;

MFBT_API void RefCountLogger::SetLeakCheckingFunctions(
    LogAddRefFunc aLogAddRefFunc, LogReleaseFunc aLogReleaseFunc) {
  if (gNumStaticCtors > 0) {
    fprintf(stderr,
            "RefCounted objects addrefed/released (static ctor?) total: %zu, "
            "last type: %s\n",
            gNumStaticCtors, gLastStaticCtorTypeName);
    gNumStaticCtors = 0;
    gLastStaticCtorTypeName = nullptr;
  }
  gLogAddRefFunc = aLogAddRefFunc;
  gLogReleaseFunc = aLogReleaseFunc;
}
#endif  // MOZ_REFCOUNTED_LEAK_CHECKING

}  
