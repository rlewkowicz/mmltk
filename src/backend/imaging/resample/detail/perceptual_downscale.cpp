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
// Mask both parts of the compensated state: adding zero can still consume a
// previous error and is not a no-op for a completed lane.
template <bool Masked = false>
void vector_add(__m256 value, __m256& sum, __m256& error, __m256 active = {}) {
 const auto adjusted = _mm256_sub_ps(value, error), next = _mm256_add_ps(sum, adjusted);
 const auto next_error = _mm256_sub_ps(_mm256_sub_ps(next, sum), adjusted);
 if constexpr (Masked) {
  error = _mm256_blendv_ps(error, next_error, active);
  sum = _mm256_blendv_ps(sum, next, active);
 } else {
  error = next_error;
  sum = next;
 }
}
// Lane addresses are valid even for inactive lanes. Packed RGB reads only its
// three bytes; neither a fourth byte nor row padding is used by a gather.
template <RgbPixelFormat Format, class SourceX>
void load_colors8(RgbConstImageView source, const TransferTable& transfer, std::uint32_t y, SourceX source_x, __m256 (&colors)[3], __m256& alpha) {
 __m256 rgb[3];
 const auto* row = static_cast<const std::uint8_t*>(source.data) + std::size_t(y) * source.layout.row_stride_bytes;
 constexpr unsigned channels = Format == RgbPixelFormat::RGBA8 ? 4 : 3;
 for (unsigned k = 0; k < 3; ++k) {
  if constexpr (Format == RgbPixelFormat::PlanarUnitSrgbF32) {
   alignas(32) float linear[8];
   const auto* plane = reinterpret_cast<const float*>(row + k * source.layout.plane_stride_bytes);
   for (unsigned lane = 0; lane < 8; ++lane) linear[lane] = decode(unit(plane[source_x(lane)]));
   rgb[k] = _mm256_load_ps(linear);
  } else {
   alignas(32) int indices[8];
   for (unsigned lane = 0; lane < 8; ++lane) indices[lane] = row[std::size_t(source_x(lane)) * channels + k];
   rgb[k] = _mm256_i32gather_ps(transfer.linear, _mm256_load_si256(reinterpret_cast<const __m256i*>(indices)), 4);
  }
 }
 if constexpr (Format == RgbPixelFormat::RGBA8) {
  alignas(32) int values[8];
  for (unsigned lane = 0; lane < 8; ++lane) values[lane] = row[std::size_t(source_x(lane)) * 4 + 3];
  alpha = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_load_si256(reinterpret_cast<const __m256i*>(values))), _mm256_set1_ps(1.0F / 255));
  for (auto& channel : rgb) channel = _mm256_mul_ps(channel, alpha);
 }
 using C = ColorCoefficients;
 const auto channel = [&](float r, float g, float b) { return _mm256_fmadd_ps(rgb[0], _mm256_set1_ps(r), _mm256_fmadd_ps(rgb[1], _mm256_set1_ps(g), _mm256_mul_ps(rgb[2], _mm256_set1_ps(b)))); };
 colors[0] = channel(C::yr, C::yg, C::yb);
 colors[1] = channel(C::cbr, C::cbg, C::cbb);
 colors[2] = channel(C::crr, C::crg, C::crb);
}
// Eight neighboring cells traverse each footprint in scalar row-major order.
// Integer specialization retains uniform addressing and one scalar reciprocal;
// fractional lanes retain MomentAccumulator's weighted arithmetic and masking.
template <RgbPixelFormat Format, bool Integer>
void moments8(RgbConstImageView source, const TransferTable& transfer, const Footprint* fx, Footprint fy, Moment* output, float* alpha_output) {
 __m256 means[3]{}, variances[3]{}, mean_errors[3]{}, variance_errors[3]{};
 __m256 alpha_sum{}, alpha_error{}, weight{}, weight_error{};
 std::uint64_t samples = 0;
 std::uint32_t span = fx[0].end - fx[0].first;
 if constexpr (!Integer)
  for (unsigned lane = 1; lane < 8; ++lane) span = std::max(span, fx[lane].end - fx[lane].first);
 for (std::uint32_t y = fy.first; y < fy.end; ++y)
  for (std::uint32_t dx = 0; dx < span; ++dx) {
   __m256 sample_weight{}, active{}, fraction{}, colors[3], alpha{};
   std::uint32_t positions[8];
   if constexpr (Integer) {
    fraction = _mm256_set1_ps(1.0F / static_cast<float>(++samples));
    load_colors8<Format>(source, transfer, y, [&](unsigned lane) { return fx[0].first + lane * span + dx; }, colors, alpha);
   } else {
    alignas(32) float weights[8];
    const float wy = fy.weight(y);
    for (unsigned lane = 0; lane < 8; ++lane) {
     const bool present = dx < fx[lane].end - fx[lane].first;
     positions[lane] = fx[lane].first + (present ? dx : 0);
     weights[lane] = present ? wy * fx[lane].weight(positions[lane]) : 0;
    }
    sample_weight = _mm256_load_ps(weights);
    active = _mm256_cmp_ps(sample_weight, _mm256_setzero_ps(), _CMP_GT_OQ);
    vector_add<true>(sample_weight, weight, weight_error, active);
    // Never divide by zero while evaluating a lane that will be discarded.
    fraction = _mm256_div_ps(sample_weight, _mm256_blendv_ps(_mm256_set1_ps(1), weight, active));
    load_colors8<Format>(source, transfer, y, [&](unsigned lane) { return positions[lane]; }, colors, alpha);
   }
   if constexpr (Format == RgbPixelFormat::RGBA8) {
    if constexpr (!Integer) alpha = _mm256_mul_ps(sample_weight, alpha);
    vector_add<!Integer>(alpha, alpha_sum, alpha_error, active);
   }
   for (int k = 0; k < 3; ++k) {
    const auto delta = _mm256_sub_ps(colors[k], means[k]);
    vector_add<!Integer>(_mm256_mul_ps(delta, fraction), means[k], mean_errors[k], active);
    auto weighted_delta = delta;
    if constexpr (!Integer) weighted_delta = _mm256_mul_ps(sample_weight, delta);
    vector_add<!Integer>(_mm256_mul_ps(weighted_delta, _mm256_sub_ps(colors[k], means[k])), variances[k], variance_errors[k], active);
   }
  }
 const auto normalize = [&](__m256 value) {
  if constexpr (Integer)
   return _mm256_mul_ps(value, _mm256_set1_ps(1.0F / static_cast<float>(samples)));
  else
   return _mm256_div_ps(value, weight);
 };
 alignas(32) float means_out[8], variances_out[8];
 for (int k = 0; k < 3; ++k) {
  _mm256_store_ps(means_out, means[k]);
  _mm256_store_ps(variances_out, _mm256_max_ps(_mm256_setzero_ps(), normalize(_mm256_sub_ps(variances[k], variance_errors[k]))));
  for (unsigned lane = 0; lane < 8; ++lane) {
   output[lane].mean[k] = means_out[lane];
   output[lane].variance[k] = variances_out[lane];
  }
 }
 if constexpr (Format == RgbPixelFormat::RGBA8) _mm256_storeu_ps(alpha_output, _mm256_min_ps(_mm256_set1_ps(1), _mm256_max_ps(_mm256_setzero_ps(), normalize(_mm256_sub_ps(alpha_sum, alpha_error)))));
}
}  // namespace
void copy_identity(RgbConstImageView source, RgbMutableImageView destination) {
 if (source.data == destination.data) return;
 const auto geometry = identity_geometry(source.layout);
 for (unsigned plane = 0; plane < geometry.planes; ++plane)
  for (std::uint32_t y = 0; y < source.layout.height; ++y)
   std::memcpy(static_cast<std::uint8_t*>(destination.data) + plane * destination.layout.plane_stride_bytes + y * destination.layout.row_stride_bytes,
    static_cast<const std::uint8_t*>(source.data) + plane * source.layout.plane_stride_bytes + y * source.layout.row_stride_bytes, geometry.row_bytes);
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
 (void)common::math::checked_multiply<std::size_t>(width, sizeof(Moment) * 2 + sizeof(Coefficient) * 2 + sizeof(float) * 2, "perceptual image extent overflow");
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
template <RgbPixelFormat Format, bool Integer, bool QuantizedPlanar>
void CpuDownscaler::execute(RgbConstImageView source, RgbMutableImageView destination) {
 const auto width = destination.layout.width, height = destination.layout.height;
 auto fill_row = [&](std::uint32_t y) {
  const Footprint fy = y_[y];
  auto& row = moments_[y % 2];
  std::uint32_t x = 0;
  for (; width - x >= 8; x += 8) {
   float* alpha = nullptr;
   if constexpr (Format == RgbPixelFormat::RGBA8) alpha = alpha_[y % 2].data() + x;
   moments8<Format, Integer>(source, transfer_, x_.data() + x, fy, row.data() + x, alpha);
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
   store<Format, QuantizedPlanar>(destination, x, y, reconstruct(row[x], current[x], current[left], previous[x], previous[left]), alpha, transfer_);
  }
 }
}
void CpuDownscaler::run_quantized_planar(RgbConstImageView source, RgbMutableImageView destination) {
 (void)validate_pair(source, destination, true);
 prepare(source.layout, destination.layout);
 if (source.layout.width % destination.layout.width == 0 && source.layout.height % destination.layout.height == 0)
  execute<RgbPixelFormat::RGB8, true, true>(source, destination);
 else
  execute<RgbPixelFormat::RGB8, false, true>(source, destination);
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
