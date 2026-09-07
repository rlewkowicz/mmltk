/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkPixmapUtils_DEFINED)
#define SkPixmapUtils_DEFINED

#include "include/codec/SkEncodedOrigin.h"
#include "include/core/SkImageInfo.h"
#include "include/private/base/SkAPI.h"

class SkPixmap;

namespace SkPixmapUtils {
SK_API bool Orient(const SkPixmap& dst, const SkPixmap& src, SkEncodedOrigin origin);

SK_API SkImageInfo SwapWidthHeight(const SkImageInfo& info);

}  

#endif
