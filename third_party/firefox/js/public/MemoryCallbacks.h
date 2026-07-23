/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_MemoryCallbacks_h
#define js_MemoryCallbacks_h

#include "jstypes.h"

struct JS_PUBLIC_API JSContext;

namespace JS {


using LargeAllocationFailureCallback = void (*)();

extern JS_PUBLIC_API void SetProcessLargeAllocationFailureCallback(
    LargeAllocationFailureCallback afc);


using OutOfMemoryCallback = void (*)(JSContext*, void*);

extern JS_PUBLIC_API void SetOutOfMemoryCallback(JSContext* cx,
                                                 OutOfMemoryCallback cb,
                                                 void* data);

}  

#endif  // js_MemoryCallbacks_h
