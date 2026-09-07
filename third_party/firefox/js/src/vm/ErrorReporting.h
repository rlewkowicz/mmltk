/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ErrorReporting_h
#define vm_ErrorReporting_h

#include <stdarg.h>

#include "jsfriendapi.h"  // for ScriptEnvironmentPreparer

#include "js/CharacterEncoding.h"  // JS::ConstUTF8CharsZ
#include "js/ColumnNumber.h"       // JS::ColumnNumberOneOrigin
#include "js/ErrorReport.h"        // for JSErrorNotes, JSErrorReport
#include "js/UniquePtr.h"          // for UniquePtr
#include "js/Utility.h"            // for UniqueTwoByteChars

namespace js {

class FrontendContext;

using JSAllocator = JSContext;

struct ErrorMetadata {
  JS::ConstUTF8CharsZ filename;


  uint32_t lineNumber;

  JS::ColumnNumberOneOrigin columnNumber;

  JS::UniqueTwoByteChars lineOfContext;

  static constexpr size_t lineOfContextRadius = 60;

  size_t lineLength;

  size_t tokenOffset;

  bool isMuted;
};

class CompileError : public JSErrorReport {
 public:
  bool throwError(JSContext* cx);
};

class MOZ_STACK_CLASS ReportExceptionClosure final
    : public ScriptEnvironmentPreparer::Closure {
  JS::HandleValue exn_;

 public:
  explicit ReportExceptionClosure(JS::HandleValue exn) : exn_(exn) {}

  bool operator()(JSContext* cx) override;
};

extern void CallWarningReporter(JSContext* cx, JSErrorReport* report);

extern void ReportCompileErrorLatin1VA(FrontendContext* fc,
                                       ErrorMetadata&& metadata,
                                       UniquePtr<JSErrorNotes> notes,
                                       unsigned errorNumber, va_list* args);

extern void ReportCompileErrorUTF8VA(FrontendContext* fc,
                                     ErrorMetadata&& metadata,
                                     UniquePtr<JSErrorNotes> notes,
                                     unsigned errorNumber, va_list* args);

extern void ReportCompileErrorLatin1(FrontendContext* fc,
                                     ErrorMetadata&& metadata,
                                     UniquePtr<JSErrorNotes> notes,
                                     unsigned errorNumber, ...);

extern void ReportCompileErrorUTF8(FrontendContext* fc,
                                   ErrorMetadata&& metadata,
                                   UniquePtr<JSErrorNotes> notes,
                                   unsigned errorNumber, ...);

[[nodiscard]] extern bool ReportCompileWarning(FrontendContext* fc,
                                               ErrorMetadata&& metadata,
                                               UniquePtr<JSErrorNotes> notes,
                                               unsigned errorNumber,
                                               va_list* args);

class GlobalObject;

extern void ReportErrorToGlobal(JSContext* cx,
                                JS::Handle<js::GlobalObject*> global,
                                JS::HandleValue error);

enum class IsWarning { No, Yes };

extern bool ReportErrorVA(JSContext* cx, IsWarning isWarning,
                          const char* format, ErrorArgumentsType argumentsType,
                          va_list ap) MOZ_FORMAT_PRINTF(3, 0);

extern bool ReportErrorNumberVA(JSContext* cx, IsWarning isWarning,
                                JSErrorCallback callback, void* userRef,
                                const unsigned errorNumber,
                                ErrorArgumentsType argumentsType, va_list ap);

extern bool ReportErrorNumberUCArray(JSContext* cx, IsWarning isWarning,
                                     JSErrorCallback callback, void* userRef,
                                     const unsigned errorNumber,
                                     const char16_t** args);

extern bool ReportErrorNumberUTF8Array(JSContext* cx, IsWarning isWarning,
                                       JSErrorCallback callback, void* userRef,
                                       const unsigned errorNumber,
                                       const char** args);

extern bool ExpandErrorArgumentsVA(FrontendContext* fc,
                                   JSErrorCallback callback, void* userRef,
                                   const unsigned errorNumber,
                                   const char16_t** messageArgs,
                                   ErrorArgumentsType argumentsType,
                                   JSErrorReport* reportp, va_list ap);

extern bool ExpandErrorArgumentsVA(FrontendContext* fc,
                                   JSErrorCallback callback, void* userRef,
                                   const unsigned errorNumber,
                                   const char** messageArgs,
                                   ErrorArgumentsType argumentsType,
                                   JSErrorReport* reportp, va_list ap);

extern bool ExpandErrorArgumentsVA(FrontendContext* fc,
                                   JSErrorCallback callback, void* userRef,
                                   const unsigned errorNumber,
                                   ErrorArgumentsType argumentsType,
                                   JSErrorReport* reportp, va_list ap);

extern bool ExpandErrorArgumentsVA(FrontendContext* fc,
                                   JSErrorCallback callback, void* userRef,
                                   const unsigned errorNumber,
                                   const char16_t** messageArgs,
                                   ErrorArgumentsType argumentsType,
                                   JSErrorNotes::Note* notep, va_list ap);

extern void MaybePrintAndClearPendingException(JSContext* cx);

}  

#endif /* vm_ErrorReporting_h */
