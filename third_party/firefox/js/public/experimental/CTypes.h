/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_experimental_CTypes_h
#define js_experimental_CTypes_h

#include "mozilla/Attributes.h"  // MOZ_RAII

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/TypeDecls.h"

namespace JS {

#ifdef JS_HAS_CTYPES

extern JS_PUBLIC_API bool InitCTypesClass(JSContext* cx,
                                          Handle<JSObject*> global);

#endif  // JS_HAS_CTYPES

enum class CTypesActivityType {
  BeginCall,
  EndCall,
  BeginCallback,
  EndCallback,
};

using CTypesActivityCallback = void (*)(JSContext*, CTypesActivityType);

extern JS_PUBLIC_API void SetCTypesActivityCallback(JSContext* cx,
                                                    CTypesActivityCallback cb);

class MOZ_RAII JS_PUBLIC_API AutoCTypesActivityCallback {
 private:
  JSContext* cx;
  CTypesActivityCallback callback;
  CTypesActivityType endType;

 public:
  AutoCTypesActivityCallback(JSContext* cx, CTypesActivityType beginType,
                             CTypesActivityType endType);

  ~AutoCTypesActivityCallback() { DoEndCallback(); }

  void DoEndCallback() {
    if (callback) {
      callback(cx, endType);
      callback = nullptr;
    }
  }
};

#ifdef JS_HAS_CTYPES

using CTypesUnicodeToNativeFun = char* (*)(JSContext*, const char16_t*, size_t);

struct CTypesCallbacks {
  CTypesUnicodeToNativeFun unicodeToNative;
};

extern JS_PUBLIC_API void SetCTypesCallbacks(JSObject* ctypesObj,
                                             const CTypesCallbacks* callbacks);

#endif  // JS_HAS_CTYPES

}  

#endif  // js_experimental_CTypes_h
