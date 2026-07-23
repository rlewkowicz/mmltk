/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_Try_h
#define mozilla_Try_h

#include "mozilla/Likely.h"
#include "mozilla/Result.h"  // IWYU pragma: keep(used by macro MOZ_TRY)

#define MOZ_TRY(expr)                                     \
  __extension__({                                         \
    auto mozTryVarTempResult = ::mozilla::ToResult(expr); \
    if (MOZ_UNLIKELY(mozTryVarTempResult.isErr())) {      \
      return mozTryVarTempResult.propagateErr();          \
    }                                                     \
    mozTryVarTempResult.unwrap();                         \
  })

#endif  // mozilla_Try_h
