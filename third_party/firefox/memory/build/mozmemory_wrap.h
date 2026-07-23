/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozmemory_wrap_h)
#define mozmemory_wrap_h


#if defined(MOZ_MEMORY_IMPL) && !defined(IMPL_MFBT)
#if defined(MFBT_API)
#    error mozmemory_wrap.h has to be included before mozilla/Types.h when MOZ_MEMORY_IMPL is set and IMPL_MFBT is not.
#endif
#  define IMPL_MFBT
#endif

#include "mozilla/Types.h"

#if !defined(MOZ_EXTERN_C)
#if defined(__cplusplus)
#    define MOZ_EXTERN_C extern "C"
#else
#    define MOZ_EXTERN_C
#endif
#endif

#if defined(MOZ_MEMORY_IMPL)
#  define MOZ_JEMALLOC_API MOZ_EXTERN_C MFBT_API
#  define MOZ_JEMALLOC_API_NODISCARD MOZ_EXTERN_C [[nodiscard]] MFBT_API
#    define MOZ_MEMORY_API MOZ_EXTERN_C MFBT_API
#endif

#if !defined(MOZ_MEMORY_IMPL)
#  define MOZ_MEMORY_API MOZ_EXTERN_C MFBT_API
#  define MOZ_JEMALLOC_API MOZ_EXTERN_C MFBT_API
#  define MOZ_JEMALLOC_API_NODISCARD MOZ_EXTERN_C [[nodiscard]] MFBT_API
#endif

#if !defined(MOZ_MEMORY_API)
#  define MOZ_MEMORY_API MOZ_EXTERN_C
#endif
#if !defined(MOZ_JEMALLOC_API)
#  define MOZ_JEMALLOC_API MOZ_EXTERN_C
#  define MOZ_JEMALLOC_API_NODISCARD MOZ_EXTERN_C [[nodiscard]]
#endif

#if !defined(mozmem_malloc_impl)
#  define mozmem_malloc_impl(a) a
#endif
#if !defined(mozmem_dup_impl)
#  define mozmem_dup_impl(a) a
#endif

#define malloc_impl mozmem_malloc_impl(malloc)
#define posix_memalign_impl mozmem_malloc_impl(posix_memalign)
#define aligned_alloc_impl mozmem_malloc_impl(aligned_alloc)
#define calloc_impl mozmem_malloc_impl(calloc)
#define realloc_impl mozmem_malloc_impl(realloc)
#define free_impl mozmem_malloc_impl(free)
#define memalign_impl mozmem_malloc_impl(memalign)
#define valloc_impl mozmem_malloc_impl(valloc)
#define malloc_usable_size_impl mozmem_malloc_impl(malloc_usable_size)
#define malloc_good_size_impl mozmem_malloc_impl(malloc_good_size)

#define strndup_impl mozmem_dup_impl(strndup)
#define strdup_impl mozmem_dup_impl(strdup)


#endif
