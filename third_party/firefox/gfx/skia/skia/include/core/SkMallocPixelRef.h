/*
 * Copyright 2008 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkMallocPixelRef_DEFINED)
#define SkMallocPixelRef_DEFINED

#include "include/core/SkRefCnt.h"
#include "include/core/SkTypes.h"

#include <cstddef>

class SkData;
class SkPixelRef;
struct SkImageInfo;

namespace SkMallocPixelRef {
    SK_API sk_sp<SkPixelRef> MakeAllocate(const SkImageInfo&, size_t rowBytes);

    SK_API sk_sp<SkPixelRef> MakeWithData(const SkImageInfo&, size_t rowBytes, sk_sp<SkData> data);
}  
#endif
