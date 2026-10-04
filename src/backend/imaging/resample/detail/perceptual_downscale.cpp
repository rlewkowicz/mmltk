// SPDX-License-Identifier: MIT
// Öztireli/Gross (2015) perceptual downscaling; provenance in perceptual_downscale_math.h.
#include "src/backend/imaging/resample/detail/perceptual_downscale.h"
#include "src/backend/imaging/resample/detail/perceptual_downscale_views.h"
#include "src/common/math/checked_arithmetic.h"
#include <algorithm>
#include <cstring>
#include <immintrin.h>
#include <new>
#include <stdexcept>
namespace mmltk::backend::imaging::resample::perceptual {
namespace {
const CpuTransferTable& cpu_transfer_table() {
 static const auto table = [] {
  CpuTransferTable result;
  for (unsigned code = 0; code < 256; ++code) result.thresholds.linear[code] = decode(float(code) * (1.0F / 255.0F));
  // Double subtraction is exact for these Float32 thresholds. A spacing
  // strictly greater than 2^-12 proves at most one threshold inside each
  // half-open bucket. Verify the real table before publishing the lookup.
  for (unsigned code = 1; code < 256; ++code)
   if (!(double(result.thresholds.linear[code]) - double(result.thresholds.linear[code - 1]) > 1.0 / CpuTransferTable::bucket_count))
    throw std::logic_error("CPU transfer thresholds exceed one correction per bucket");
  for (unsigned bucket = 0; bucket <= CpuTransferTable::bucket_count; ++bucket) result.base_code[bucket] = result.thresholds.quantize(float(bucket) * (1.0F / CpuTransferTable::bucket_count));
  return result;
 }();
 return table;
}
// The same patch, reconstruction and inverse expressions instantiate for CPU
// lanes and the scalar/CUDA value type. Explicit FMA remains explicit; ordinary
// expressions retain their association and the target's contraction policy.
struct Lanes {
 __m256 value;
 Lanes() : value(_mm256_setzero_ps()) {}
 Lanes(float v) : value(_mm256_set1_ps(v)) {}
 Lanes(__m256 v) : value(v) {}
 friend Lanes operator+(Lanes a, Lanes b) { return _mm256_add_ps(a.value, b.value); }
 friend Lanes operator-(Lanes a, Lanes b) { return _mm256_sub_ps(a.value, b.value); }
 friend Lanes operator*(Lanes a, Lanes b) { return _mm256_mul_ps(a.value, b.value); }
};
Lanes multiply_add(Lanes a, Lanes b, Lanes c) { return _mm256_fmadd_ps(a.value, b.value, c.value); }
Lanes sum_squares(Lanes a, Lanes b, Lanes c, Lanes d) {
 // Match scalar contraction: round b*b before the first fused addition.
 // Intrinsic expression contraction otherwise chooses a*a as its addend.
 return multiply_add(d, d, multiply_add(c, c, multiply_add(a, a, b * b)));
}
Lanes contrast_ratio(Lanes low, Lanes within) {
 const auto active = _mm256_cmp_ps(low.value, _mm256_set1_ps(variance_threshold), _CMP_GE_OQ);
 const auto denominator = _mm256_blendv_ps(_mm256_set1_ps(1), low.value, active);
 const auto ratio = _mm256_sqrt_ps(_mm256_add_ps(_mm256_set1_ps(1), _mm256_div_ps(_mm256_max_ps(within.value, _mm256_setzero_ps()), denominator)));
 return _mm256_blendv_ps(_mm256_set1_ps(low_variance_ratio), ratio, active);
}
__m256 unit8(__m256 value) {
 // Second-operand zero gives scalar unit's NaN -> zero behavior.
 return _mm256_min_ps(_mm256_max_ps(value, _mm256_setzero_ps()), _mm256_set1_ps(1));
}
__m256i quantize8(const CpuTransferTable& table, __m256 value) {
 value = unit8(value);
 // Power-of-two scaling preserves the exact bucket for clamped Float32
 // inputs, including subnormals. The shared table proves one correction.
 const auto bucket = _mm256_cvttps_epi32(_mm256_mul_ps(value, _mm256_set1_ps(CpuTransferTable::bucket_count)));
 const auto code = _mm256_i32gather_epi32(reinterpret_cast<const int*>(table.base_code.data()), bucket, 4);
 const auto last = _mm256_set1_epi32(255);
 const auto candidate = _mm256_min_epi32(_mm256_add_epi32(code, _mm256_set1_epi32(1)), last);
 const auto threshold = _mm256_i32gather_ps(table.thresholds.linear, candidate, 4);
 const auto accept = _mm256_and_si256(_mm256_cmpgt_epi32(last, code), _mm256_castps_si256(_mm256_cmp_ps(threshold, value, _CMP_LE_OQ)));
 // The endpoint gathers threshold 255 and cannot increment beyond it.
 return _mm256_sub_epi32(code, accept);
}
__m256i lane_mask(unsigned count) { return _mm256_cmpgt_epi32(_mm256_set1_epi32(static_cast<int>(count)), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7)); }
void store_lanes(float* output, unsigned count, __m256 value) {
 if (count == 8)
  _mm256_storeu_ps(output, value);
 else
  _mm256_maskstore_ps(output, lane_mask(count), value);
}
// Six contiguous channel planes per row avoid the old moment scatter followed
// by coefficient gathers. Only the two rows of each state are retained.
class StateRow {
 float* data_;
 std::uint32_t width_;
 Lanes load(unsigned channel, std::int64_t x) const {
  const float* values = data_ + std::size_t(channel) * width_;
  if (x < 0) return _mm256_permutevar8x32_ps(load(channel, 0).value, _mm256_setr_epi32(0, 0, 1, 2, 3, 4, 5, 6));
  if (x >= width_) return values[width_ - 1];
  const auto remaining = width_ - static_cast<std::uint32_t>(x);
  if (remaining >= 8) return _mm256_loadu_ps(values + x);
  const auto mask = lane_mask(remaining);
  return _mm256_blendv_ps(_mm256_set1_ps(values[width_ - 1]), _mm256_maskload_ps(values + x, mask), _mm256_castsi256_ps(mask));
 }

public:
 StateRow(float* data, std::uint32_t width) : data_(data), width_(width) {}
 float* channel(unsigned k, unsigned x = 0) const { return data_ + std::size_t(k) * width_ + x; }
 MomentValue<Lanes> moment(std::int64_t x) const { return {{load(0, x), load(1, x), load(2, x)}, {load(3, x), load(4, x), load(5, x)}}; }
 CoefficientValue<Lanes> coefficient(std::int64_t x) const { return {{load(0, x), load(1, x), load(2, x)}, {load(3, x), load(4, x), load(5, x)}}; }
 void store(unsigned x, const CoefficientValue<Lanes>& value) const {
  const auto count = std::min(8U, width_ - x);
  for (unsigned k = 0; k < 3; ++k) {
   store_lanes(channel(k, x), count, value.ratio[k].value);
   store_lanes(channel(k + 3, x), count, value.center[k].value);
  }
 }
};
__m128i pack_codes(__m256i codes) {
 const auto words = _mm_packus_epi32(_mm256_castsi256_si128(codes), _mm256_extracti128_si256(codes, 1));
 return _mm_packus_epi16(words, _mm_setzero_si128());
}
template <RgbPixelFormat Format>
void store_packed_block(std::uint8_t* output, unsigned count, const __m128i (&rgb)[3], __m256 coverage) {
 __m128i alpha = _mm_setzero_si128();
 if constexpr (Format == RgbPixelFormat::RGBA8) {
  const auto scaled = _mm256_mul_ps(unit8(coverage), _mm256_set1_ps(255));
  const auto integral = _mm256_cvttps_epi32(scaled);
  const auto half = _mm256_cmp_ps(_mm256_sub_ps(scaled, _mm256_cvtepi32_ps(integral)), _mm256_set1_ps(0.5F), _CMP_GE_OQ);
  // lroundf for the nonnegative unit-domain alpha, including half ties.
  alpha = pack_codes(_mm256_sub_epi32(integral, _mm256_castps_si256(half)));
 }
 const auto rg = _mm_unpacklo_epi8(rgb[0], rgb[1]), ba = _mm_unpacklo_epi8(rgb[2], alpha);
 auto first = _mm_unpacklo_epi16(rg, ba), second = _mm_unpackhi_epi16(rg, ba);
 constexpr unsigned channels = Format == RgbPixelFormat::RGBA8 ? 4 : 3;
 if constexpr (Format == RgbPixelFormat::RGB8) {
  const auto mask = _mm_setr_epi8(0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1);
  first = _mm_shuffle_epi8(first, mask);
  second = _mm_shuffle_epi8(second, mask);
  first = _mm_or_si128(first, _mm_slli_si128(second, 12));
  second = _mm_srli_si128(second, 4);
 }
 if (count == 8) {
  _mm_storeu_si128(reinterpret_cast<__m128i*>(output), first);
  if constexpr (channels == 4)
   _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 16), second);
  else
   _mm_storel_epi64(reinterpret_cast<__m128i*>(output + 16), second);
 } else {
  alignas(16) std::uint8_t bytes[32];
  _mm_store_si128(reinterpret_cast<__m128i*>(bytes), first);
  _mm_store_si128(reinterpret_cast<__m128i*>(bytes + 16), second);
  std::memcpy(output, bytes, count * channels);
 }
}
template <RgbPixelFormat Format, bool QuantizedPlanar>
void store_block(RgbMutableImageView destination, unsigned x, unsigned y, ColorValue<Lanes> value, const float* alpha, const CpuTransferTable& table) {
 const auto rgb = inverse(value);
 __m256 channels[]{rgb.y.value, rgb.cb.value, rgb.cr.value};
 const unsigned count = std::min(8U, destination.layout.width - x);
 const auto mask = lane_mask(count);
 auto* row = static_cast<std::uint8_t*>(destination.data) + std::size_t(y) * destination.layout.row_stride_bytes;
 __m256 coverage{};
 [[maybe_unused]] __m128i packed[3];
 if constexpr (Format == RgbPixelFormat::RGBA8) coverage = _mm256_maskload_ps(alpha + x, mask);
 for (unsigned k = 0; k < 3; ++k) {
  auto linear = channels[k];
  if constexpr (Format == RgbPixelFormat::RGBA8) {
   const auto present = _mm256_cmp_ps(coverage, _mm256_setzero_ps(), _CMP_GT_OQ);
   linear = _mm256_and_ps(_mm256_div_ps(linear, _mm256_blendv_ps(_mm256_set1_ps(1), coverage, present)), present);
  }
  linear = unit8(linear);
  if constexpr (QuantizedPlanar) {
   store_lanes(reinterpret_cast<float*>(row + k * destination.layout.plane_stride_bytes) + x, count, _mm256_mul_ps(_mm256_cvtepi32_ps(quantize8(table, linear)), _mm256_set1_ps(1.0F / 255.0F)));
  } else if constexpr (Format == RgbPixelFormat::PlanarUnitSrgbF32) {
   // Continuous Float32 output deliberately retains the scalar library powf.
   alignas(32) float values[8];
   _mm256_store_ps(values, linear);
   auto* plane = reinterpret_cast<float*>(row + k * destination.layout.plane_stride_bytes);
   for (unsigned lane = 0; lane < count; ++lane) plane[x + lane] = encode(values[lane]);
  } else {
   packed[k] = pack_codes(quantize8(table, linear));
  }
 }
 if constexpr (!QuantizedPlanar && Format != RgbPixelFormat::PlanarUnitSrgbF32) {
  constexpr unsigned components = Format == RgbPixelFormat::RGBA8 ? 4 : 3;
  store_packed_block<Format>(row + std::size_t(x) * components, count, packed, coverage);
 }
}
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
void moments8(RgbConstImageView source, const TransferTable& transfer, const Footprint* fx, Footprint fy, StateRow output, unsigned x, float* alpha_output) {
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
 for (unsigned k = 0; k < 3; ++k) {
  _mm256_storeu_ps(output.channel(k, x), means[k]);
  _mm256_storeu_ps(output.channel(k + 3, x), _mm256_max_ps(_mm256_setzero_ps(), normalize(_mm256_sub_ps(variances[k], variance_errors[k]))));
 }
 if constexpr (Format == RgbPixelFormat::RGBA8) _mm256_storeu_ps(alpha_output, _mm256_min_ps(_mm256_set1_ps(1), _mm256_max_ps(_mm256_setzero_ps(), normalize(_mm256_sub_ps(alpha_sum, alpha_error)))));
}
}  // namespace
void CpuDownscaler::quantize_block(const float* input, std::uint8_t* output) const {
 alignas(32) int codes[8];
 _mm256_store_si256(reinterpret_cast<__m256i*>(codes), quantize8(transfer_, _mm256_loadu_ps(input)));
 for (unsigned lane = 0; lane < 8; ++lane) output[lane] = static_cast<std::uint8_t>(codes[lane]);
}
void copy_identity(RgbConstImageView source, RgbMutableImageView destination) {
 if (source.data == destination.data) return;
 const auto geometry = identity_geometry(source.layout);
 for (unsigned plane = 0; plane < geometry.planes; ++plane)
  for (std::uint32_t y = 0; y < source.layout.height; ++y)
   std::memcpy(static_cast<std::uint8_t*>(destination.data) + plane * destination.layout.plane_stride_bytes + y * destination.layout.row_stride_bytes,
    static_cast<const std::uint8_t*>(source.data) + plane * source.layout.plane_stride_bytes + y * source.layout.row_stride_bytes, geometry.row_bytes);
}
CpuDownscaler::CpuDownscaler() : transfer_(cpu_transfer_table()) {}
namespace {
std::size_t aligned_extent(std::size_t count, std::size_t size) {
 const auto bytes = common::math::checked_multiply(count, size, "perceptual workspace overflow");
 return common::math::checked_add(bytes, std::size_t{63}, "perceptual workspace overflow") / 64 * 64;
}
}  // namespace
std::size_t CpuDownscaler::workspace_bytes(std::uint32_t width, std::uint32_t height, bool alpha) {
 using common::math::checked_add;
 auto total = checked_add(aligned_extent(width, sizeof(Footprint)), aligned_extent(height, sizeof(Footprint)), "perceptual workspace overflow");
 for (unsigned row = 0; row < 2; ++row) {
  total = checked_add(total, aligned_extent(width, sizeof(Moment)), "perceptual workspace overflow");
  total = checked_add(total, aligned_extent(width, sizeof(Coefficient)), "perceptual workspace overflow");
  if (alpha) total = checked_add(total, aligned_extent(width, sizeof(float)), "perceptual workspace overflow");
 }
 return total;
}
void CpuDownscaler::bind(std::uint32_t width, std::uint32_t height, bool alpha, std::span<std::byte> memory) {
 if (memory.size() < workspace_bytes(width, height, alpha) || reinterpret_cast<std::uintptr_t>(memory.data()) % 64) throw std::length_error("perceptual workspace capacity or alignment invalid");
 prepared_ = false;
 const auto array = [&]<class T>(mmltk::common::types::WorkspaceBuffer<T>& target, std::size_t count) {
  target.bind({reinterpret_cast<T*>(memory.data()), count});
  memory = memory.subspan(aligned_extent(count, sizeof(T)));
 };
 array(x_, width);
 array(y_, height);
 for (unsigned row = 0; row < 2; ++row) {
  array(moments_[row], std::size_t(width) * 6);
  array(coefficients_[row], std::size_t(width) * 6);
  if (alpha) array(alpha_[row], width);
 }
}
std::size_t CpuDownscaler::retained_bytes() const noexcept {
 auto result = x_.capacity() * sizeof(Footprint) + y_.capacity() * sizeof(Footprint);
 for (unsigned row = 0; row < 2; ++row) result += (moments_[row].capacity() + coefficients_[row].capacity()) * sizeof(float) + alpha_[row].capacity() * sizeof(float);
 return result;
}
void CpuDownscaler::preparation_checkpoint(PreparationStep step) {
 if (fail_before_ == step) {
  fail_before_ = PreparationStep::None;
  throw std::bad_alloc();
 }
}
void CpuDownscaler::prepare(const RgbImageLayout& source, const RgbImageLayout& destination, RgbResizeWork* observed) {
 const auto width = destination.width, height = destination.height;
 (void)common::math::checked_multiply<std::size_t>(width, sizeof(Moment) * 2 + sizeof(Coefficient) * 2 + sizeof(float) * 2, "perceptual image extent overflow");
 const bool geometry_changed = !prepared_ || source_width_ != source.width || source_height_ != source.height || width_ != width || height_ != height;
 const bool alpha_needed = source.format == RgbPixelFormat::RGBA8 && (alpha_[0].size() != width || alpha_[1].size() != width);
 if (!geometry_changed && !alpha_needed) return;
 if (observed) ++observed->preparations;
 const auto resize = [&]<class T>(common::types::WorkspaceBuffer<T>& buffer, std::size_t count) {
  if (observed && !buffer.bound() && count > buffer.capacity()) {
   ++observed->allocation_requests;
   observed->allocation_bytes += count * sizeof(T);
  }
  buffer.resize_for_overwrite(count);
 };
 // Both axes are filled below. Execution fills every active moment/alpha
 // row before reading it and every coefficient row before reconstruction;
 // first/last-row aliases only address those filled rows. Accumulator zeros
 // remain mathematical state in execute/moments8, never scratch contents.
 // In-place preparation retains useful capacity. Any exception after this
 // point must force complete axis/row preparation, even for the old key.
 prepared_ = false;
 if (geometry_changed) {
  preparation_checkpoint(PreparationStep::HorizontalAxis);
  resize(x_, width);
  preparation_checkpoint(PreparationStep::VerticalAxis);
  resize(y_, height);
  for (std::uint32_t x = 0; x < width; ++x) x_[x] = footprint(source.width, width, x);
  for (std::uint32_t y = 0; y < height; ++y) y_[y] = footprint(source.height, height, y);
  for (unsigned row = 0; row < 2; ++row) {
   preparation_checkpoint(row == 0 ? PreparationStep::FirstMoment : PreparationStep::SecondMoment);
   resize(moments_[row], std::size_t(width) * 6);
  }
  for (unsigned row = 0; row < 2; ++row) {
   preparation_checkpoint(row == 0 ? PreparationStep::FirstCoefficient : PreparationStep::SecondCoefficient);
   resize(coefficients_[row], std::size_t(width) * 6);
  }
 }
 if (source.format == RgbPixelFormat::RGBA8) {
  for (unsigned row = 0; row < 2; ++row) {
   preparation_checkpoint(row == 0 ? PreparationStep::FirstAlpha : PreparationStep::SecondAlpha);
   resize(alpha_[row], width);
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
  const StateRow row(moments_[y % 2].data(), width);
  std::uint32_t x = 0;
  for (; width - x >= 8; x += 8) {
   float* alpha = nullptr;
   if constexpr (Format == RgbPixelFormat::RGBA8) alpha = alpha_[y % 2].data() + x;
   moments8<Format, Integer>(source, transfer_.thresholds, x_.data() + x, fy, row, x, alpha);
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
     const Color color = load<Format>(source, sx, sy, transfer_.thresholds, alpha);
     sum.add(color, weight);
     if constexpr (Format == RgbPixelFormat::RGBA8) compensated_add(weight * alpha, coverage, coverage_error);
    }
   }
   const auto value = sum.finish();
   for (unsigned k = 0; k < 3; ++k) {
    *row.channel(k, x) = value.mean[k];
    *row.channel(k + 3, x) = value.variance[k];
   }
   if constexpr (Format == RgbPixelFormat::RGBA8) alpha_[y % 2][x] = unit((coverage - coverage_error) / sum.weight);
  }
 };
 fill_row(0);
 for (std::uint32_t y = 0; y < height; ++y) {
  if (y + 1 < height) fill_row(y + 1);
  const StateRow row(moments_[y % 2].data(), width);
  const StateRow next(moments_[(y + 1 < height ? y + 1 : y) % 2].data(), width);
  const StateRow current(coefficients_[y % 2].data(), width);
  const StateRow previous(coefficients_[(y ? y - 1 : y) % 2].data(), width);
  for (std::uint32_t x = 0; x < width; x += 8) current.store(x, patch(row.moment(x), row.moment(std::int64_t(x) + 1), next.moment(x), next.moment(std::int64_t(x) + 1)));
  for (std::uint32_t x = 0; x < width; x += 8) {
   const auto left = std::int64_t(x) - 1;
   const float* alpha = nullptr;
   if constexpr (Format == RgbPixelFormat::RGBA8) alpha = alpha_[y % 2].data();
   store_block<Format, QuantizedPlanar>(
    destination, x, y, reconstruct(row.moment(x), current.coefficient(x), current.coefficient(left), previous.coefficient(x), previous.coefficient(left)), alpha, transfer_);
  }
 }
}
void CpuDownscaler::run_quantized_planar(RgbConstImageView source, RgbMutableImageView destination) {
 (void)validate_pair(source, destination, true);
 // Explicit quantized filtering also supports unchanged geometry; unlike the
 // compiler's identity projection this API has always applied the filter.
 run_selected(source, destination, classify_geometry(source.layout.width, source.layout.height, destination.layout.width, destination.layout.height), true);
}
void CpuDownscaler::run(RgbConstImageView source, RgbMutableImageView destination) {
 if (validate_pair(source, destination)) {
  copy_identity(source, destination);
  return;
 }
 run_selected(source, destination, classify_geometry(source.layout.width, source.layout.height, destination.layout.width, destination.layout.height));
}
void CpuDownscaler::run_selected(RgbConstImageView source, RgbMutableImageView destination, RgbResizePath path, bool quantized_planar, RgbResizeWork* observed) {
 (void)validate_pair(source, destination, quantized_planar);
 if (path != RgbResizePath::PerceptualInteger && path != RgbResizePath::PerceptualFractional) throw std::invalid_argument("invalid perceptual execution path");
 prepare(source.layout, destination.layout, observed);
 const bool integer = path == RgbResizePath::PerceptualInteger;
 if (quantized_planar) {
  if (integer)
   execute<RgbPixelFormat::RGB8, true, true>(source, destination);
  else
   execute<RgbPixelFormat::RGB8, false, true>(source, destination);
  return;
 }
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
