// Copyright 2023 The Abseil Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#ifndef ABSL_BASE_PREFETCH_H_
#define ABSL_BASE_PREFETCH_H_

#include "absl/base/attributes.h"
#include "absl/base/config.h"

#if defined(ABSL_INTERNAL_HAVE_SSE)
#include <xmmintrin.h>
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#if defined(ABSL_INTERNAL_HAVE_SSE)
#pragma intrinsic(_mm_prefetch)
#endif
#endif

namespace absl {
ABSL_NAMESPACE_BEGIN

void PrefetchToLocalCache(const void* addr);

void PrefetchToLocalCacheNta(const void* addr);

void PrefetchToLocalCacheForWrite(const void* addr);

#if ABSL_HAVE_BUILTIN(__builtin_prefetch) || defined(__GNUC__)

#define ABSL_HAVE_PREFETCH 1

ABSL_ATTRIBUTE_ALWAYS_INLINE inline void PrefetchToLocalCache(
    const void* addr) {
  __builtin_prefetch(addr, 0, 3);
}

ABSL_ATTRIBUTE_ALWAYS_INLINE inline void PrefetchToLocalCacheNta(
    const void* addr) {
  __builtin_prefetch(addr, 0, 0);
}

ABSL_ATTRIBUTE_ALWAYS_INLINE inline void PrefetchToLocalCacheForWrite(
    const void* addr) {
#if defined(__x86_64__) && !defined(__PRFCHW__)
  asm("prefetchw %0" : : "m"(*reinterpret_cast<const char*>(addr)));
#else
  __builtin_prefetch(addr, 1, 3);
#endif
}

#elif defined(ABSL_INTERNAL_HAVE_SSE)

#define ABSL_HAVE_PREFETCH 1

ABSL_ATTRIBUTE_ALWAYS_INLINE inline void PrefetchToLocalCache(
    const void* addr) {
  _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T0);
}

ABSL_ATTRIBUTE_ALWAYS_INLINE inline void PrefetchToLocalCacheNta(
    const void* addr) {
  _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_NTA);
}

ABSL_ATTRIBUTE_ALWAYS_INLINE inline void PrefetchToLocalCacheForWrite(
    const void* addr) {
#if defined(_MM_HINT_ET0)
  _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_ET0);
#elif !defined(_MSC_VER) && defined(__x86_64__)
  asm("prefetchw %0" : : "m"(*reinterpret_cast<const char*>(addr)));
#endif
}

#else

ABSL_ATTRIBUTE_ALWAYS_INLINE inline void PrefetchToLocalCache(
    const void* addr) {}
ABSL_ATTRIBUTE_ALWAYS_INLINE inline void PrefetchToLocalCacheNta(
    const void* addr) {}
ABSL_ATTRIBUTE_ALWAYS_INLINE inline void PrefetchToLocalCacheForWrite(
    const void* addr) {}

#endif

ABSL_NAMESPACE_END
}  

#endif  // ABSL_BASE_PREFETCH_H_
