/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_SharedArrayBuffer_h
#define js_SharedArrayBuffer_h

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t

#include "jstypes.h"  // JS_PUBLIC_API

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace JS {
class JS_PUBLIC_API AutoRequireNoGC;


extern JS_PUBLIC_API JSObject* NewSharedArrayBuffer(JSContext* cx,
                                                    size_t nbytes);


extern JS_PUBLIC_API bool IsSharedArrayBufferObject(JSObject* obj);


extern JS_PUBLIC_API JSObject* UnwrapSharedArrayBuffer(JSObject* obj);

extern JS_PUBLIC_API size_t GetSharedArrayBufferByteLength(JSObject* obj);

extern JS_PUBLIC_API uint8_t* GetSharedArrayBufferData(JSObject* obj,
                                                       bool* isSharedMemory,
                                                       const AutoRequireNoGC&);

extern JS_PUBLIC_API void GetSharedArrayBufferLengthAndData(
    JSObject* obj, size_t* length, bool* isSharedMemory, uint8_t** data);

extern JS_PUBLIC_API bool ContainsSharedArrayBuffer(JSContext* cx);

extern JS_PUBLIC_API bool IsArrayBufferViewShared(JSObject* obj);

}  

#endif /* js_SharedArrayBuffer_h */
