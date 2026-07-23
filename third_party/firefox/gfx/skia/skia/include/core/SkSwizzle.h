/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkSwizzle_DEFINED)
#define SkSwizzle_DEFINED

#include "include/private/base/SkAPI.h"

#include <cstdint>

SK_API void SkSwapRB(uint32_t* dest, const uint32_t* src, int count);

#endif
