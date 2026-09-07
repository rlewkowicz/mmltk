/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_mozalloc_oom_h
#define mozilla_mozalloc_oom_h

#include "mozalloc.h"

#ifdef __wasm__
__attribute__((import_module("env")))
__attribute__((import_name("mozalloc_handle_oom")))
#  if defined(__clang__) && (__clang_major__ < 11)
__attribute__((weak))
#  endif
#endif
MFBT_API void
mozalloc_handle_oom(size_t requestedSize);

extern MFBT_DATA size_t gOOMAllocationSize;


#endif /* ifndef mozilla_mozalloc_oom_h */
