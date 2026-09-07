/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_Array_h
#define js_Array_h

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/TypeDecls.h"

namespace JS {

class HandleValueArray;

extern JS_PUBLIC_API JSObject* NewArrayObject(JSContext* cx,
                                              const HandleValueArray& contents);

extern JS_PUBLIC_API JSObject* NewArrayObject(JSContext* cx, size_t length);

extern JS_PUBLIC_API bool IsArrayObject(JSContext* cx, Handle<Value> value,
                                        bool* isArray);

extern JS_PUBLIC_API bool IsArrayObject(JSContext* cx, Handle<JSObject*> obj,
                                        bool* isArray);

extern JS_PUBLIC_API bool GetArrayLength(JSContext* cx, Handle<JSObject*> obj,
                                         uint32_t* lengthp);

extern JS_PUBLIC_API bool SetArrayLength(JSContext* cx, Handle<JSObject*> obj,
                                         uint32_t length);

enum class IsArrayAnswer { Array, NotArray, RevokedProxy };

extern JS_PUBLIC_API bool IsArray(JSContext* cx, Handle<JSObject*> obj,
                                  bool* isArray);

extern JS_PUBLIC_API bool IsArray(JSContext* cx, Handle<JSObject*> obj,
                                  IsArrayAnswer* answer);

}  

#endif  // js_Array_h
