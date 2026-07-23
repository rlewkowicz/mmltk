/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_Zone_h
#define js_Zone_h

#include "mozilla/MemoryReporting.h"  // mozilla::MallocSizeOf

#include <stddef.h>  // size_t

#include "jstypes.h"        // JS_PUBLIC_API
#include "js/RootingAPI.h"  // JS::Handle
#include "js/TypeDecls.h"  // JSContext, JSObject, jsid, JS::Compartment, JS::GCContext, JS::Value, JS::Zone


using JSDestroyZoneCallback = void (*)(JS::GCContext*, JS::Zone*);

using JSDestroyCompartmentCallback = void (*)(JS::GCContext*, JS::Compartment*);

using JSSizeOfIncludingThisCompartmentCallback =
    size_t (*)(mozilla::MallocSizeOf, JS::Compartment*);

extern JS_PUBLIC_API void JS_SetDestroyZoneCallback(
    JSContext* cx, JSDestroyZoneCallback callback);

extern JS_PUBLIC_API void JS_SetDestroyCompartmentCallback(
    JSContext* cx, JSDestroyCompartmentCallback callback);

extern JS_PUBLIC_API void JS_SetSizeOfIncludingThisCompartmentCallback(
    JSContext* cx, JSSizeOfIncludingThisCompartmentCallback callback);

extern JS_PUBLIC_API void JS_SetCompartmentPrivate(JS::Compartment* compartment,
                                                   void* data);

extern JS_PUBLIC_API void* JS_GetCompartmentPrivate(
    JS::Compartment* compartment);

extern JS_PUBLIC_API void JS_SetZoneUserData(JS::Zone* zone, void* data);

extern JS_PUBLIC_API void* JS_GetZoneUserData(JS::Zone* zone);

extern JS_PUBLIC_API bool JS_RefreshCrossCompartmentWrappers(
    JSContext* cx, JS::Handle<JSObject*> obj);

extern JS_PUBLIC_API void JS_MarkCrossZoneId(JSContext* cx, jsid id);

extern JS_PUBLIC_API void JS_MarkCrossZoneIdValue(JSContext* cx,
                                                  const JS::Value& value);

#endif  // js_Zone_h
