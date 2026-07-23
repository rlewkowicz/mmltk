/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_friend_WindowProxy_h
#define js_friend_WindowProxy_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/Class.h"       // JSCLASS_IS_GLOBAL
#include "js/Object.h"      // JS::GetClass
#include "js/RootingAPI.h"  // JS::Handle

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace js {

extern JS_PUBLIC_API void SetWindowProxyClass(JSContext* cx,
                                              const JSClass* clasp);

extern JS_PUBLIC_API void SetWindowProxy(JSContext* cx,
                                         JS::Handle<JSObject*> global,
                                         JS::Handle<JSObject*> windowProxy);

namespace detail {

extern JS_PUBLIC_API bool IsWindowSlow(JSObject* obj);

extern JS_PUBLIC_API JSObject* ToWindowProxyIfWindowSlow(JSObject* obj);

}  

inline bool IsWindow(JSObject* obj) {
  if (JS::GetClass(obj)->flags & JSCLASS_IS_GLOBAL) {
    return detail::IsWindowSlow(obj);
  }
  return false;
}

extern JS_PUBLIC_API bool IsWindowProxy(JSObject* obj);

MOZ_ALWAYS_INLINE JSObject* ToWindowProxyIfWindow(JSObject* obj) {
  if (JS::GetClass(obj)->flags & JSCLASS_IS_GLOBAL) {
    return detail::ToWindowProxyIfWindowSlow(obj);
  }
  return obj;
}

extern JS_PUBLIC_API JSObject* ToWindowIfWindowProxy(JSObject* obj);

}  

#endif  // js_friend_WindowProxy_h
