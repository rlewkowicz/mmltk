/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <string.h>
#include "mozmemory_wrap.h"
#include "mozilla/Types.h"

#define NOTHROW_MALLOC_DECL(name, return_type, ...) \
  MOZ_MEMORY_API return_type name##_impl(__VA_ARGS__) noexcept(true);
#define MALLOC_DECL(name, return_type, ...) \
  MOZ_MEMORY_API return_type name##_impl(__VA_ARGS__);
#define MALLOC_FUNCS MALLOC_FUNCS_MALLOC
#include "malloc_decls.h"

#undef strndup
#undef strdup

MOZ_MEMORY_API char* strndup_impl(const char* src, size_t len) {
  char* dst = (char*)malloc_impl(len + 1);
  if (dst) {
    strncpy(dst, src, len);
    dst[len] = '\0';
  }
  return dst;
}

MOZ_MEMORY_API char* strdup_impl(const char* src) {
  size_t len = strlen(src);
  return strndup_impl(src, len);
}
