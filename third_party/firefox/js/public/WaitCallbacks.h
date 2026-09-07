/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_WaitCallbacks_h
#define js_WaitCallbacks_h

#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"

struct JS_PUBLIC_API JSRuntime;

namespace JS {


static constexpr size_t WAIT_CALLBACK_CLIENT_MAXMEM = 32;

using BeforeWaitCallback = void* (*)(uint8_t* memory);
using AfterWaitCallback = void (*)(void* cookie);

extern JS_PUBLIC_API void SetWaitCallback(JSRuntime* rt,
                                          BeforeWaitCallback beforeWait,
                                          AfterWaitCallback afterWait,
                                          size_t requiredMemory);

}  

#endif  // js_WaitCallbacks_h
