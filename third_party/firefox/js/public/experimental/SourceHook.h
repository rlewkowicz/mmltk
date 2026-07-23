/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_experimental_SourceHook_h
#define js_experimental_SourceHook_h

#include "mozilla/UniquePtr.h"  // mozilla::UniquePtr

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

struct JS_PUBLIC_API JSContext;

namespace js {

class SourceHook {
 public:
  virtual ~SourceHook() = default;

  virtual bool load(JSContext* cx, const char* filename,
                    char16_t** twoByteSource, char** utf8Source,
                    size_t* length) = 0;
};

extern JS_PUBLIC_API void SetSourceHook(JSContext* cx,
                                        mozilla::UniquePtr<SourceHook> hook);

extern JS_PUBLIC_API mozilla::UniquePtr<SourceHook> ForgetSourceHook(
    JSContext* cx);

}  

#endif  // js_experimental_SourceHook_h
