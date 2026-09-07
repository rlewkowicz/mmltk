/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_public_ObjectWithStashedPointer_h
#define js_public_ObjectWithStashedPointer_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/RootingAPI.h"  // JS::Handle

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace JS {

namespace detail {
using UntypedFreeFunction = void (*)(void*);

extern JS_PUBLIC_API JSObject* NewObjectWithUntypedStashedPointer(
    JSContext* cx, void* ptr, UntypedFreeFunction freeFunc);

extern JS_PUBLIC_API void* ObjectGetUntypedStashedPointer(JSContext* cx,
                                                          JSObject* obj);
}  

template <typename T, typename F>
inline JSObject* NewObjectWithStashedPointer(JSContext* cx, T* ptr,
                                             F freeFunc) {
  using FreeFunction = void (*)(T*);
  static_assert(std::is_convertible_v<F, FreeFunction>,
                "free function is not of a compatible type");
  return detail::NewObjectWithUntypedStashedPointer(
      cx, ptr,
      reinterpret_cast<detail::UntypedFreeFunction>(
          static_cast<FreeFunction>(freeFunc)));
}

template <typename T>
inline JSObject* NewObjectWithStashedPointer(JSContext* cx, T* ptr) {
  return detail::NewObjectWithUntypedStashedPointer(cx, ptr, nullptr);
}

template <typename T>
inline T* ObjectGetStashedPointer(JSContext* cx, JSObject* obj) {
  return static_cast<T*>(detail::ObjectGetUntypedStashedPointer(cx, obj));
}

}  

#endif  // js_public_ObjectWithStashedPointer_h
