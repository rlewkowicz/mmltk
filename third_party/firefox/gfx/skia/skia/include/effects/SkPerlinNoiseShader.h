/*
 * Copyright 2013 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkPerlinNoiseShader_DEFINED)
#define SkPerlinNoiseShader_DEFINED

#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkShader.h" // IWYU pragma: keep
#include "include/private/base/SkAPI.h"

struct SkISize;

namespace SkShaders {
SK_API sk_sp<SkShader> MakeFractalNoise(SkScalar baseFrequencyX, SkScalar baseFrequencyY,
                                        int numOctaves, SkScalar seed,
                                        const SkISize* tileSize = nullptr);
SK_API sk_sp<SkShader> MakeTurbulence(SkScalar baseFrequencyX, SkScalar baseFrequencyY,
                                      int numOctaves, SkScalar seed,
                                      const SkISize* tileSize = nullptr);
}  

#endif
