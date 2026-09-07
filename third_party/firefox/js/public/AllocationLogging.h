/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_AllocationLogging_h
#define js_AllocationLogging_h

#include <stdint.h>  // uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

namespace JS {

using LogCtorDtor = void (*)(void*, const char*, uint32_t);

extern JS_PUBLIC_API void SetLogCtorDtorFunctions(LogCtorDtor ctor,
                                                  LogCtorDtor dtor);

extern JS_PUBLIC_API void LogCtor(void* self, const char* type, uint32_t sz);

extern JS_PUBLIC_API void LogDtor(void* self, const char* type, uint32_t sz);

#define JS_COUNT_CTOR(Class) \
  (::JS::LogCtor(static_cast<void*>(this), #Class, sizeof(Class)))

#define JS_COUNT_DTOR(Class) \
  (::JS::LogDtor(static_cast<void*>(this), #Class, sizeof(Class)))

}  

#endif  // js_AllocationLogging_h
