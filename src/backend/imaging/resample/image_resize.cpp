#include "src/backend/imaging/resample/image_resize.h"
#include "src/backend/imaging/resample/detail/perceptual_downscale.h"
#include "src/backend/imaging/resample/detail/perceptual_downscale_views.h"
#include "src/backend/imaging/resample/detail/torch_resize.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/common/types/workspace_buffer.h"
#include <immintrin.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include "avir_float4_sse.h"
namespace mmltk::backend::imaging::resample {  // Model-independent resampling.
namespace {
using CpuAvirResizer = avir::CImageResizer<avir::fpclass_float4>;
}  // namespace
struct RgbImageResizer::Impl {
 struct AlignedDelete {
  void operator()(std::byte* data) const { ::operator delete(data, std::align_val_t{64}); }
 };
 std::unique_ptr<std::byte, AlignedDelete> owned_avir;
 std::unique_ptr<std::byte, AlignedDelete> owned_torch;
 std::size_t owned_torch_bytes = 0;
 std::span<std::byte> torch;
 std::size_t owned_avir_bytes = 0;
 std::optional<RgbResizeWorkspacePlan> plan;
 const RgbResizeWorkspacePlan& plan_for(std::uint32_t sw, std::uint32_t sh, std::uint32_t dw, std::uint32_t dh, ImageResizeMode mode, RgbPixelFormat format = RgbPixelFormat::RGB8);
 void execute_avir(const RgbResizeWorkspacePlan&, const std::uint8_t*, std::uint8_t*, RgbResizeWork*);
 std::span<std::byte> avir;
 bool supplied = false;
 std::size_t supplied_bytes = 0;
 bool perceptual_enabled = false;
 RgbResizeMethod method = RgbResizeMethod::Native;
 mmltk::common::types::WorkspaceBuffer<std::uint8_t> byte_scratch;
 std::unique_ptr<perceptual::CpuDownscaler> perceptual;
};
namespace {
std::size_t resize_aligned(std::size_t bytes) { return common::math::checked_add(bytes, std::size_t{63}, "resize workspace overflow") / 64 * 64; }
class AvirWorkspace final : public avir::detail::CWorkspaceAllocator {
 std::span<std::byte> supplied_;
 RgbResizeWork* observed_;

public:
 std::size_t used = 0;
 explicit AvirWorkspace(std::span<std::byte> memory, RgbResizeWork* observed) : supplied_(memory), observed_(observed) {}
 char* acquire(std::size_t bytes, bool) override {
  const auto extent = resize_aligned(bytes);
  const auto begin = used;
  used = common::math::checked_add(used, extent, "AVIR workspace overflow");
  if (observed_) {
   ++observed_->allocation_requests;
   observed_->allocation_bytes += extent;
  }
  if (used > supplied_.size()) throw std::length_error("AVIR supplied workspace exhausted");
  return reinterpret_cast<char*>(supplied_.data() + begin);
 }
 void release(char*, std::size_t) override {}
};
}  // namespace
std::size_t RgbResizeWorkspacePlan::bytes() const {
 return common::math::checked_add(torch_bytes,
  common::math::checked_add(resize_aligned(avir_bytes), common::math::checked_add(resize_aligned(byte_output_bytes), perceptual_bytes, "resize workspace overflow"), "resize workspace overflow"),
  "resize workspace overflow");
}
void RgbImageResizer::require_method(RgbResizeMethod method) {
 switch (method) {
  case RgbResizeMethod::Native: return;
  case RgbResizeMethod::TorchBilinear:
  case RgbResizeMethod::TorchBilinearAntialias: detail::require_torch_resize(); return;
 }
 throw std::invalid_argument("invalid RGB resize method");
}
RgbResizeWorkspacePlan RgbImageResizer::workspace_plan(
 std::uint32_t sw, std::uint32_t sh, std::uint32_t dw, std::uint32_t dh, ImageResizeMode mode, bool perceptual, RgbPixelFormat format, RgbResizeWork* observed, RgbResizeMethod method) {
 require_method(method);
 RgbResizeWorkspacePlan result;
 result.method_ = method;
 if (observed) ++observed->preparations;
 result.geometry = compute_image_resize_geometry(sw, sh, dw, dh, mode);
 result.source_width = sw;
 result.source_height = sh;
 result.target_width = dw;
 result.target_height = dh;
 result.mode = mode;
 result.format = format;
 result.alpha = format == RgbPixelFormat::RGBA8;
 result.perceptual = perceptual;
 switch (format) {
  case RgbPixelFormat::RGB8:
  case RgbPixelFormat::RGBA8:
  case RgbPixelFormat::PlanarUnitSrgbF32: break;
  default: throw std::invalid_argument("invalid resize pixel format");
 }
 const auto width = result.geometry.resized_width, height = result.geometry.resized_height;
 if (method != RgbResizeMethod::Native && format != RgbPixelFormat::RGB8) throw std::invalid_argument("Torch resize requires RGB8 input and planar float output");
 if (sw == width && sh == height) return result;
 if (method != RgbResizeMethod::Native) {
  const auto storage = detail::torch_resize_storage(sw, sh, width, height, method == RgbResizeMethod::TorchBilinearAntialias);
  result.path_ = RgbResizePath::Torch;
  result.torch_bytes = storage.supplied_bytes;
  result.opaque_bytes_ = storage.opaque_bytes;
  return result;
 }
 if (perceptual && width <= sw && height <= sh) {
  result.path_ = perceptual::classify_geometry(sw, sh, width, height);
  result.perceptual_bytes = perceptual::CpuDownscaler::workspace_bytes(width, height, result.alpha);
  return result;
 }
 if (format != RgbPixelFormat::RGB8) throw std::invalid_argument("AVIR workspace requires RGB8");
 const auto checked = [](std::uint32_t value) { return common::math::checked_cast<int>(value, "resize dimension overflow"); };
 result.path_ = RgbResizePath::Avir;
 std::size_t requests = 0;
 result.avir_bytes = CpuAvirResizer::workspaceBytes<std::uint8_t, std::uint8_t>(checked(sw), checked(sh), checked(width), checked(height), 3, observed ? &requests : nullptr);
 if (observed) {
  observed->allocation_requests += requests;
  observed->allocation_bytes += result.avir_bytes;
 }
 result.byte_output_bytes = common::math::checked_multiply(common::math::checked_multiply<std::size_t>(width, height, "resize workspace overflow"), 3, "resize workspace overflow");
 return result;
}
const RgbResizeWorkspacePlan& RgbImageResizer::Impl::plan_for(std::uint32_t sw, std::uint32_t sh, std::uint32_t dw, std::uint32_t dh, ImageResizeMode mode, RgbPixelFormat format) {
 if (plan && plan->source_width == sw && plan->source_height == sh && plan->target_width == dw && plan->target_height == dh && plan->mode == mode && plan->format == format) return *plan;
 if (supplied) throw std::invalid_argument("resize geometry differs from supplied workspace plan");
 plan = RgbImageResizer::workspace_plan(sw, sh, dw, dh, mode, perceptual_enabled, format, nullptr, method);
 return *plan;
}
void RgbImageResizer::Impl::execute_avir(const RgbResizeWorkspacePlan& selected, const std::uint8_t* source, std::uint8_t* destination, RgbResizeWork* observed) {
 if (!supplied && owned_avir_bytes != selected.avir_bytes) {
  owned_avir.reset(static_cast<std::byte*>(::operator new(selected.avir_bytes, std::align_val_t{64})));
  owned_avir_bytes = selected.avir_bytes;
 }
 const auto memory = supplied ? avir : std::span<std::byte>(owned_avir.get(), owned_avir_bytes);
 // Every allocator-dependent filter/queue dies before this stack allocator and
 // before the caller may return its arena lease. No coefficient cache survives.
 AvirWorkspace allocation(memory, observed);
 avir::detail::CWorkspaceScope scope(allocation);
 if (observed) ++observed->preparations;
 CpuAvirResizer prepared{8};
 prepared.resizeImage(source, static_cast<int>(selected.source_width), static_cast<int>(selected.source_height), 0, destination, static_cast<int>(selected.geometry.resized_width),
  static_cast<int>(selected.geometry.resized_height), 3, 0.0);
}
void RgbImageResizer::prepare(const RgbResizeWorkspacePlan& plan, std::span<std::byte> memory) {
 if (memory.size() < plan.bytes() || (memory.size() && reinterpret_cast<std::uintptr_t>(memory.data()) % 64)) throw std::length_error("resize supplied workspace capacity or alignment invalid");
 if (impl_->owned_avir || impl_->owned_torch || (impl_->perceptual && impl_->perceptual->retained_bytes() && !impl_->supplied) || (!impl_->supplied && impl_->byte_scratch.capacity()))
  throw std::logic_error("resize supplied workspace must bind before owned allocation");
 if (plan.perceptual_bytes && !impl_->perceptual) impl_->perceptual = std::make_unique<perceptual::CpuDownscaler>();
 impl_->plan = plan;
 impl_->supplied = true;
 impl_->supplied_bytes = plan.bytes();
 impl_->perceptual_enabled = plan.perceptual;
 impl_->method = plan.method();
 impl_->torch = memory.first(plan.torch_bytes);
 memory = memory.subspan(plan.torch_bytes);
 impl_->avir = memory.first(plan.avir_bytes);
 memory = memory.subspan(resize_aligned(plan.avir_bytes));
 impl_->byte_scratch.bind({reinterpret_cast<std::uint8_t*>(memory.data()), plan.byte_output_bytes});
 memory = memory.subspan(resize_aligned(plan.byte_output_bytes));
 if (plan.perceptual_bytes) { impl_->perceptual->bind(plan.geometry.resized_width, plan.geometry.resized_height, plan.alpha, memory.first(plan.perceptual_bytes)); }
}
std::size_t RgbImageResizer::retained_bytes() const noexcept {
 if (impl_->supplied) return impl_->supplied_bytes;
 return impl_->owned_avir_bytes + impl_->owned_torch_bytes + impl_->byte_scratch.capacity() + (impl_->perceptual ? impl_->perceptual->retained_bytes() : 0);
}
ImageResizeGeometry compute_image_resize_geometry(
 const std::uint32_t source_width, const std::uint32_t source_height, const std::uint32_t target_width, const std::uint32_t target_height, const ImageResizeMode mode) {
 if (source_width == 0U || source_height == 0U || target_width == 0U || target_height == 0U) { throw std::runtime_error("letterbox source and target dimensions must be positive"); }
 if (mode == ImageResizeMode::Stretch) return {target_width, target_height, 0U, 0U};
 if (mode != ImageResizeMode::Letterbox) throw std::invalid_argument("invalid image resize mode");
 ImageResizeGeometry result;
 const std::uint64_t width_limited = static_cast<std::uint64_t>(source_width) * target_height;
 const std::uint64_t height_limited = static_cast<std::uint64_t>(source_height) * target_width;
 if (width_limited >= height_limited) {
  result.resized_width = target_width;
  const std::uint64_t scaled_height = static_cast<std::uint64_t>(source_height) * target_width;
  result.resized_height = std::max<std::uint32_t>(1U, static_cast<std::uint32_t>((scaled_height + source_width / 2U) / source_width));
 } else {
  result.resized_height = target_height;
  const std::uint64_t scaled_width = static_cast<std::uint64_t>(source_width) * target_height;
  result.resized_width = std::max<std::uint32_t>(1U, static_cast<std::uint32_t>((scaled_width + source_height / 2U) / source_height));
 }
 result.resized_width = std::min(result.resized_width, target_width);
 result.resized_height = std::min(result.resized_height, target_height);
 result.offset_x = (target_width - result.resized_width) / 2U;
 result.offset_y = (target_height - result.resized_height) / 2U;
 return result;
}
namespace {
[[nodiscard]] std::size_t checked_pixel_count(const std::uint32_t width, const std::uint32_t height) {
 if (width == 0U || height == 0U) { throw std::runtime_error("RGB conversion dimensions must be positive"); }
 if (static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(width)) { throw std::overflow_error("RGB conversion pixel count overflow"); }
 const std::size_t pixels = static_cast<std::size_t>(width) * height;
 if (pixels > std::numeric_limits<std::size_t>::max() / 3U) { throw std::overflow_error("RGB conversion plane size overflow"); }
 return pixels;
}
struct RgbShuffleMasks {
 __m128i r_a;
 __m128i r_b;
 __m128i r_c;
 __m128i g_a;
 __m128i g_b;
 __m128i g_c;
 __m128i b_a;
 __m128i b_b;
 __m128i b_c;
};
[[nodiscard]] const RgbShuffleMasks& rgb_shuffle_masks() {
 static const RgbShuffleMasks masks{
  _mm_setr_epi8(0, 3, 6, 9, 12, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1),
  _mm_setr_epi8(-1, -1, -1, -1, -1, -1, 2, 5, 8, 11, 14, -1, -1, -1, -1, -1),
  _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 1, 4, 7, 10, 13),
  _mm_setr_epi8(1, 4, 7, 10, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1),
  _mm_setr_epi8(-1, -1, -1, -1, -1, 0, 3, 6, 9, 12, 15, -1, -1, -1, -1, -1),
  _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 2, 5, 8, 11, 14),
  _mm_setr_epi8(2, 5, 8, 11, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1),
  _mm_setr_epi8(-1, -1, -1, -1, -1, 1, 4, 7, 10, 13, -1, -1, -1, -1, -1, -1),
  _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 3, 6, 9, 12, 15),
 };
 return masks;
}
inline void store_sixteen_scaled_bytes(float* destination, const __m128i bytes, const __m256 scale) {
 _mm256_storeu_ps(destination, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(bytes)), scale));
 _mm256_storeu_ps(destination + 8, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_srli_si128(bytes, 8))), scale));
}
inline void convert_sixteen_rgb_pixels(const std::uint8_t* source, float* destination_r, float* destination_g, float* destination_b, const __m256 scale, const RgbShuffleMasks& masks) {
 const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source));
 const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + 16));
 const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + 32));
 const __m128i r = _mm_or_si128(_mm_shuffle_epi8(a, masks.r_a), _mm_or_si128(_mm_shuffle_epi8(b, masks.r_b), _mm_shuffle_epi8(c, masks.r_c)));
 const __m128i g = _mm_or_si128(_mm_shuffle_epi8(a, masks.g_a), _mm_or_si128(_mm_shuffle_epi8(b, masks.g_b), _mm_shuffle_epi8(c, masks.g_c)));
 const __m128i blue = _mm_or_si128(_mm_shuffle_epi8(a, masks.b_a), _mm_or_si128(_mm_shuffle_epi8(b, masks.b_b), _mm_shuffle_epi8(c, masks.b_c)));
 store_sixteen_scaled_bytes(destination_r, r, scale);
 store_sixteen_scaled_bytes(destination_g, g, scale);
 store_sixteen_scaled_bytes(destination_b, blue, scale);
}
void clear_letterbox_padding(float* plane, const std::uint32_t destination_width, const std::uint32_t destination_height, const std::uint32_t source_width, const std::uint32_t source_height,
 const std::uint32_t offset_x, const std::uint32_t offset_y) {
 const std::size_t top_pixels = static_cast<std::size_t>(offset_y) * destination_width;
 if (top_pixels != 0U) { std::fill_n(plane, top_pixels, 0.0F); }
 const std::uint32_t right = destination_width - offset_x - source_width;
 if (offset_x != 0U || right != 0U) {
  for (std::uint32_t row = 0U; row < source_height; ++row) {
   float* destination_row = plane + static_cast<std::size_t>(offset_y + row) * destination_width;
   std::fill_n(destination_row, offset_x, 0.0F);
   std::fill_n(destination_row + offset_x + source_width, right, 0.0F);
  }
 }
 const std::uint32_t content_end_y = offset_y + source_height;
 const std::size_t bottom_pixels = static_cast<std::size_t>(destination_height - content_end_y) * destination_width;
 if (bottom_pixels != 0U) { std::fill_n(plane + static_cast<std::size_t>(content_end_y) * destination_width, bottom_pixels, 0.0F); }
}
}  // namespace
namespace {
// Converts pixel_count packed RGB bytes into planar floats: sixteen pixels per SIMD step, then a
// scalar tail. Pointers are taken by value so callers keep their own cursors.
inline void convert_rgb_pixels_to_planar_f32(
 const std::uint8_t* source, float* destination_r, float* destination_g, float* destination_b, const std::size_t pixel_count, const __m256 scale, const RgbShuffleMasks& masks) {
 std::size_t pixel = 0U;
 for (; pixel + 15U < pixel_count; pixel += 16U) {
  convert_sixteen_rgb_pixels(source, destination_r, destination_g, destination_b, scale, masks);
  source += 48;
  destination_r += 16;
  destination_g += 16;
  destination_b += 16;
 }
 for (; pixel < pixel_count; ++pixel) {
  *destination_r++ = static_cast<float>(source[0]) * (1.0F / 255.0F);
  *destination_g++ = static_cast<float>(source[1]) * (1.0F / 255.0F);
  *destination_b++ = static_cast<float>(source[2]) * (1.0F / 255.0F);
  source += 3;
 }
}
}  // namespace
void rgb_hwc_u8_to_nchw_f32(const std::uint8_t* source, float* destination, const std::uint32_t width, const std::uint32_t height) {
 if (source == nullptr || destination == nullptr) { throw std::runtime_error("RGB conversion requires non-null buffers"); }
 const std::size_t pixel_count = checked_pixel_count(width, height);
 const __m256 scale = _mm256_set1_ps(1.0F / 255.0F);
 const RgbShuffleMasks& masks = rgb_shuffle_masks();
 convert_rgb_pixels_to_planar_f32(source, destination, destination + pixel_count, destination + pixel_count * 2U, pixel_count, scale, masks);
}
void letterboxed_rgb_hwc_u8_to_nchw_f32(const std::uint8_t* source, float* destination, const std::uint32_t source_width, const std::uint32_t source_height, const std::uint32_t destination_width,
 const std::uint32_t destination_height, const std::uint32_t offset_x, const std::uint32_t offset_y) {
 if (source == nullptr || destination == nullptr) { throw std::runtime_error("letterboxed RGB conversion requires non-null buffers"); }
 if (source_width > destination_width || source_height > destination_height || offset_x > destination_width - source_width || offset_y > destination_height - source_height) {
  throw std::runtime_error("letterboxed RGB region is outside its destination");
 }
 const std::size_t destination_pixel_count = checked_pixel_count(destination_width, destination_height);
 (void)checked_pixel_count(source_width, source_height);
 clear_letterbox_padding(destination, destination_width, destination_height, source_width, source_height, offset_x, offset_y);
 clear_letterbox_padding(destination + destination_pixel_count, destination_width, destination_height, source_width, source_height, offset_x, offset_y);
 clear_letterbox_padding(destination + destination_pixel_count * 2U, destination_width, destination_height, source_width, source_height, offset_x, offset_y);
 const __m256 scale = _mm256_set1_ps(1.0F / 255.0F);
 const RgbShuffleMasks& masks = rgb_shuffle_masks();
 for (std::uint32_t row = 0U; row < source_height; ++row) {
  const std::uint8_t* source_row = source + static_cast<std::size_t>(row) * source_width * 3U;
  const std::size_t destination_row = static_cast<std::size_t>(offset_y + row) * destination_width + offset_x;
  convert_rgb_pixels_to_planar_f32(
   source_row, destination + destination_row, destination + destination_pixel_count + destination_row, destination + destination_pixel_count * 2U + destination_row, source_width, scale, masks);
 }
}
ResizeWorkerPlan plan_rgb_resize_workers(int total_workers, bool any_resize, bool any_downscale) {
 (void)any_resize;
 (void)any_downscale;
 return {std::max(1, total_workers), 1};
}
RgbImageResizer::RgbImageResizer(int thread_count, bool perceptual_downscale, RgbResizeMethod method) {
 if (thread_count != 1) { throw std::runtime_error("RgbImageResizer internal threading is disabled; use image-level parallelism and pass thread_count=1"); }
 require_method(method);
 impl_ = std::make_unique<Impl>();
 impl_->perceptual_enabled = perceptual_downscale;
 impl_->method = method;
}
RgbImageResizer::~RgbImageResizer() = default;
RgbImageResizer::RgbImageResizer(RgbImageResizer&&) noexcept = default;
RgbImageResizer& RgbImageResizer::operator=(RgbImageResizer&&) noexcept = default;
void RgbImageResizer::resize(const uint8_t* src, int src_width, int src_height, uint8_t* dst, int dst_width, int dst_height) {
 if (src == nullptr || dst == nullptr) { throw std::runtime_error("RgbImageResizer requires non-null input and output buffers"); }
 if (src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0) { throw std::runtime_error("RgbImageResizer dimensions must be positive"); }
 const auto& plan = impl_->plan_for(src_width, src_height, dst_width, dst_height, ImageResizeMode::Stretch);
 switch (plan.path()) {
  case RgbResizePath::Identity:
   if (dst != src) {
    const auto pixels = common::math::checked_multiply<std::size_t>(src_width, src_height, "perceptual image extent overflow");
    std::memcpy(dst, src, common::math::checked_multiply(pixels, 3, "perceptual image extent overflow"));
   }
   return;
  case RgbResizePath::PerceptualInteger:
  case RgbResizePath::PerceptualFractional: {
   const auto layout = [](int width, int height) {
    const auto row = common::math::checked_multiply<std::size_t>(width, 3, "perceptual image extent overflow");
    return RgbImageLayout{
     static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), row, 0, common::math::checked_multiply(row, height, "perceptual image extent overflow"), RgbPixelFormat::RGB8
    };
   };
   if (!impl_->perceptual) impl_->perceptual = std::make_unique<perceptual::CpuDownscaler>();
   impl_->perceptual->run_selected({src, layout(src_width, src_height)}, {dst, layout(dst_width, dst_height)}, plan.path());
   return;
  }
  case RgbResizePath::Avir: impl_->execute_avir(plan, src, dst, nullptr); return;
  case RgbResizePath::Torch: throw std::invalid_argument("Torch resize writes float planes; use resize_to_planar");
 }
}
ImageResizeGeometry RgbImageResizer::resize_to_planar(RgbConstImageView source, RgbMutableImageView destination, const ImageResizeMode mode, RgbResizeWork* observed) {
 const auto source_bytes = perceptual::validate_view(source.data, source.layout);
 const auto destination_bytes = perceptual::validate_view(destination.data, destination.layout);
 const auto& input = source.layout;
 const auto& output = destination.layout;
 if (input.format != RgbPixelFormat::RGB8 || output.format != RgbPixelFormat::PlanarUnitSrgbF32 || input.row_stride_bytes != std::size_t(input.width) * 3U ||
     output.row_stride_bytes != std::size_t(output.width) * sizeof(float) || output.plane_stride_bytes != output.row_stride_bytes * output.height)
  throw std::invalid_argument("compiler resize requires packed RGB8 and contiguous planar float storage");
 const auto a = reinterpret_cast<std::uintptr_t>(source.data), b = reinterpret_cast<std::uintptr_t>(destination.data);
 if (a < b + destination_bytes && b < a + source_bytes) throw std::invalid_argument("compiler resize input/output storage overlaps");
 const auto& plan = impl_->plan_for(input.width, input.height, output.width, output.height, mode);
 const auto geometry = plan.geometry;
 auto* pixels = static_cast<float*>(destination.data);
 if (plan.path() == RgbResizePath::Torch) {
  if (!impl_->supplied && impl_->owned_torch_bytes < plan.torch_bytes) {
   auto* storage = static_cast<std::byte*>(::operator new(plan.torch_bytes, std::align_val_t{64}));
   impl_->owned_torch.reset(storage);
   impl_->owned_torch_bytes = plan.torch_bytes;
  }
  const auto memory = impl_->supplied ? impl_->torch : std::span<std::byte>(impl_->owned_torch.get(), impl_->owned_torch_bytes);
  detail::torch_resize(static_cast<const std::uint8_t*>(source.data), input.width, input.height, geometry.resized_width, geometry.resized_height,
   plan.method() == RgbResizeMethod::TorchBilinearAntialias, memory, observed);
  const auto* resized = reinterpret_cast<const float*>(memory.data()) + std::size_t(input.width) * input.height * 3;
  const auto plane_size = std::size_t(geometry.resized_width) * geometry.resized_height;
  for (unsigned plane = 0; plane < 3; ++plane) {
   auto* target = pixels + plane * output.plane_stride_bytes / sizeof(float);
   clear_letterbox_padding(target, output.width, output.height, geometry.resized_width, geometry.resized_height, geometry.offset_x, geometry.offset_y);
   for (std::uint32_t row = 0; row < geometry.resized_height; ++row)
    std::copy_n(resized + plane * plane_size + row * std::size_t(geometry.resized_width), geometry.resized_width, target + (row + geometry.offset_y) * std::size_t(output.width) + geometry.offset_x);
  }
  return geometry;
 }
 if (plan.path() == RgbResizePath::PerceptualInteger || plan.path() == RgbResizePath::PerceptualFractional) {
  const auto offset = std::size_t(geometry.offset_y) * output.row_stride_bytes + std::size_t(geometry.offset_x) * sizeof(float);
  auto content = destination;
  content.data = static_cast<std::uint8_t*>(destination.data) + offset;
  content.layout.width = geometry.resized_width;
  content.layout.height = geometry.resized_height;
  content.layout.capacity_bytes -= offset;
  if (!impl_->perceptual) impl_->perceptual = std::make_unique<perceptual::CpuDownscaler>();
  impl_->perceptual->run_selected(source, content, plan.path(), true, observed);
  for (unsigned plane = 0; plane < 3; ++plane)
   clear_letterbox_padding(
    pixels + plane * (output.plane_stride_bytes / sizeof(float)), output.width, output.height, geometry.resized_width, geometry.resized_height, geometry.offset_x, geometry.offset_y);
  return geometry;
 }
 const auto* bytes = static_cast<const std::uint8_t*>(source.data);
 if (plan.path() == RgbResizePath::Avir) {
  const auto count = common::math::checked_multiply<std::size_t>(geometry.resized_width, geometry.resized_height, "perceptual image extent overflow");
  impl_->byte_scratch.resize_for_overwrite(common::math::checked_multiply(count, 3U, "perceptual image extent overflow"));
  impl_->execute_avir(plan, bytes, impl_->byte_scratch.data(), observed);
  bytes = impl_->byte_scratch.data();
 }
 if (geometry.resized_width == output.width && geometry.resized_height == output.height)
  rgb_hwc_u8_to_nchw_f32(bytes, pixels, output.width, output.height);
 else
  letterboxed_rgb_hwc_u8_to_nchw_f32(bytes, pixels, geometry.resized_width, geometry.resized_height, output.width, output.height, geometry.offset_x, geometry.offset_y);
 return geometry;
}
void RgbImageResizer::downscale(RgbConstImageView source, RgbMutableImageView destination) {
 const auto validated = perceptual::validate_pair(source, destination);
 if (impl_->supplied) {
  const auto& selected = impl_->plan_for(source.layout.width, source.layout.height, destination.layout.width, destination.layout.height, ImageResizeMode::Stretch, source.layout.format);
  if (!validated) {
   if (!impl_->perceptual) throw std::invalid_argument("supplied workspace does not prepare perceptual filtering");
   impl_->perceptual->run_selected(source, destination, selected.path());
   return;
  }
 }
 if (validated) {
  perceptual::copy_identity(source, destination);
  return;
 }
 if (!impl_->perceptual) impl_->perceptual = std::make_unique<perceptual::CpuDownscaler>();
 impl_->perceptual->run(source, destination);
}
}  // namespace mmltk::backend::imaging::resample
