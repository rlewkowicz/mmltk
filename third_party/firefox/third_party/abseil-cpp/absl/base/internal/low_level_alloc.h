// Copyright 2017 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#if !defined(ABSL_BASE_INTERNAL_LOW_LEVEL_ALLOC_H_)
#define ABSL_BASE_INTERNAL_LOW_LEVEL_ALLOC_H_


// IWYU pragma: private, include "base/low_level_alloc.h"

#include <sys/types.h>

#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/base/config.h"

#if defined(ABSL_LOW_LEVEL_ALLOC_MISSING)
#error ABSL_LOW_LEVEL_ALLOC_MISSING cannot be directly set
#elif !defined(ABSL_HAVE_MMAP) && !0
#define ABSL_LOW_LEVEL_ALLOC_MISSING 1
#endif

#if defined(ABSL_LOW_LEVEL_ALLOC_ASYNC_SIGNAL_SAFE_MISSING)
#error ABSL_LOW_LEVEL_ALLOC_ASYNC_SIGNAL_SAFE_MISSING cannot be directly set
#elif 0 || defined(__asmjs__) || defined(__wasm__) || \
    defined(__hexagon__)
#define ABSL_LOW_LEVEL_ALLOC_ASYNC_SIGNAL_SAFE_MISSING 1
#endif

#include <cstddef>

#include "absl/base/port.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace base_internal {

class LowLevelAlloc {
 public:
  struct Arena;       

  static void *Alloc(size_t request) ABSL_ATTRIBUTE_SECTION(malloc_hook);
  static void *AllocWithArena(size_t request, Arena *arena)
      ABSL_ATTRIBUTE_SECTION(malloc_hook);

  static void Free(void *s) ABSL_ATTRIBUTE_SECTION(malloc_hook);


  enum {
    kCallMallocHook = 0x0001,

#if !defined(ABSL_LOW_LEVEL_ALLOC_ASYNC_SIGNAL_SAFE_MISSING)
    kAsyncSignalSafe = 0x0002,
#endif
  };
  static Arena *NewArena(uint32_t flags);

  static bool DeleteArena(Arena *arena);

  static Arena *DefaultArena();

 private:
  LowLevelAlloc();      
};

LowLevelAlloc::Arena *SigSafeArena();

void InitSigSafeArena();

}  
ABSL_NAMESPACE_END
}  

#endif
