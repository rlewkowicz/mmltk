/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_experimental_PCCountProfiling_h
#define js_experimental_PCCountProfiling_h

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSString;

namespace JS {

extern JS_PUBLIC_API void StartPCCountProfiling(JSContext* cx);

extern JS_PUBLIC_API void StopPCCountProfiling(JSContext* cx);

extern JS_PUBLIC_API size_t GetPCCountScriptCount(JSContext* cx);

extern JS_PUBLIC_API JSString* GetPCCountScriptSummary(JSContext* cx,
                                                       size_t script);

extern JS_PUBLIC_API JSString* GetPCCountScriptContents(JSContext* cx,
                                                        size_t script);

extern JS_PUBLIC_API void PurgePCCounts(JSContext* cx);

}  

#endif  // js_experimental_PCCountProfiling_h
