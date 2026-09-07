/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozmemory_h)
#define mozmemory_h


#if defined(MALLOC_H)
#  include MALLOC_H
#endif

#include "mozmemory_wrap.h"
#include "mozilla/Types.h"
#include "mozjemalloc_types.h"
#include "malloc_decls.h"
#include "stdbool.h"

#if defined(MOZ_MEMORY)
MOZ_MEMORY_API size_t malloc_good_size_impl(size_t size);

static inline size_t _malloc_good_size(size_t size) {
#if defined(MOZ_GLUE_IN_PROGRAM) && !defined(IMPL_MFBT)
  if (!malloc_good_size) return size;
#endif
  return malloc_good_size_impl(size);
}

#    define malloc_good_size _malloc_good_size

#  define MALLOC_DECL(name, return_type, ...) \
    MOZ_JEMALLOC_API return_type name(__VA_ARGS__);
#  define MALLOC_FUNCS MALLOC_FUNCS_JEMALLOC
#  include "malloc_decls.h"

#if defined(__cplusplus)
static inline void jemalloc_stats(jemalloc_stats_t* aStats,
                                  jemalloc_bin_stats_t* aBinStats = nullptr) {
  jemalloc_stats_internal(aStats, aBinStats);
}
#else
static inline void jemalloc_stats(jemalloc_stats_t* aStats) {
  jemalloc_stats_internal(aStats, NULL);
}
#endif

#endif

#define NOTHROW_MALLOC_DECL(name, return_type, ...) \
  MOZ_JEMALLOC_API return_type name(__VA_ARGS__) noexcept(true);
#define MALLOC_DECL(name, return_type, ...) \
  MOZ_JEMALLOC_API return_type name(__VA_ARGS__);
#define MALLOC_FUNCS MALLOC_FUNCS_ARENA
#include "malloc_decls.h"

#if defined(__cplusplus)
#  define moz_create_arena() moz_create_arena_with_params(nullptr)
#else
#  define moz_create_arena() moz_create_arena_with_params(NULL)
#endif

#endif
