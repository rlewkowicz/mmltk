// SPDX-License-Identifier: MIT
// Independent scalar specification of Öztireli/Gross (2015), derived from the
// supplied MIT ssim_perceptual_downscaling.c. Uses direct footprint intersection
// and direct four-patch reconstruction, not production tables or math helpers.
#pragma once
#include "src/backend/imaging/resample/image_resize.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>
namespace mmltk::backend::imaging::resample::test_perceptual {
// Near the variance cutoff, contrast gains can approach 500. Rounding the
// retained FP32 moments alone can exceed 2e-6 even with exact accumulation and
// reconstruction. This unit-sRGB allowance is about 0.005 of an 8-bit code;
// threshold-branch and transfer-function tests retain their tighter bounds.
inline double reference_tolerance(RgbPixelFormat format) {
    return format == RgbPixelFormat::PlanarUnitSrgbF32 ? 2e-5 : 1.0 / 255 + 1e-12;
}
struct Image {
    RgbImageLayout layout;
    std::vector<float> storage;
    Image(std::uint32_t width, std::uint32_t height, RgbPixelFormat format, std::size_t padding = 0) {
        const bool planar = format == RgbPixelFormat::PlanarUnitSrgbF32;
        const std::size_t channels = format == RgbPixelFormat::RGB8 ? 3 : 4;
        const std::size_t row = ((width * channels + padding + 3) / 4) * 4;
        const std::size_t plane = planar ? row * height + 4 * padding : 0;
        layout = {width, height, row, plane, planar ? 3 * plane : row * height, format};
        storage.resize((layout.capacity_bytes + 3) / 4);
        std::memset(storage.data(), 0xCD, storage.size() * sizeof(float));
    }
    RgbConstImageView read() const { return {storage.data(), layout}; }
    RgbMutableImageView write() { return {storage.data(), layout}; }
    double at(unsigned x, unsigned y, unsigned channel) const {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(storage.data()) + y * layout.row_stride_bytes;
        if (layout.format == RgbPixelFormat::PlanarUnitSrgbF32) return reinterpret_cast<const float*>(bytes + channel * layout.plane_stride_bytes)[x];
        return bytes[x * (layout.format == RgbPixelFormat::RGB8 ? 3 : 4) + channel] / 255.0;
    }
    void set(unsigned x, unsigned y, unsigned channel, double value) {
        auto* bytes = reinterpret_cast<std::uint8_t*>(storage.data()) + y * layout.row_stride_bytes;
        if (layout.format == RgbPixelFormat::PlanarUnitSrgbF32)
            reinterpret_cast<float*>(bytes + channel * layout.plane_stride_bytes)[x] = static_cast<float>(value);
        else
            bytes[x * (layout.format == RgbPixelFormat::RGB8 ? 3 : 4) + channel] = static_cast<std::uint8_t>(std::clamp(value, 0.0, 1.0) * 255.0 + 0.5);
    }
    void fill(unsigned pattern) {
        const unsigned channels = layout.format == RgbPixelFormat::RGBA8 ? 4 : 3;
        for (unsigned y = 0; y < layout.height; ++y)
            for (unsigned x = 0; x < layout.width; ++x)
                for (unsigned k = 0; k < channels; ++k) {
                    double value = 0;
                    switch (pattern) {
                        case 0: value = 0.15 + 0.3 * k; break;
                        case 1: value = double(x + y + k) / double(layout.width + layout.height + 3); break;
                        case 2: value = (x == layout.width / 2 && y == layout.height / 2) ? 1 : 0; break;
                        case 3: value = (x + y + k) % 2; break;
                        case 4: value = (x == layout.width - 1 || y == 0) ? 1 : 0; break;
                        case 6: {
                            constexpr double transition[]{0, 0.040449, 0.04045, 0.040451, 1};
                            value = transition[(x + y + k) % 5];
                            break;
                        }
                        case 7: value = k == 3 ? ((x + y) % 3 == 0 ? 1 : 0) : ((x + y + k) % 2); break;
                        default: value = ((x * 37U + y * 107U + k * 19U + x * y * 31U) % 256) / 255.0; break;
                    }
                    set(x, y, k, value);
                }
    }
};
inline double eotf(double value) {
    value = !(value > 0) ? 0 : std::min(value, 1.0);
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}
inline double oetf(double value) {
    value = std::clamp(value, 0.0, 1.0);
    return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}
inline std::array<double, 3> ycc(const Image& source, unsigned x, unsigned y) {
    const double a = source.layout.format == RgbPixelFormat::RGBA8 ? source.at(x, y, 3) : 1;
    const double r = eotf(source.at(x, y, 0)) * a, g = eotf(source.at(x, y, 1)) * a, b = eotf(source.at(x, y, 2)) * a;
    return {0.299 * r + 0.587 * g + 0.114 * b, -0.168736 * r - 0.331264 * g + 0.5 * b + 0.5, 0.5 * r - 0.418688 * g - 0.081312 * b + 0.5};
}
inline Image reference(const Image& source, unsigned width, unsigned height, std::size_t padding = 0) {
    Image result(width, height, source.layout.format, padding);
    if (width == source.layout.width && height == source.layout.height) {
        const unsigned channels = source.layout.format == RgbPixelFormat::RGBA8 ? 4 : 3;
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x)
                for (unsigned k = 0; k < channels; ++k) result.set(x, y, k, source.at(x, y, k));
        return result;
    }
    const auto count = std::size_t(width) * height;
    std::vector<std::array<double, 3>> mean(count), second(count), patch_mean(count), ratio(count);
    std::vector<double> coverage(count);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const auto i = std::size_t(y) * width + x;
            const double x0 = double(x) * source.layout.width / width, x1 = double(x + 1) * source.layout.width / width;
            const double y0 = double(y) * source.layout.height / height, y1 = double(y + 1) * source.layout.height / height;
            const double area = (x1 - x0) * (y1 - y0);
            for (unsigned sy = static_cast<unsigned>(std::floor(y0)); sy < std::ceil(y1); ++sy)
                for (unsigned sx = static_cast<unsigned>(std::floor(x0)); sx < std::ceil(x1); ++sx) {
                    const double weight =
                        (std::min(x1, double(sx + 1)) - std::max(x0, double(sx))) * (std::min(y1, double(sy + 1)) - std::max(y0, double(sy))) / area;
                    const auto color = ycc(source, sx, sy);
                    for (unsigned k = 0; k < 3; ++k) {
                        mean[i][k] += color[k] * weight;
                        second[i][k] += color[k] * color[k] * weight;
                    }
                    coverage[i] += weight * (source.layout.format == RgbPixelFormat::RGBA8 ? source.at(sx, sy, 3) : 1);
                }
        }
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const auto i = std::size_t(y) * width + x;
            for (unsigned k = 0; k < 3; ++k) {
                double m = 0, lo = 0, hi = 0;
                for (unsigned dy = 0; dy < 2; ++dy)
                    for (unsigned dx = 0; dx < 2; ++dx) {
                        const auto j = std::size_t(std::min(y + dy, height - 1)) * width + std::min(x + dx, width - 1);
                        m += mean[j][k] / 4;
                        lo += mean[j][k] * mean[j][k] / 4;
                        hi += second[j][k] / 4;
                    }
                patch_mean[i][k] = m;
                const double variance = std::max(0.0, lo - m * m);
                ratio[i][k] = variance >= 0.000001 ? std::sqrt(std::max(0.0, hi - m * m) / variance) : 2.0;
            }
        }
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const auto i = std::size_t(y) * width + x;
            std::array<double, 3> out{};
            for (unsigned k = 0; k < 3; ++k)
                for (unsigned dy = 0; dy < 2; ++dy)
                    for (unsigned dx = 0; dx < 2; ++dx) {
                        const auto j = std::size_t(y >= dy ? y - dy : 0) * width + (x >= dx ? x - dx : 0);
                        out[k] += (patch_mean[j][k] + ratio[j][k] * (mean[i][k] - patch_mean[j][k])) / 4;
                    }
            const std::array<double, 3> rgb{out[0] + 1.402 * (out[2] - 0.5), out[0] - 0.344136 * (out[1] - 0.5) - 0.714136 * (out[2] - 0.5),
                                            out[0] + 1.772 * (out[1] - 0.5)};
            for (unsigned k = 0; k < 3; ++k) {
                const double linear = coverage[i] > 0 ? std::clamp(rgb[k] / coverage[i], 0.0, 1.0) : 0;
                double encoded = oetf(linear);
                if (source.layout.format != RgbPixelFormat::PlanarUnitSrgbF32) encoded = std::floor(encoded * 255) / 255;
                result.set(x, y, k, encoded);
            }
            if (source.layout.format == RgbPixelFormat::RGBA8) result.set(x, y, 3, coverage[i]);
        }
    return result;
}
inline double maximum_error(const Image& a, const Image& b) {
    double error = 0;
    const unsigned channels = a.layout.format == RgbPixelFormat::RGBA8 ? 4 : 3;
    for (unsigned y = 0; y < a.layout.height; ++y)
        for (unsigned x = 0; x < a.layout.width; ++x)
            for (unsigned k = 0; k < channels; ++k) {
                const double difference = std::abs(a.at(x, y, k) - b.at(x, y, k));
                if (!std::isfinite(difference)) return std::numeric_limits<double>::infinity();
                error = std::max(error, difference);
            }
    return error;
}
inline bool padding_intact(const Image& image) {
    const auto& l = image.layout;
    const bool planar = l.format == RgbPixelFormat::PlanarUnitSrgbF32;
    const auto row_bytes = l.width * (l.format == RgbPixelFormat::RGB8 ? 3 : 4);
    const auto* data = reinterpret_cast<const std::uint8_t*>(image.storage.data());
    for (std::size_t offset = 0; offset < l.capacity_bytes; ++offset) {
        const auto within = planar ? offset % l.plane_stride_bytes : offset;
        if (within / l.row_stride_bytes < l.height && within % l.row_stride_bytes < row_bytes) continue;
        if (data[offset] != 0xCD) return false;
    }
    return true;
}
inline Image threshold_source(unsigned width, unsigned height, double delta) {
    Image source(width, height, RgbPixelFormat::PlanarUnitSrgbF32, 3);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            for (unsigned k = 0; k < 3; ++k) source.set(x, y, k, oetf(0.3 + ((x / 2) % 2 ? delta : 0) + (x % 2 ? 0.01 : -0.01)));
    return source;
}
inline constexpr std::array<std::array<unsigned, 4>, 16> geometries{{{17, 13, 9, 7},
                                                                     {15, 9, 5, 3},
                                                                     {16, 12, 8, 6},
                                                                     {23, 11, 23, 4},
                                                                     {13, 19, 3, 19},
                                                                     {1, 23, 1, 7},
                                                                     {19, 1, 5, 1},
                                                                     {1, 1, 1, 1},
                                                                     {17, 13, 1, 1},
                                                                     {35, 17, 17, 8},
                                                                     {129, 127, 2, 3},
                                                                     {7, 5, 7, 5},
                                                                     {34, 6, 17, 3},
                                                                     {32, 6, 16, 3},
                                                                     {65537, 1, 2, 1},
                                                                     {8193, 3, 17, 1}}};
inline constexpr std::array<RgbPixelFormat, 3> formats{RgbPixelFormat::RGB8, RgbPixelFormat::RGBA8, RgbPixelFormat::PlanarUnitSrgbF32};
}  // namespace mmltk::backend::imaging::resample::test_perceptual
