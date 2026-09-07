/*
 * Copyright 2025 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkPMColor_DEFINED)
#define SkPMColor_DEFINED

#include "include/core/SkColor.h"
#include "include/private/base/SkAPI.h"

#include <cstdint>

SK_API SkPMColor SkPMColorSetARGB(SkAlpha a, uint8_t r, uint8_t g, uint8_t b);

SK_API SkAlpha SkPMColorGetA(SkPMColor);

SK_API uint8_t SkPMColorGetR(SkPMColor);

SK_API uint8_t SkPMColorGetG(SkPMColor);

SK_API uint8_t SkPMColorGetB(SkPMColor);

#endif
