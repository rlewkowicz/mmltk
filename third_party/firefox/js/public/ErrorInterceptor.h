/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_ErrorInterceptor_h
#define js_ErrorInterceptor_h

#include "jstypes.h"

#include "js/TypeDecls.h"

struct JSErrorInterceptor {
  virtual void interceptError(JSContext* cx, JS::HandleValue error) = 0;
};

extern JS_PUBLIC_API void JS_SetErrorInterceptorCallback(
    JSRuntime*, JSErrorInterceptor* callback);

extern JS_PUBLIC_API JSErrorInterceptor* JS_GetErrorInterceptorCallback(
    JSRuntime*);

#endif  // js_ErrorInterceptor_h
