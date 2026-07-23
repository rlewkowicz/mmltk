/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SavedStacksInl_h
#define vm_SavedStacksInl_h

#include "vm/SavedStacks.h"

inline void js::AssertObjectIsSavedFrameOrWrapper(JSContext* cx,
                                                  HandleObject stack) {
  if (stack) {
    MOZ_RELEASE_ASSERT(stack->canUnwrapAs<SavedFrame>());
  }
}

#endif  // vm_SavedStacksInl_h
