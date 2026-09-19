// SPDX-License-Identifier: MIT
// Derived from the supplied MIT ssim_perceptual_downscaling.c, implementing
// A. Cengiz Öztireli and Markus Gross, "Perceptually Based Downscaling of
// Images" (2015), https://www.cl.cam.ac.uk/~aco41/Files/Sig15PerceptualDownscaling.pdf
// Transfer functions in that source credit https://github.com/tobspr/GLSL-Color-Spaces/.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "src/backend/imaging/resample/image_resize.h"
#if defined(__CUDACC__)
#define MMLTK_PERCEPTUAL_HD __host__ __device__
#else
#define MMLTK_PERCEPTUAL_HD
#endif
namespace mmltk::backend::imaging::resample::perceptual {
inline constexpr float variance_threshold = 1e-6F;
inline constexpr float low_variance_ratio = 2.0F;
MMLTK_PERCEPTUAL_HD inline float unit(float value) { return !(value > 0.0F) ? 0.0F : (value < 1.0F ? value : 1.0F); }
MMLTK_PERCEPTUAL_HD inline float decode(float v) { return v > 0.04045F ? ::powf((v + 0.055F) / 1.055F, 2.4F) : v / 12.92F; }
MMLTK_PERCEPTUAL_HD inline float encode(float v) { return v > 0.0031308F ? 1.055F * ::powf(v, 1.0F / 2.4F) - 0.055F : 12.92F * v; }
struct Color {
    float y, cb, cr;
};
struct ColorCoefficients {
    static constexpr float yr = 0.299F, yg = 0.587F, yb = 0.114F;
    static constexpr float cbr = -0.168736F, cbg = -0.331264F, cbb = 0.5F;
    static constexpr float crr = 0.5F, crg = -0.418688F, crb = -0.081312F;
};
MMLTK_PERCEPTUAL_HD inline Color transform(float r, float g, float b) {
    using C = ColorCoefficients;
    // Store Cb/Cr relative to the reference's +0.5 origin. Translation
    // invariance of central moments removes that cancellation at dark colors.
    return {C::yr * r + C::yg * g + C::yb * b, C::cbr * r + C::cbg * g + C::cbb * b, C::crr * r + C::crg * g + C::crb * b};
}
MMLTK_PERCEPTUAL_HD inline Color inverse(Color v) { return {v.y + 1.402F * v.cr, v.y - 0.344136F * v.cb - 0.714136F * v.cr, v.y + 1.772F * v.cb}; }
// Integer rational endpoints avoid loss of coverage even at large uint32
// extents. Interior weights are exactly one; only the two borders are partial.
struct Footprint {
    std::uint32_t first, end;
    float first_weight, last_weight;
    MMLTK_PERCEPTUAL_HD float weight(std::uint32_t pixel) const {
        if (pixel == first) return first_weight;
        if (pixel + 1 == end) return last_weight;
        return 1.0F;
    }
};
MMLTK_PERCEPTUAL_HD inline Footprint footprint(std::uint32_t src, std::uint32_t dst, std::uint32_t i) {
    const std::uint64_t begin = std::uint64_t(i) * src, end = (std::uint64_t(i) + 1) * src;
    const auto first = static_cast<std::uint32_t>(begin / dst);
    const auto last = static_cast<std::uint32_t>((end - 1) / dst);
    return {first, last + 1, first == last ? float(double(end - begin) / dst) : float(double(dst - begin % dst) / dst),
            float(double(end - std::uint64_t(last) * dst) / dst)};
}
struct Moment {
    float mean[3], variance[3];
};
struct Coefficient {
    float ratio[3]{}, center[3]{};
};
static_assert(sizeof(Moment) == 24 && sizeof(Coefficient) == 24);
MMLTK_PERCEPTUAL_HD inline void compensated_add(float value, float& sum, float& error) {
    const float adjusted = value - error;
    const float next = sum + adjusted;
    error = (next - sum) - adjusted;
    sum = next;
}
// Weighted Welford updates keep central M2, never E[x*x]-E[x]*E[x].
// Compensation bounds drift in long per-lane runs. One FP32 reciprocal per
// sample is shared by all three channels; no source-sample FP64 is needed.
struct MomentAccumulator {
    Moment value{};
    float weight = 0, weight_error = 0, mean_error[3]{}, variance_error[3]{};
    MMLTK_PERCEPTUAL_HD void add(Color color, float sample_weight) {
        if (!(sample_weight > 0)) return;
        compensated_add(sample_weight, weight, weight_error);
        const float fraction = sample_weight / weight;
        const float channels[]{color.y, color.cb, color.cr};
        for (int k = 0; k < 3; ++k) {
            const float delta = channels[k] - value.mean[k];
            compensated_add(delta * fraction, value.mean[k], mean_error[k]);
            compensated_add(sample_weight * delta * (channels[k] - value.mean[k]), value.variance[k], variance_error[k]);
        }
    }
    MMLTK_PERCEPTUAL_HD Moment finish() const {
        Moment result = value;
        for (int k = 0; k < 3; ++k) result.variance[k] = weight > 0 ? ::fmaxf(0, (value.variance[k] - variance_error[k]) / weight) : 0;
        return result;
    }
};
// Law of total variance combines independently centered lane reductions.
MMLTK_PERCEPTUAL_HD inline Moment merge(Moment a, float aw, Moment b, float bw) {
    if (!(aw > 0)) return b;
    if (!(bw > 0)) return a;
    const float fraction = bw / (aw + bw), other = 1 - fraction;
    for (int k = 0; k < 3; ++k) {
        const float delta = b.mean[k] - a.mean[k];
        a.mean[k] = ::fmaf(delta, fraction, a.mean[k]);
        a.variance[k] = ::fmaf(delta * delta, fraction * other, a.variance[k] * other + b.variance[k] * fraction);
    }
    return a;
}
MMLTK_PERCEPTUAL_HD inline float contrast_ratio(float low, float within) {
    return low >= variance_threshold ? ::sqrtf(1.0F + ::fmaxf(0, within) / low) : low_variance_ratio;
}
MMLTK_PERCEPTUAL_HD inline Coefficient patch(const Moment& a, const Moment& b, const Moment& c, const Moment& d) {
    Coefficient result;
    for (int k = 0; k < 3; ++k) {
        const float db = b.mean[k] - a.mean[k], dc = c.mean[k] - a.mean[k], dd = d.mean[k] - a.mean[k];
        const float offset = (db + dc + dd) * 0.25F;
        const float rb = db - offset, rc = dc - offset, rd = dd - offset;
        const float low = (offset * offset + rb * rb + rc * rc + rd * rd) * 0.25F;
        const float within = (a.variance[k] + b.variance[k] + c.variance[k] + d.variance[k]) * 0.25F;
        result.ratio[k] = contrast_ratio(low, within);
        result.center[k] = a.mean[k] + offset;
    }
    return result;
}
MMLTK_PERCEPTUAL_HD inline Color reconstruct(const Moment& m, const Coefficient& a, const Coefficient& b, const Coefficient& c, const Coefficient& d) {
    float v[3];
    for (int k = 0; k < 3; ++k) {
        // Centered reconstruction avoids subtracting large r*m terms.
        v[k] = (::fmaf(a.ratio[k], m.mean[k] - a.center[k], a.center[k]) + ::fmaf(b.ratio[k], m.mean[k] - b.center[k], b.center[k]) +
                ::fmaf(c.ratio[k], m.mean[k] - c.center[k], c.center[k]) + ::fmaf(d.ratio[k], m.mean[k] - d.center[k], d.center[k])) *
               0.25F;
    }
    return {v[0], v[1], v[2]};
}
// The reference's 256 Float32 EOTF values also serve as monotonic quantization
// thresholds. Threshold rounding can move floor(255*OETF(v)) by one code;
// there is no output pow in the byte path. Float32 output uses the continuous
// transfer function and is never quantized through bytes.
struct TransferTable {
    float linear[256]{};
    MMLTK_PERCEPTUAL_HD std::uint8_t quantize(float value) const {
        value = unit(value);
        unsigned lo = 0, hi = 256;
        while (lo + 1 < hi) {
            const unsigned mid = (lo + hi) / 2;
            if (linear[mid] <= value)
                lo = mid;
            else
                hi = mid;
        }
        return static_cast<std::uint8_t>(lo);
    }
};
template <RgbPixelFormat Format>
MMLTK_PERCEPTUAL_HD inline Color load(RgbConstImageView view, std::uint32_t x, std::uint32_t y, const TransferTable& table, float& alpha) {
    const auto* row = static_cast<const std::uint8_t*>(view.data) + std::size_t(y) * view.layout.row_stride_bytes;
    float r, g, b;
    if constexpr (Format == RgbPixelFormat::PlanarUnitSrgbF32) {
        r = decode(unit(reinterpret_cast<const float*>(row)[x]));
        g = decode(unit(reinterpret_cast<const float*>(row + view.layout.plane_stride_bytes)[x]));
        b = decode(unit(reinterpret_cast<const float*>(row + 2 * view.layout.plane_stride_bytes)[x]));
    } else {
        constexpr unsigned channels = Format == RgbPixelFormat::RGBA8 ? 4 : 3;
        const auto* pixel = row + std::size_t(x) * channels;
        r = table.linear[pixel[0]];
        g = table.linear[pixel[1]];
        b = table.linear[pixel[2]];
        if constexpr (Format == RgbPixelFormat::RGBA8) {
            alpha = float(pixel[3]) / 255.0F;
            r *= alpha;
            g *= alpha;
            b *= alpha;
        }
    }
    return transform(r, g, b);
}
template <RgbPixelFormat Format, bool QuantizedPlanar = false>
MMLTK_PERCEPTUAL_HD inline void store(RgbMutableImageView view, std::uint32_t x, std::uint32_t y, Color value, float alpha, const TransferTable& table) {
    const Color rgb = inverse(value);
    float channels[3]{rgb.y, rgb.cb, rgb.cr};
    auto* row = static_cast<std::uint8_t*>(view.data) + std::size_t(y) * view.layout.row_stride_bytes;
    for (int k = 0; k < 3; ++k) {
        float linear = channels[k];
        if constexpr (Format == RgbPixelFormat::RGBA8)
            linear = alpha > 0.0F ? unit(linear / alpha) : 0.0F;
        else
            linear = unit(linear);
        if constexpr (QuantizedPlanar) {
            reinterpret_cast<float*>(row + std::size_t(k) * view.layout.plane_stride_bytes)[x] = float(table.quantize(linear)) * (1.0F / 255.0F);
        } else if constexpr (Format == RgbPixelFormat::PlanarUnitSrgbF32) {
            reinterpret_cast<float*>(row + std::size_t(k) * view.layout.plane_stride_bytes)[x] = static_cast<float>(encode(linear));
        } else {
            constexpr unsigned count = Format == RgbPixelFormat::RGBA8 ? 4 : 3;
            row[std::size_t(x) * count + k] = table.quantize(linear);
        }
    }
    if constexpr (Format == RgbPixelFormat::RGBA8) row[std::size_t(x) * 4 + 3] = static_cast<std::uint8_t>(::lroundf(unit(alpha) * 255.0F));
}
}  // namespace mmltk::backend::imaging::resample::perceptual
#undef MMLTK_PERCEPTUAL_HD
