#include "src/backend/data/image_resize.h"
#include "src/backend/data/detail/perceptual_downscale.h"

#include <immintrin.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "avir.h"

namespace mmltk::backend::data {  // Canonical backend.data implementation namespace.

namespace {

void warm_up_avir_rgb_resize_path() {
    std::array<std::uint8_t, 12> source{
        0U, 16U, 32U, 48U, 64U, 80U, 96U, 112U, 128U, 144U, 160U, 176U,
    };
    std::array<std::uint8_t, 27> output{};
    avir::CImageResizer<> resizer{8};
    resizer.resizeImage(source.data(), 2, 2, 0, output.data(), 3, 3, 3, 0.0, nullptr);
}

}  // namespace

struct RgbImageResizer::Impl {
    avir::CImageResizer<> resizer{8};
    bool perceptual_enabled = false;
    std::unique_ptr<perceptual::CpuDownscaler> perceptual;
};

RgbLetterbox compute_rgb_letterbox(const std::uint32_t source_width, const std::uint32_t source_height, const std::uint32_t target_width,
                                   const std::uint32_t target_height) {
    if (source_width == 0U || source_height == 0U || target_width == 0U || target_height == 0U) {
        throw std::runtime_error("letterbox source and target dimensions must be positive");
    }

    RgbLetterbox result;
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
    if (static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(width)) {
        throw std::overflow_error("RGB conversion pixel count overflow");
    }
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

inline void convert_sixteen_rgb_pixels(const std::uint8_t* source, float* destination_r, float* destination_g, float* destination_b,
                                       const __m256 scale, const RgbShuffleMasks& masks) {
    const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source));
    const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + 16));
    const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + 32));
    const __m128i r =
        _mm_or_si128(_mm_shuffle_epi8(a, masks.r_a), _mm_or_si128(_mm_shuffle_epi8(b, masks.r_b), _mm_shuffle_epi8(c, masks.r_c)));
    const __m128i g =
        _mm_or_si128(_mm_shuffle_epi8(a, masks.g_a), _mm_or_si128(_mm_shuffle_epi8(b, masks.g_b), _mm_shuffle_epi8(c, masks.g_c)));
    const __m128i blue =
        _mm_or_si128(_mm_shuffle_epi8(a, masks.b_a), _mm_or_si128(_mm_shuffle_epi8(b, masks.b_b), _mm_shuffle_epi8(c, masks.b_c)));

    _mm256_storeu_ps(destination_r, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(r)), scale));
    _mm256_storeu_ps(destination_r + 8, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_srli_si128(r, 8))), scale));
    _mm256_storeu_ps(destination_g, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(g)), scale));
    _mm256_storeu_ps(destination_g + 8, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_srli_si128(g, 8))), scale));
    _mm256_storeu_ps(destination_b, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(blue)), scale));
    _mm256_storeu_ps(destination_b + 8, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_srli_si128(blue, 8))), scale));
}

