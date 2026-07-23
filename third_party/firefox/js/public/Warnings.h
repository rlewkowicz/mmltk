/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_Warnings_h
#define js_Warnings_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_FORMAT_PRINTF, MOZ_RAII

#include "jstypes.h"  // JS_PUBLIC_API

struct JS_PUBLIC_API JSContext;
class JSErrorReport;

namespace JS {

extern JS_PUBLIC_API bool WarnASCII(JSContext* cx, const char* format, ...)
    MOZ_FORMAT_PRINTF(2, 3);

extern JS_PUBLIC_API bool WarnLatin1(JSContext* cx, const char* format, ...)
    MOZ_FORMAT_PRINTF(2, 3);

extern JS_PUBLIC_API bool WarnUTF8(JSContext* cx, const char* format, ...)
    MOZ_FORMAT_PRINTF(2, 3);

using WarningReporter = void (*)(JSContext* cx, JSErrorReport* report);

extern JS_PUBLIC_API WarningReporter GetWarningReporter(JSContext* cx);

extern JS_PUBLIC_API WarningReporter
SetWarningReporter(JSContext* cx, WarningReporter reporter);

class MOZ_RAII JS_PUBLIC_API AutoSuppressWarningReporter {
  JSContext* context_;
  WarningReporter prevReporter_;

 public:
  explicit AutoSuppressWarningReporter(JSContext* cx) : context_(cx) {
    prevReporter_ = SetWarningReporter(context_, nullptr);
  }

  ~AutoSuppressWarningReporter() {
#ifdef DEBUG
    WarningReporter reporter =
#endif
        SetWarningReporter(context_, prevReporter_);
    MOZ_ASSERT(reporter == nullptr, "Unexpected WarningReporter active");
    SetWarningReporter(context_, prevReporter_);
  }
};

}  

#endif  // js_Warnings_h
