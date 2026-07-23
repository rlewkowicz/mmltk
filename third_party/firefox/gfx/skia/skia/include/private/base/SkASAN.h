/*
 * Copyright 2020 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkASAN_DEFINED)
#define SkASAN_DEFINED

#include <cstddef>

#if defined(MOZ_SKIA)

#include "mozilla/MemoryChecking.h"

#if defined(MOZ_HAVE_MEM_CHECKS)
#define SK_SANITIZE_ADDRESS MOZ_HAVE_MEM_CHECKS
#endif

static inline void sk_asan_poison_memory_region([[maybe_unused]] void const volatile* addr,
                                                [[maybe_unused]] size_t size) {
    MOZ_MAKE_MEM_NOACCESS(addr, size);
}

static inline void sk_asan_unpoison_memory_region([[maybe_unused]] void const volatile* addr,
                                                  [[maybe_unused]] size_t size) {
    MOZ_MAKE_MEM_DEFINED(addr, size);
}

static inline int sk_asan_address_is_poisoned([[maybe_unused]] void const volatile* addr) {
    return 0;
}

#else

#if defined(__SANITIZE_ADDRESS__)
    #define SK_SANITIZE_ADDRESS 1
#endif
#if !defined(SK_SANITIZE_ADDRESS) && defined(__has_feature)
    #if __has_feature(address_sanitizer)
        #define SK_SANITIZE_ADDRESS 1
    #endif
#endif

#if defined(SK_SANITIZE_ADDRESS)
extern "C" {
    void __asan_poison_memory_region(void const volatile *addr, size_t size);
    void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
    int __asan_address_is_poisoned(void const volatile *addr);
}
#endif

static inline void sk_asan_poison_memory_region([[maybe_unused]] void const volatile* addr,
                                                [[maybe_unused]] size_t size) {
#if defined(SK_SANITIZE_ADDRESS)
    __asan_poison_memory_region(addr, size);
#endif
}

static inline void sk_asan_unpoison_memory_region([[maybe_unused]] void const volatile* addr,
                                                  [[maybe_unused]] size_t size) {
#if defined(SK_SANITIZE_ADDRESS)
    __asan_unpoison_memory_region(addr, size);
#endif
}

static inline int sk_asan_address_is_poisoned([[maybe_unused]] void const volatile* addr) {
#if defined(SK_SANITIZE_ADDRESS)
    return __asan_address_is_poisoned(addr);
#else
    return 0;
#endif
}

#endif

#endif
