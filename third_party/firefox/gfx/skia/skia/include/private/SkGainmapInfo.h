/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkGainmapInfo_DEFINED)
#define SkGainmapInfo_DEFINED

#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkRefCnt.h"
class SkData;

/**
 *  Gainmap rendering parameters. Suppose our display has HDR to SDR ratio of H and we wish to
 *  display an image with gainmap on this display. Let B be the pixel value from the base image
 *  in a color space that has the primaries of the base image and a linear transfer function. Let
 *  G be the pixel value from the gainmap. Let D be the output pixel in the same color space as B.
 *  The value of D is computed as follows:
 *
 *  First, let W be a weight parameter determing how much the gainmap will be applied.
 *    W = clamp((log(H)                - log(fDisplayRatioSdr)) /
 *              (log(fDisplayRatioHdr) - log(fDisplayRatioSdr), 0, 1)
 *
 *  Next, let L be the gainmap value in log space. We compute this from the value G that was
 *  sampled from the texture as follows:
 *    L = mix(log(fGainmapRatioMin), log(fGainmapRatioMax), pow(G, fGainmapGamma))
 *
 *  Finally, apply the gainmap to compute D, the displayed pixel. If the base image is SDR then
 *  compute:
 *    D = (B + fEpsilonSdr) * exp(L * W) - fEpsilonHdr
 *  If the base image is HDR then compute:
 *    D = (B + fEpsilonHdr) * exp(L * (W - 1)) - fEpsilonSdr
 *
 *  In the above math, log() is a natural logarithm and exp() is natural exponentiation. Note,
 *  however, that the base used for the log() and exp() functions does not affect the results of
 *  the computation (it cancels out, as long as the same base is used throughout).
 *
 *  This product includes Gain Map technology under license by Adobe.
 */
struct SkGainmapInfo {
    SkColor4f fGainmapRatioMin = {1.f, 1.f, 1.f, 1.0};
    SkColor4f fGainmapRatioMax = {2.f, 2.f, 2.f, 1.0};
    SkColor4f fGainmapGamma = {1.f, 1.f, 1.f, 1.f};

    SkColor4f fEpsilonSdr = {0.f, 0.f, 0.f, 1.0};
    SkColor4f fEpsilonHdr = {0.f, 0.f, 0.f, 1.0};

    float fDisplayRatioSdr = 1.f;
    float fDisplayRatioHdr = 2.f;

    enum class BaseImageType {
        kSDR,
        kHDR,
    };
    BaseImageType fBaseImageType = BaseImageType::kSDR;

    enum class Type {
        kDefault,
        kApple,
    };
    Type fType = Type::kDefault;

    sk_sp<SkColorSpace> fGainmapMathColorSpace = nullptr;

    bool isUltraHDRv1Compatible() const;

    static bool ParseVersion(const SkData* data);

    static bool Parse(const SkData* data, SkGainmapInfo& info);

    static sk_sp<SkData> SerializeVersion();

    sk_sp<SkData> serialize() const;

    inline bool operator==(const SkGainmapInfo& other) const {
        return fGainmapRatioMin == other.fGainmapRatioMin &&
               fGainmapRatioMax == other.fGainmapRatioMax && fGainmapGamma == other.fGainmapGamma &&
               fEpsilonSdr == other.fEpsilonSdr && fEpsilonHdr == other.fEpsilonHdr &&
               fDisplayRatioSdr == other.fDisplayRatioSdr &&
               fDisplayRatioHdr == other.fDisplayRatioHdr &&
               fBaseImageType == other.fBaseImageType && fType == other.fType &&
               SkColorSpace::Equals(fGainmapMathColorSpace.get(),
                                    other.fGainmapMathColorSpace.get());
    }
    inline bool operator!=(const SkGainmapInfo& other) const { return !(*this == other); }
};

#endif
