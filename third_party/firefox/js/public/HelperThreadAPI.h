/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_HelperThreadAPI_h
#define js_HelperThreadAPI_h

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

namespace JS {

class HelperThreadTask;

using HelperThreadTaskCallback = void (*)(HelperThreadTask* task);
extern JS_PUBLIC_API void SetHelperThreadTaskCallback(
    HelperThreadTaskCallback callback, size_t threadCount, size_t stackSize);

extern JS_PUBLIC_API void RunHelperThreadTask(HelperThreadTask* task);

extern JS_PUBLIC_API const char* GetHelperThreadTaskName(
    HelperThreadTask* task);

}  

#endif  // js_HelperThreadAPI_h
