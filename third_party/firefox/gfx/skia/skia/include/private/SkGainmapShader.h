/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkGainmapShader_DEFINED)
#define SkGainmapShader_DEFINED

#include "include/core/SkRefCnt.h"
#include "include/private/base/SkAPI.h"

class SkColorSpace;
class SkShader;
class SkImage;
struct SkGainmapInfo;
struct SkRect;
struct SkSamplingOptions;

class SK_API SkGainmapShader {
public:
    static sk_sp<SkShader> Make(const sk_sp<const SkImage>& baseImage,
                                const SkRect& baseRect,
                                const SkSamplingOptions& baseSamplingOptions,
                                const sk_sp<const SkImage>& gainmapImage,
                                const SkRect& gainmapRect,
                                const SkSamplingOptions& gainmapSamplingOptions,
                                const SkGainmapInfo& gainmapInfo,
                                const SkRect& dstRect,
                                float dstHdrRatio);

    static sk_sp<SkShader> Make(const sk_sp<const SkImage>& baseImage,
                                const SkRect& baseRect,
                                const SkSamplingOptions& baseSamplingOptions,
                                const sk_sp<const SkImage>& gainmapImage,
                                const SkRect& gainmapRect,
                                const SkSamplingOptions& gainmapSamplingOptions,
                                const SkGainmapInfo& gainmapInfo,
                                const SkRect& dstRect,
                                float dstHdrRatio,
                                sk_sp<SkColorSpace> dstColorSpace);
};

#endif
