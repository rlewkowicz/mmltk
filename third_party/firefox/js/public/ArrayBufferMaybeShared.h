/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_ArrayBufferMaybeShared_h
#define js_ArrayBufferMaybeShared_h

#include <stdint.h>  // uint8_t, uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace JS {

class JS_PUBLIC_API AutoRequireNoGC;


extern JS_PUBLIC_API bool IsArrayBufferObjectMaybeShared(JSObject* obj);


extern JS_PUBLIC_API JSObject* UnwrapArrayBufferMaybeShared(JSObject* obj);

extern JS_PUBLIC_API void GetArrayBufferMaybeSharedLengthAndData(
    JSObject* obj, size_t* length, bool* isSharedMemory, uint8_t** data);

extern JS_PUBLIC_API uint8_t* GetArrayBufferMaybeSharedData(
    JSObject* obj, bool* isSharedMemory, const AutoRequireNoGC&);

extern JS_PUBLIC_API bool IsLargeArrayBufferMaybeShared(JSObject* obj);

extern JS_PUBLIC_API bool IsResizableArrayBufferMaybeShared(JSObject* obj);

extern JS_PUBLIC_API bool IsImmutableArrayBufferMaybeShared(JSObject* obj);

}  

#endif /* js_ArrayBufferMaybeShared_h */
