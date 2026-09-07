/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef intl_components_ICU4CLibrary_h
#define intl_components_ICU4CLibrary_h

#include "mozilla/intl/ICU4CGlue.h"
#include "mozilla/Span.h"

#include <stddef.h>

namespace mozilla::intl {
class ICU4CLibrary final {
 public:
  ICU4CLibrary() = delete;

  static ICUResult Initialize();

  static void Cleanup();

  struct MemoryFunctions {
    using AllocFn = void* (*)(const void*, size_t);
    using ReallocFn = void* (*)(const void*, void*, size_t);
    using FreeFn = void (*)(const void*, void*);

    AllocFn mAllocFn = nullptr;

    ReallocFn mReallocFn = nullptr;

    FreeFn mFreeFn = nullptr;
  };

  static ICUResult SetMemoryFunctions(MemoryFunctions aMemoryFunctions);

  static Span<const char> GetVersion();
};
}  

#endif
