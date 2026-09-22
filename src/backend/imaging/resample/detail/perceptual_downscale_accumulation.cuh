// SPDX-License-Identifier: MIT
#pragma once
#include "src/backend/imaging/resample/detail/perceptual_downscale_math.h"
#if defined(__CUDACC__)
namespace mmltk::backend::imaging::resample::perceptual {
template <RgbPixelFormat Format, bool Integer>
__device__ inline void accumulate_sample(
 RgbConstImageView source, const TransferTable& transfer, Footprint fx, Footprint fy, std::uint32_t x, std::uint32_t y, MomentAccumulator& sum, float& alpha, float& alpha_error) {
 const float weight = Integer ? 1.0F : fx.weight(x) * fy.weight(y);
 float coverage = 1;
 const Color color = load<Format>(source, x, y, transfer, coverage);
 sum.add(color, weight);
 if constexpr (Format == RgbPixelFormat::RGBA8) compensated_add(weight * coverage, alpha, alpha_error);
}
template <RgbPixelFormat Format, bool Integer>
__device__ inline void accumulate_strided(
 RgbConstImageView source, const TransferTable& transfer, Footprint fx, Footprint fy, std::size_t offset, std::size_t step, MomentAccumulator& sum, float& alpha) {
 const std::size_t width = std::size_t(fx.end) - fx.first;
 const std::size_t count = width * (std::size_t(fy.end) - fy.first);
 float alpha_error = 0;
 for (std::size_t j = offset; j < count; j += step) {
  const auto x = fx.first + static_cast<std::uint32_t>(j % width), y = fy.first + static_cast<std::uint32_t>(j / width);
  accumulate_sample<Format, Integer>(source, transfer, fx, fy, x, y, sum, alpha, alpha_error);
 }
 if constexpr (Format == RgbPixelFormat::RGBA8) alpha = sum.weight > 0 ? (alpha - alpha_error) / sum.weight : 0;
}
template <RgbPixelFormat Format, bool Integer>
__device__ inline void accumulate_serial(RgbConstImageView source, const TransferTable& transfer, Footprint fx, Footprint fy, MomentAccumulator& sum, float& alpha) {
 float alpha_error = 0;
 for (auto y = fy.first; y < fy.end; ++y)
  for (auto x = fx.first; x < fx.end; ++x) accumulate_sample<Format, Integer>(source, transfer, fx, fy, x, y, sum, alpha, alpha_error);
 if constexpr (Format == RgbPixelFormat::RGBA8) alpha = sum.weight > 0 ? (alpha - alpha_error) / sum.weight : 0;
}
}  // namespace mmltk::backend::imaging::resample::perceptual
#endif
