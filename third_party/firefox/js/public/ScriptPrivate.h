/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_ScriptPrivate_h
#define js_ScriptPrivate_h

#include "jstypes.h"

#include "js/TypeDecls.h"

namespace JS {

extern JS_PUBLIC_API void SetScriptPrivate(JSScript* script,
                                           const JS::Value& value);

extern JS_PUBLIC_API JS::Value GetScriptPrivate(JSScript* script);

extern JS_PUBLIC_API JS::Value GetScriptedCallerPrivate(JSContext* cx);

using ScriptPrivateReferenceHook = void (*)(const JS::Value&);

extern JS_PUBLIC_API void SetScriptPrivateReferenceHooks(
    JSRuntime* rt, ScriptPrivateReferenceHook addRefHook,
    ScriptPrivateReferenceHook releaseHook);

}  

#endif  // js_ScriptPrivate_h
