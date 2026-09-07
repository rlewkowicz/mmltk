/*
 * Copyright 2017 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkWebpEncoder_DEFINED)
#define SkWebpEncoder_DEFINED

#include "include/core/SkRefCnt.h"
#include "include/core/SkSpan.h" // IWYU pragma: keep
#include "include/encode/SkEncoder.h"
#include "include/private/base/SkAPI.h"

class SkPixmap;
class SkWStream;
class SkData;
class GrDirectContext;
class SkImage;
struct skcms_ICCProfile;

namespace SkWebpEncoder {

enum class Compression {
    kLossy,
    kLossless,
};

struct SK_API Options {
    Compression fCompression = Compression::kLossy;
    float fQuality = 100.0f;
};

SK_API bool Encode(SkWStream* dst, const SkPixmap& src, const Options& options);

SK_API sk_sp<SkData> Encode(const SkPixmap& src, const Options& options);

SK_API sk_sp<SkData> Encode(GrDirectContext* ctx, const SkImage* img, const Options& options);

SK_API bool EncodeAnimated(SkWStream* dst,
                           SkSpan<const SkEncoder::Frame> src,
                           const Options& options);
} 

#endif
