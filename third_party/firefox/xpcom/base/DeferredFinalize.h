/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_DeferredFinalize_h
#define mozilla_DeferredFinalize_h

#include <cstdint>
#include "mozilla/Attributes.h"

class nsISupports;

namespace mozilla {

typedef void* (*DeferredFinalizeAppendFunction)(void* aPointers, void* aThing);

typedef bool (*DeferredFinalizeFunction)(uint32_t aSlice, void* aData);

void DeferredFinalize(DeferredFinalizeAppendFunction aAppendFunc,
                      DeferredFinalizeFunction aFunc, void* aThing);

MOZ_NEVER_INLINE void DeferredFinalize(nsISupports* aSupports);

}  

#endif  // mozilla_DeferredFinalize_h
