/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_BytecodeCompiler_h
#define frontend_BytecodeCompiler_h

#include "mozilla/AlreadyAddRefed.h"  // already_AddRefed
#include "mozilla/Maybe.h"            // mozilla::Maybe
#include "mozilla/Utf8.h"             // mozilla::Utf8Unit

#include <stdint.h>  // uint32_t

#include "ds/LifoAlloc.h"                 // js::LifoAlloc
#include "frontend/FunctionSyntaxKind.h"  // FunctionSyntaxKind
#include "frontend/ScriptIndex.h"         // ScriptIndex
#include "js/CompileOptions.h"  // JS::ReadOnlyCompileOptions, JS::PrefableCompileOptions
#include "js/GCVector.h"    // JS::StackGCVector
#include "js/Id.h"          // JS::PropertyKey
#include "js/RootingAPI.h"  // JS::Handle
#include "js/SourceText.h"  // JS::SourceText
#include "js/UniquePtr.h"   // js::UniquePtr
#include "js/Value.h"       // JS::Value
#include "vm/ScopeKind.h"   // js::ScopeKind


class JSFunction;
class JSObject;
class JSScript;
struct JSContext;

namespace js {

class ModuleObject;
class FrontendContext;
class Scope;

namespace frontend {

struct CompilationInput;
struct CompilationStencil;
struct ExtensibleCompilationStencil;
struct InitialStencilAndDelazifications;
struct CompilationGCOutput;
class ScopeBindingCache;

extern already_AddRefed<CompilationStencil>
CompileGlobalScriptToStencilWithInput(JSContext* maybeCx, FrontendContext* fc,
                                      js::LifoAlloc& tempLifoAlloc,
                                      CompilationInput& input,
                                      ScopeBindingCache* scopeCache,
                                      JS::SourceText<mozilla::Utf8Unit>& srcBuf,
                                      ScopeKind scopeKind);

[[nodiscard]] extern bool InstantiateStencils(JSContext* cx,
                                              CompilationInput& input,
                                              const CompilationStencil& stencil,
                                              CompilationGCOutput& gcOutput);

[[nodiscard]] extern bool InstantiateStencils(
    JSContext* cx, CompilationInput& input,
    InitialStencilAndDelazifications& stencils, CompilationGCOutput& gcOutput);

extern JSScript* CompileGlobalScript(JSContext* cx, FrontendContext* fc,
                                     const JS::ReadOnlyCompileOptions& options,
                                     JS::SourceText<char16_t>& srcBuf,
                                     ScopeKind scopeKind);

extern JSScript* CompileGlobalScript(JSContext* cx, FrontendContext* fc,
                                     const JS::ReadOnlyCompileOptions& options,
                                     JS::SourceText<mozilla::Utf8Unit>& srcBuf,
                                     ScopeKind scopeKind);

extern JSScript* CompileGlobalScriptWithExtraBindings(
    JSContext* cx, FrontendContext* fc,
    const JS::ReadOnlyCompileOptions& options, JS::SourceText<char16_t>& srcBuf,
    JS::Handle<JS::StackGCVector<JS::PropertyKey>> unwrappedBindingKeys,
    JS::Handle<JS::StackGCVector<JS::Value>> unwrappedBindingValues,
    JS::MutableHandle<JSObject*> env);

extern JSScript* CompileEvalScript(JSContext* cx,
                                   const JS::ReadOnlyCompileOptions& options,
                                   JS::SourceText<char16_t>& srcBuf,
                                   JS::Handle<js::Scope*> enclosingScope,
                                   JS::Handle<JSObject*> enclosingEnv);

ModuleObject* CompileModule(JSContext* cx, FrontendContext* fc,
                            const JS::ReadOnlyCompileOptions& options,
                            JS::SourceText<char16_t>& srcBuf);
ModuleObject* CompileModule(JSContext* cx, FrontendContext* fc,
                            const JS::ReadOnlyCompileOptions& options,
                            JS::SourceText<mozilla::Utf8Unit>& srcBuf);

[[nodiscard]] JSFunction* CompileStandaloneFunction(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf,
    const mozilla::Maybe<uint32_t>& parameterListEnd,
    frontend::FunctionSyntaxKind syntaxKind);

[[nodiscard]] JSFunction* CompileStandaloneGenerator(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf,
    const mozilla::Maybe<uint32_t>& parameterListEnd,
    frontend::FunctionSyntaxKind syntaxKind);

[[nodiscard]] JSFunction* CompileStandaloneAsyncFunction(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf,
    const mozilla::Maybe<uint32_t>& parameterListEnd,
    frontend::FunctionSyntaxKind syntaxKind);

[[nodiscard]] JSFunction* CompileStandaloneAsyncGenerator(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf,
    const mozilla::Maybe<uint32_t>& parameterListEnd,
    frontend::FunctionSyntaxKind syntaxKind);

[[nodiscard]] JSFunction* CompileStandaloneFunctionInNonSyntacticScope(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf,
    const mozilla::Maybe<uint32_t>& parameterListEnd,
    frontend::FunctionSyntaxKind syntaxKind, JS::Handle<Scope*> enclosingScope);

extern bool DelazifyCanonicalScriptedFunction(JSContext* cx,
                                              FrontendContext* fc,
                                              JS::Handle<JSFunction*> fun);

enum class DelazifyFailureReason {
  Compressed,
  Other,
};

extern const CompilationStencil* DelazifyCanonicalScriptedFunction(
    FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    const JS::PrefableCompileOptions& prefableOptions,
    ScopeBindingCache* scopeCache, ScriptIndex scriptIndex,
    InitialStencilAndDelazifications* stencils,
    DelazifyFailureReason* failureReason);

inline bool CanLazilyParse(const JS::ReadOnlyCompileOptions& options) {
  return !options.discardSource && !options.sourceIsLazy &&
         !options.forceFullParse();
}

} 
} 

#endif /* frontend_BytecodeCompiler_h */
