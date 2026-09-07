// Copyright 2019 Google LLC
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#if !defined(SkPixelRefPriv_DEFINED)
#define SkPixelRefPriv_DEFINED

#include "include/core/SkRefCnt.h"

#include <cstddef>

class SkPixelRef;

sk_sp<SkPixelRef> SkMakePixelRefWithProc(int w, int h, size_t rowBytes, void* addr,
                                         void (*releaseProc)(void* addr, void* ctx), void* ctx);

#endif