void clear_letterbox_padding(float* plane, const std::uint32_t destination_width, const std::uint32_t destination_height,
                             const std::uint32_t source_width, const std::uint32_t source_height, const std::uint32_t offset_x,
                             const std::uint32_t offset_y) {
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
inline void convert_rgb_pixels_to_planar_f32(const std::uint8_t* source, float* destination_r, float* destination_g, float* destination_b,
                                             const std::size_t pixel_count, const __m256 scale, const RgbShuffleMasks& masks) {
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
    convert_rgb_pixels_to_planar_f32(source, destination, destination + pixel_count, destination + pixel_count * 2U, pixel_count, scale,
                                     masks);
}

void letterboxed_rgb_hwc_u8_to_nchw_f32(const std::uint8_t* source, float* destination, const std::uint32_t source_width,
                                        const std::uint32_t source_height, const std::uint32_t destination_width,
                                        const std::uint32_t destination_height, const std::uint32_t offset_x,
                                        const std::uint32_t offset_y) {
    if (source == nullptr || destination == nullptr) { throw std::runtime_error("letterboxed RGB conversion requires non-null buffers"); }
    if (source_width > destination_width || source_height > destination_height || offset_x > destination_width - source_width ||
        offset_y > destination_height - source_height) {
        throw std::runtime_error("letterboxed RGB region is outside its destination");
    }
    const std::size_t destination_pixel_count = checked_pixel_count(destination_width, destination_height);
    (void)checked_pixel_count(source_width, source_height);
    clear_letterbox_padding(destination, destination_width, destination_height, source_width, source_height, offset_x, offset_y);
    clear_letterbox_padding(destination + destination_pixel_count, destination_width, destination_height, source_width, source_height,
                            offset_x, offset_y);
    clear_letterbox_padding(destination + destination_pixel_count * 2U, destination_width, destination_height, source_width, source_height,
                            offset_x, offset_y);

    const __m256 scale = _mm256_set1_ps(1.0F / 255.0F);
    const RgbShuffleMasks& masks = rgb_shuffle_masks();
    for (std::uint32_t row = 0U; row < source_height; ++row) {
        const std::uint8_t* source_row = source + static_cast<std::size_t>(row) * source_width * 3U;
        const std::size_t destination_row = static_cast<std::size_t>(offset_y + row) * destination_width + offset_x;
        convert_rgb_pixels_to_planar_f32(source_row, destination + destination_row, destination + destination_pixel_count + destination_row,
                                         destination + destination_pixel_count * 2U + destination_row, source_width, scale, masks);
    }
}

ResizeWorkerPlan plan_rgb_resize_workers(int total_workers, bool any_resize, bool any_downscale) {
    (void)any_resize;
    (void)any_downscale;
    return {std::max(1, total_workers), 1};
}

RgbImageResizer::RgbImageResizer(int thread_count, bool perceptual_downscale) {
    if (thread_count != 1) {
        throw std::runtime_error("RgbImageResizer internal threading is disabled; use image-level parallelism and pass thread_count=1");
    }
    static std::once_flag avir_warmup_once;
    std::call_once(avir_warmup_once, warm_up_avir_rgb_resize_path);
    impl_ = std::make_unique<Impl>();
    impl_->perceptual_enabled = perceptual_downscale;
}

RgbImageResizer::~RgbImageResizer() = default;

RgbImageResizer::RgbImageResizer(RgbImageResizer&&) noexcept = default;

RgbImageResizer& RgbImageResizer::operator=(RgbImageResizer&&) noexcept = default;

void RgbImageResizer::resize(const uint8_t* src, int src_width, int src_height, uint8_t* dst, int dst_width, int dst_height) {
    if (src == nullptr || dst == nullptr) { throw std::runtime_error("RgbImageResizer requires non-null input and output buffers"); }
    if (src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0) {
        throw std::runtime_error("RgbImageResizer dimensions must be positive");
    }
    if (src_width == dst_width && src_height == dst_height) {
        if (dst != src) std::memcpy(dst, src, perceptual::checked_product(perceptual::checked_product(src_width,src_height),3));
        return;
    }

    if (impl_->perceptual_enabled && dst_width <= src_width && dst_height <= src_height) {
        const auto layout = [](int width,int height) {
            const auto row=perceptual::checked_product(width,3);
            return RgbImageLayout{static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),row,0,
                                  perceptual::checked_product(row,height),RgbPixelFormat::RGB8};
        };
        downscale({src,layout(src_width,src_height)},{dst,layout(dst_width,dst_height)});
        return;
    }
    impl_->resizer.resizeImage(src, src_width, src_height, 0, dst, dst_width, dst_height, 3, 0.0, nullptr);
}

void RgbImageResizer::downscale(RgbConstImageView source, RgbMutableImageView destination) {
    if (perceptual::validate_pair(source,destination)) { perceptual::copy_identity(source,destination); return; }
    if (!impl_->perceptual) impl_->perceptual=std::make_unique<perceptual::CpuDownscaler>();
    impl_->perceptual->run(source,destination);
}
}  // namespace mmltk::backend::data
