/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */


#ifndef mozilla_RandomNum_h_
#define mozilla_RandomNum_h_

#include "mozilla/Maybe.h"
#include "mozilla/Types.h"

namespace mozilla {

[[nodiscard]] MFBT_API bool GenerateRandomBytesFromOS(void* aBuffer,
                                                      size_t aLength);

MFBT_API Maybe<uint64_t> RandomUint64();

MFBT_API uint64_t RandomUint64OrDie();

}  

#endif  // mozilla_RandomNum_h_
