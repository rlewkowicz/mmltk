// SPDX-License-Identifier: MIT
// Öztireli/Gross (2015) perceptual downscaling; provenance in perceptual_downscale_math.h.
#include "src/backend/imaging/resample/detail/perceptual_downscale.h"
#include "src/backend/imaging/resample/detail/perceptual_downscale_views.h"
#include "src/common/math/checked_arithmetic.h"
#include <algorithm>
#include <cstring>
#include <immintrin.h>
#include <new>
namespace mmltk::backend::imaging::resample::perceptual {
namespace {
void vector_add(__m256 value, __m256& sum, __m256& error) {
    const auto adjusted = _mm256_sub_ps(value, error), next = _mm256_add_ps(sum, adjusted);
    error = _mm256_sub_ps(_mm256_sub_ps(next, sum), adjusted);
    sum = next;
}
// Eight neighboring output cells traverse their contiguous integer source
// rectangle together. Even 2x2 reductions use eight active SIMD lanes. Byte
// gathers never load a fourth byte beyond an RGB pixel or cross row padding.
template <RgbPixelFormat Format>
void integer_moments8(RgbConstImageView source, const TransferTable& transfer, std::uint32_t first_x, Footprint fy, std::uint32_t step, Moment* output,
                      float* alpha_output) {
    __m256 means[3]{}, variances[3]{}, mean_errors[3]{}, variance_errors[3]{};
    __m256 alpha_sum = _mm256_setzero_ps(), alpha_error = _mm256_setzero_ps();
    std::uint64_t samples = 0;
    for (std::uint32_t y = fy.first; y < fy.end; ++y)
        for (std::uint32_t dx = 0; dx < step; ++dx) {
            __m256 rgb[3];
            __m256 alpha = _mm256_set1_ps(1);
            const auto* row = static_cast<const std::uint8_t*>(source.data) + std::size_t(y) * source.layout.row_stride_bytes;
            constexpr unsigned channels = Format == RgbPixelFormat::RGBA8 ? 4 : 3;
            for (unsigned k = 0; k < 3; ++k) {
                if constexpr (Format == RgbPixelFormat::PlanarUnitSrgbF32) {
                    alignas(32) float linear[8];
                    const auto* plane = reinterpret_cast<const float*>(row + k * source.layout.plane_stride_bytes);
                    for (unsigned lane = 0; lane < 8; ++lane) linear[lane] = decode(unit(plane[first_x + lane * step + dx]));
                    rgb[k] = _mm256_load_ps(linear);
                } else {
                    alignas(32) int indices[8];
                    for (unsigned lane = 0; lane < 8; ++lane) indices[lane] = row[std::size_t(first_x + lane * step + dx) * channels + k];
                    rgb[k] = _mm256_i32gather_ps(transfer.linear, _mm256_load_si256(reinterpret_cast<const __m256i*>(indices)), 4);
                }
            }
            if constexpr (Format == RgbPixelFormat::RGBA8) {
                alignas(32) int values[8];
                for (unsigned lane = 0; lane < 8; ++lane) values[lane] = row[std::size_t(first_x + lane * step + dx) * 4 + 3];
                alpha = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_load_si256(reinterpret_cast<const __m256i*>(values))), _mm256_set1_ps(1.0F / 255));
                for (auto& channel : rgb) channel = _mm256_mul_ps(channel, alpha);
                vector_add(alpha, alpha_sum, alpha_error);
            }
            using C = ColorCoefficients;
            const auto channel = [&](float r, float g, float b) {
                return _mm256_fmadd_ps(rgb[0], _mm256_set1_ps(r), _mm256_fmadd_ps(rgb[1], _mm256_set1_ps(g), _mm256_mul_ps(rgb[2], _mm256_set1_ps(b))));
            };
            const __m256 colors[]{channel(C::yr, C::yg, C::yb), channel(C::cbr, C::cbg, C::cbb), channel(C::crr, C::crg, C::crb)};
            const auto inverse = _mm256_set1_ps(1.0F / static_cast<float>(++samples));
            for (int k = 0; k < 3; ++k) {
                const auto delta = _mm256_sub_ps(colors[k], means[k]);
                vector_add(_mm256_mul_ps(delta, inverse), means[k], mean_errors[k]);
                vector_add(_mm256_mul_ps(delta, _mm256_sub_ps(colors[k], means[k])), variances[k], variance_errors[k]);
            }
        }
    const auto inverse = _mm256_set1_ps(1.0F / static_cast<float>(samples));
    alignas(32) float means_out[8], variances_out[8];
    for (int k = 0; k < 3; ++k) {
        _mm256_store_ps(means_out, means[k]);
        _mm256_store_ps(variances_out, _mm256_max_ps(_mm256_setzero_ps(), _mm256_mul_ps(_mm256_sub_ps(variances[k], variance_errors[k]), inverse)));
        for (unsigned lane = 0; lane < 8; ++lane) {
            output[lane].mean[k] = means_out[lane];
            output[lane].variance[k] = variances_out[lane];
        }
    }
    if constexpr (Format == RgbPixelFormat::RGBA8) _mm256_storeu_ps(alpha_output, _mm256_mul_ps(_mm256_sub_ps(alpha_sum, alpha_error), inverse));
}
}  // namespace
void copy_identity(RgbConstImageView source, RgbMutableImageView destination) {
    if (source.data == destination.data) return;
    const auto geometry = identity_geometry(source.layout);
    for (unsigned plane = 0; plane < geometry.planes; ++plane)
        for (std::uint32_t y = 0; y < source.layout.height; ++y)
            std::memcpy(static_cast<std::uint8_t*>(destination.data) + plane * destination.layout.plane_stride_bytes + y * destination.layout.row_stride_bytes,
                        static_cast<const std::uint8_t*>(source.data) + plane * source.layout.plane_stride_bytes + y * source.layout.row_stride_bytes,
                        geometry.row_bytes);
}
CpuDownscaler::CpuDownscaler() {
    for (unsigned i = 0; i < 256; ++i) transfer_.linear[i] = decode(float(i) * (1.0F / 255.0F));
}
void CpuDownscaler::preparation_checkpoint(PreparationStep step) {
    if (fail_before_ == step) {
        fail_before_ = PreparationStep::None;
        throw std::bad_alloc();
    }
}
void CpuDownscaler::prepare(const RgbImageLayout& source, const RgbImageLayout& destination) {
    const auto width = destination.width, height = destination.height;
    (void)common::math::checked_multiply<std::size_t>(width, sizeof(Moment) * 2 + sizeof(Coefficient) * 2 + sizeof(float) * 2,
                                                      "perceptual image extent overflow");
    const bool geometry_changed = !prepared_ || source_width_ != source.width || source_height_ != source.height || width_ != width || height_ != height;
    const bool alpha_needed = source.format == RgbPixelFormat::RGBA8 && (alpha_[0].size() != width || alpha_[1].size() != width);
    if (!geometry_changed && !alpha_needed) return;
    // In-place preparation retains useful capacity. Any exception after this
    // point must force complete axis/row preparation, even for the old key.
    prepared_ = false;
    if (geometry_changed) {
        preparation_checkpoint(PreparationStep::HorizontalAxis);
        x_.resize(width);
        preparation_checkpoint(PreparationStep::VerticalAxis);
        y_.resize(height);
        for (std::uint32_t x = 0; x < width; ++x) x_[x] = footprint(source.width, width, x);
        for (std::uint32_t y = 0; y < height; ++y) y_[y] = footprint(source.height, height, y);
        for (unsigned row = 0; row < 2; ++row) {
            preparation_checkpoint(row == 0 ? PreparationStep::FirstMoment : PreparationStep::SecondMoment);
            moments_[row].resize(width);
        }
        for (unsigned row = 0; row < 2; ++row) {
            preparation_checkpoint(row == 0 ? PreparationStep::FirstCoefficient : PreparationStep::SecondCoefficient);
            coefficients_[row].resize(width);
        }
    }
    if (source.format == RgbPixelFormat::RGBA8) {
        for (unsigned row = 0; row < 2; ++row) {
            preparation_checkpoint(row == 0 ? PreparationStep::FirstAlpha : PreparationStep::SecondAlpha);
            alpha_[row].resize(width);
        }
    }
    source_width_ = source.width;
    source_height_ = source.height;
    width_ = width;
    height_ = height;
    prepared_ = true;
}
template <RgbPixelFormat Format, bool Integer>
void CpuDownscaler::execute(RgbConstImageView source, RgbMutableImageView destination) {
    const auto width = destination.layout.width, height = destination.layout.height;
    auto fill_row = [&](std::uint32_t y) {
        const Footprint fy = y_[y];
        auto& row = moments_[y % 2];
        std::uint32_t x = 0;
        if constexpr (Integer) {
            for (; width - x >= 8; x += 8) {
                float* alpha = nullptr;
                if constexpr (Format == RgbPixelFormat::RGBA8) alpha = alpha_[y % 2].data() + x;
                integer_moments8<Format>(source, transfer_, x_[x].first, fy, source.layout.width / width, row.data() + x, alpha);
            }
        }
        for (; x < width; ++x) {
            const Footprint fx = x_[x];
            MomentAccumulator sum;
            float coverage = 0, coverage_error = 0;
            for (std::uint32_t sy = fy.first; sy < fy.end; ++sy) {
                const float wy = Integer ? 1.0F : fy.weight(sy);
                for (std::uint32_t sx = fx.first; sx < fx.end; ++sx) {
                    const float weight = Integer ? 1.0F : wy * fx.weight(sx);
                    float alpha = 1.0F;
                    const Color color = load<Format>(source, sx, sy, transfer_, alpha);
                    sum.add(color, weight);
                    if constexpr (Format == RgbPixelFormat::RGBA8) compensated_add(weight * alpha, coverage, coverage_error);
                }
            }
            row[x] = sum.finish();
            if constexpr (Format == RgbPixelFormat::RGBA8) alpha_[y % 2][x] = unit((coverage - coverage_error) / sum.weight);
        }
    };
    fill_row(0);
    for (std::uint32_t y = 0; y < height; ++y) {
        if (y + 1 < height) fill_row(y + 1);
        const auto& row = moments_[y % 2];
        const auto& next = moments_[(y + 1 < height ? y + 1 : y) % 2];
        auto& current = coefficients_[y % 2];
        for (std::uint32_t x = 0; x + 1 < width; ++x) current[x] = patch(row[x], row[x + 1], next[x], next[x + 1]);
        current[width - 1] = patch(row[width - 1], row[width - 1], next[width - 1], next[width - 1]);
        const auto& previous = coefficients_[(y ? y - 1 : y) % 2];
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto left = x ? x - 1 : 0;
            float alpha = 1;
            if constexpr (Format == RgbPixelFormat::RGBA8) alpha = alpha_[y % 2][x];
            store<Format>(destination, x, y, reconstruct(row[x], current[x], current[left], previous[x], previous[left]), alpha, transfer_);
        }
    }
}
void CpuDownscaler::run(RgbConstImageView source, RgbMutableImageView destination) {
    if (validate_pair(source, destination)) {
        copy_identity(source, destination);
        return;
    }
    prepare(source.layout, destination.layout);
    const bool integer = source.layout.width % destination.layout.width == 0 && source.layout.height % destination.layout.height == 0;
    // One format/geometry dispatch per image, never per source pixel.
    switch (source.layout.format) {
        case RgbPixelFormat::RGB8:
            if (integer)
                execute<RgbPixelFormat::RGB8, true>(source, destination);
            else
                execute<RgbPixelFormat::RGB8, false>(source, destination);
            break;
        case RgbPixelFormat::RGBA8:
            if (integer)
                execute<RgbPixelFormat::RGBA8, true>(source, destination);
            else
                execute<RgbPixelFormat::RGBA8, false>(source, destination);
            break;
        case RgbPixelFormat::PlanarUnitSrgbF32:
            if (integer)
                execute<RgbPixelFormat::PlanarUnitSrgbF32, true>(source, destination);
            else
                execute<RgbPixelFormat::PlanarUnitSrgbF32, false>(source, destination);
            break;
    }
}
}  // namespace mmltk::backend::imaging::resample::perceptual
