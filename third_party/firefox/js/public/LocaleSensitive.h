/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_LocaleSensitive_h
#define js_LocaleSensitive_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/RootingAPI.h"  // JS::Handle, JS::MutableHandle
#include "js/Utility.h"     // JS::UniqueChars
#include "js/Value.h"       // JS::Value

struct JS_PUBLIC_API JSContext;
struct JS_PUBLIC_API JSRuntime;
class JS_PUBLIC_API JSString;

extern JS_PUBLIC_API bool JS_SetDefaultLocale(JSRuntime* rt,
                                              const char* locale);

extern JS_PUBLIC_API JS::UniqueChars JS_GetDefaultLocale(JSContext* cx);

extern JS_PUBLIC_API void JS_ResetDefaultLocale(JSRuntime* rt);

using JSLocaleToUpperCase = bool (*)(JSContext* cx, JS::Handle<JSString*> src,
                                     JS::MutableHandle<JS::Value> rval);

using JSLocaleToLowerCase = bool (*)(JSContext* cx, JS::Handle<JSString*> src,
                                     JS::MutableHandle<JS::Value> rval);

using JSLocaleCompare = bool (*)(JSContext* cx, JS::Handle<JSString*> src1,
                                 JS::Handle<JSString*> src2,
                                 JS::MutableHandle<JS::Value> rval);

using JSLocaleToUnicode = bool (*)(JSContext* cx, const char* src,
                                   JS::MutableHandle<JS::Value> rval);

struct JSLocaleCallbacks {
  JSLocaleToUpperCase localeToUpperCase;
  JSLocaleToLowerCase localeToLowerCase;
  JSLocaleCompare localeCompare;
  JSLocaleToUnicode localeToUnicode;
};

extern JS_PUBLIC_API void JS_SetLocaleCallbacks(
    JSRuntime* rt, const JSLocaleCallbacks* callbacks);

extern JS_PUBLIC_API const JSLocaleCallbacks* JS_GetLocaleCallbacks(
    JSRuntime* rt);

#endif /* js_LocaleSensitive_h */
