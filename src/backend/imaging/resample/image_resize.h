#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#if !defined(__CUDACC__)
#include "src/frameworks/reflection/reflection_metadata.h"
#endif
namespace mmltk::backend::imaging::resample {
enum class RgbPixelFormat : std::uint8_t { RGB8, RGBA8, PlanarUnitSrgbF32 };
// All strides and capacity are bytes. Float planes are R, G, B, contain unit
// sRGB (not model-normalized values), and require float alignment. RGBA is
// straight alpha. Padding is neither read as pixels nor written.
struct RgbImageLayout {
 std::uint32_t width = 0, height = 0;
 std::size_t row_stride_bytes = 0, plane_stride_bytes = 0, capacity_bytes = 0;
 RgbPixelFormat format = RgbPixelFormat::RGB8;
};
struct RgbConstImageView {
 const void* data = nullptr;
 RgbImageLayout layout{};
};
struct RgbMutableImageView {
 void* data = nullptr;
 RgbImageLayout layout{};
};
enum class ImageResizeMode : std::uint8_t { Stretch, Letterbox };
#if !defined(__CUDACC__)
MMLTK_REFLECT_ENUM(ImageResizeMode)
#endif
struct ImageResizeGeometry {
 std::uint32_t resized_width = 0;
 std::uint32_t resized_height = 0;
 std::uint32_t offset_x = 0;
 std::uint32_t offset_y = 0;
};
[[nodiscard]] ImageResizeGeometry compute_image_resize_geometry(std::uint32_t source_width, std::uint32_t source_height, std::uint32_t target_width,
                                                                std::uint32_t target_height, ImageResizeMode mode);
void rgb_hwc_u8_to_nchw_f32(const std::uint8_t* source, float* destination, std::uint32_t width, std::uint32_t height);
void letterboxed_rgb_hwc_u8_to_nchw_f32(const std::uint8_t* source, float* destination, std::uint32_t source_width, std::uint32_t source_height,
                                        std::uint32_t destination_width, std::uint32_t destination_height, std::uint32_t offset_x, std::uint32_t offset_y);
struct ResizeWorkerPlan {
 int image_workers = 1;
 int resize_threads_per_image = 1;
};
ResizeWorkerPlan plan_rgb_resize_workers(int total_workers, bool any_resize, bool any_downscale);
class RgbImageResizer {
public:
 explicit RgbImageResizer(int thread_count = 1, bool perceptual_downscale = false);
 ~RgbImageResizer();
 RgbImageResizer(const RgbImageResizer&) = delete;
 RgbImageResizer& operator=(const RgbImageResizer&) = delete;
 RgbImageResizer(RgbImageResizer&&) noexcept;
 RgbImageResizer& operator=(RgbImageResizer&&) noexcept;
 void resize(const uint8_t* src, int src_width, int src_height, uint8_t* dst, int dst_width, int dst_height);
 // Compiler projection: tightly packed RGB8 to a tightly packed planar
 // canvas. Perceptual output retains RGB8 quantization before conversion.
 // Admission and fallible preparation finish before any canvas write.
 ImageResizeGeometry resize_to_planar(RgbConstImageView source, RgbMutableImageView destination, ImageResizeMode mode);
 // Explicit checked perceptual operation. Same format on both sides; no
 // enlargement. Identity copies exactly (an exact alias is a no-op); all
 // other overlapping spans are rejected. Nonfinite/out-of-unit float input
 // is deterministically clamped, with NaN mapped to zero.
 // sRGB is decoded once, linear Y/Cb/Cr receives the clamped 2x2 SSIM
 // filter, and is encoded once. Alpha is area averaged, with premultiplied
 // linear filtering, safe unpremultiplication and zero color at zero alpha.
 void downscale(RgbConstImageView source, RgbMutableImageView destination);

private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::imaging::resample
