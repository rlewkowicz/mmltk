/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_CompilationAndEvaluation_h
#define js_CompilationAndEvaluation_h

#include <stddef.h>  // size_t
#include <stdio.h>   // FILE

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/RootingAPI.h"  // JS::Handle, JS::MutableHandle
#include "js/TypeDecls.h"

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSFunction;
class JS_PUBLIC_API JSObject;
class JS_PUBLIC_API JSScript;

namespace mozilla {
union Utf8Unit;
}

namespace JS {

class JS_PUBLIC_API EnvironmentChain;
class JS_PUBLIC_API InstantiateOptions;
class JS_PUBLIC_API ReadOnlyCompileOptions;

template <typename UnitT>
class SourceText;

}  

extern JS_PUBLIC_API bool JS_Utf8BufferIsCompilableUnit(
    JSContext* cx, JS::Handle<JSObject*> obj, const char* utf8, size_t length);


extern JS_PUBLIC_API bool JS_ExecuteScript(JSContext* cx,
                                           JS::Handle<JSScript*> script,
                                           JS::MutableHandle<JS::Value> rval);

extern JS_PUBLIC_API bool JS_ExecuteScript(JSContext* cx,
                                           JS::Handle<JSScript*> script);

extern JS_PUBLIC_API bool JS_ExecuteScript(JSContext* cx,
                                           const JS::EnvironmentChain& envChain,
                                           JS::Handle<JSScript*> script,
                                           JS::MutableHandle<JS::Value> rval);

extern JS_PUBLIC_API bool JS_ExecuteScript(JSContext* cx,
                                           const JS::EnvironmentChain& envChain,
                                           JS::Handle<JSScript*> script);

namespace JS {

extern JS_PUBLIC_API bool Evaluate(JSContext* cx,
                                   const ReadOnlyCompileOptions& options,
                                   SourceText<char16_t>& srcBuf,
                                   MutableHandle<Value> rval);

extern JS_PUBLIC_API bool Evaluate(JSContext* cx,
                                   const JS::EnvironmentChain& envChain,
                                   const ReadOnlyCompileOptions& options,
                                   SourceText<char16_t>& srcBuf,
                                   MutableHandle<Value> rval);

extern JS_PUBLIC_API bool Evaluate(JSContext* cx,
                                   const ReadOnlyCompileOptions& options,
                                   SourceText<mozilla::Utf8Unit>& srcBuf,
                                   MutableHandle<Value> rval);

extern JS_PUBLIC_API bool EvaluateUtf8Path(
    JSContext* cx, const ReadOnlyCompileOptions& options, const char* filename,
    MutableHandle<Value> rval);

extern JS_PUBLIC_API JSScript* Compile(JSContext* cx,
                                       const ReadOnlyCompileOptions& options,
                                       SourceText<char16_t>& srcBuf);

extern JS_PUBLIC_API JSScript* Compile(JSContext* cx,
                                       const ReadOnlyCompileOptions& options,
                                       SourceText<mozilla::Utf8Unit>& srcBuf);

extern JS_PUBLIC_API JSScript* CompileUtf8File(
    JSContext* cx, const ReadOnlyCompileOptions& options, FILE* file);

extern JS_PUBLIC_API JSScript* CompileUtf8Path(
    JSContext* cx, const ReadOnlyCompileOptions& options, const char* filename);

extern JS_PUBLIC_API JSFunction* CompileFunction(
    JSContext* cx, const JS::EnvironmentChain& envChain,
    const ReadOnlyCompileOptions& options, const char* name, unsigned nargs,
    const char* const* argnames, SourceText<char16_t>& srcBuf);

extern JS_PUBLIC_API JSFunction* CompileFunction(
    JSContext* cx, const JS::EnvironmentChain& envChain,
    const ReadOnlyCompileOptions& options, const char* name, unsigned nargs,
    const char* const* argnames, SourceText<mozilla::Utf8Unit>& srcBuf);

extern JS_PUBLIC_API JSFunction* CompileFunctionUtf8(
    JSContext* cx, const JS::EnvironmentChain& envChain,
    const ReadOnlyCompileOptions& options, const char* name, unsigned nargs,
    const char* const* argnames, const char* utf8, size_t length);

extern JS_PUBLIC_API bool UpdateDebugMetadata(
    JSContext* cx, Handle<JSScript*> script, const InstantiateOptions& options,
    HandleValue privateValue, HandleString elementAttributeName,
    HandleScript introScript, HandleScript scriptOrModule);

} 

#endif /* js_CompilationAndEvaluation_h */
