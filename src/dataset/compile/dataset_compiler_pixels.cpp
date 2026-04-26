#include "dataset_compiler_internal.h"
#include "image_resize.h"
#include "profile_utils.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <immintrin.h>
#include <vector>

namespace mmltk::compiler_internal {

namespace {

#if defined(__GNUC__) || defined(__clang__)
#define MMLTK_RESTRICT __restrict__
#else
#define MMLTK_RESTRICT
#endif

class WritablePixelRange {
   public:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    WritablePixelRange(const FileHandle& fd, size_t offset, size_t bytes) : bytes_(bytes) {
        if (bytes_ == 0) {
            return;
        }
        void* ptr = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(),
                         checked_cast<off_t>(offset, "pixel mmap offset overflow"));
        if (ptr == MAP_FAILED) {
            throw errno_error("mmap failed");
        }
        data_ = static_cast<float*>(ptr);
    }

    WritablePixelRange(const WritablePixelRange&) = delete;
    WritablePixelRange& operator=(const WritablePixelRange&) = delete;

    WritablePixelRange(WritablePixelRange&& other) noexcept : data_(other.data_), bytes_(other.bytes_) {
        other.data_ = nullptr;
        other.bytes_ = 0;
    }

    WritablePixelRange& operator=(WritablePixelRange&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        data_ = other.data_;
        bytes_ = other.bytes_;
        other.data_ = nullptr;
        other.bytes_ = 0;
        return *this;
    }

    ~WritablePixelRange() {
        reset();
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] float* image(uint32_t image_index, size_t image_stride) const {
        const size_t image_floats = image_stride / sizeof(float);
        return data_ + static_cast<size_t>(image_index) * image_floats;
    }

   private:
    void reset() noexcept {
        if (data_) {
            munmap(data_, bytes_);
            data_ = nullptr;
            bytes_ = 0;
        }
    }

    float* data_ = nullptr;
    size_t bytes_ = 0;
};

void hwc_uint8_to_nchw_float_avx2(const uint8_t* MMLTK_RESTRICT src, float* MMLTK_RESTRICT dst, int height, int width) {
    const int hw = height * width;
    const __m256 scale = _mm256_set1_ps(1.0f / 255.0f);
    float* MMLTK_RESTRICT dst_r = dst;
    float* MMLTK_RESTRICT dst_g = dst + hw;
    float* MMLTK_RESTRICT dst_b = dst + static_cast<ptrdiff_t>(hw) * 2;

    static const __m128i shuf_r_a = _mm_setr_epi8(0, 3, 6, 9, 12, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    static const __m128i shuf_r_b = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, 2, 5, 8, 11, 14, -1, -1, -1, -1, -1);
    static const __m128i shuf_r_c = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 1, 4, 7, 10, 13);

    static const __m128i shuf_g_a = _mm_setr_epi8(1, 4, 7, 10, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    static const __m128i shuf_g_b = _mm_setr_epi8(-1, -1, -1, -1, -1, 0, 3, 6, 9, 12, 15, -1, -1, -1, -1, -1);
    static const __m128i shuf_g_c = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 2, 5, 8, 11, 14);

    static const __m128i shuf_b_a = _mm_setr_epi8(2, 5, 8, 11, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    static const __m128i shuf_b_b = _mm_setr_epi8(-1, -1, -1, -1, -1, 1, 4, 7, 10, 13, -1, -1, -1, -1, -1, -1);
    static const __m128i shuf_b_c = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 3, 6, 9, 12, 15);

    int i = 0;
    for (; i + 15 < hw; i += 16) {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 16));
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 32));

        const __m128i r16 = _mm_or_si128(_mm_shuffle_epi8(a, shuf_r_a),
                                         _mm_or_si128(_mm_shuffle_epi8(b, shuf_r_b), _mm_shuffle_epi8(c, shuf_r_c)));
        const __m128i g16 = _mm_or_si128(_mm_shuffle_epi8(a, shuf_g_a),
                                         _mm_or_si128(_mm_shuffle_epi8(b, shuf_g_b), _mm_shuffle_epi8(c, shuf_g_c)));
        const __m128i b16 = _mm_or_si128(_mm_shuffle_epi8(a, shuf_b_a),
                                         _mm_or_si128(_mm_shuffle_epi8(b, shuf_b_b), _mm_shuffle_epi8(c, shuf_b_c)));

        const __m256 r_lo = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(r16)), scale);
        const __m256 r_hi = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_srli_si128(r16, 8))), scale);
        _mm256_storeu_ps(dst_r, r_lo);
        _mm256_storeu_ps(dst_r + 8, r_hi);

        const __m256 g_lo = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(g16)), scale);
        const __m256 g_hi = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_srli_si128(g16, 8))), scale);
        _mm256_storeu_ps(dst_g, g_lo);
        _mm256_storeu_ps(dst_g + 8, g_hi);

        const __m256 b_lo = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(b16)), scale);
        const __m256 b_hi = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_srli_si128(b16, 8))), scale);
        _mm256_storeu_ps(dst_b, b_lo);
        _mm256_storeu_ps(dst_b + 8, b_hi);

        src += 48;
        dst_r += 16;
        dst_g += 16;
        dst_b += 16;
    }
    for (; i < hw; ++i) {
        *dst_r++ = static_cast<float>(src[0]) * (1.0f / 255.0f);
        *dst_g++ = static_cast<float>(src[1]) * (1.0f / 255.0f);
        *dst_b++ = static_cast<float>(src[2]) * (1.0f / 255.0f);
        src += 3;
    }
}

void hwc_uint8_to_nchw_float(const uint8_t* src, float* dst, int height, int width) {
    MMLTK_PROFILE_SCOPE("compiler.pixels.convert.avx2");
    hwc_uint8_to_nchw_float_avx2(src, dst, height, width);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void decode_pixel_image(const std::filesystem::path& split_dir, const WritablePixelRange& pixel_blob,
                        uint32_t image_index, uint32_t target_width, uint32_t target_height,
                        RgbImageResizer& image_resizer, size_t image_stride, std::vector<uint8_t>& resize_scratch) {
    const int width = checked_cast<int>(target_width, "image width too large");
    const int height = checked_cast<int>(target_height, "image height too large");

    const std::filesystem::path img_path = image_path(split_dir, image_index);
    float* dst = pixel_blob.image(image_index, image_stride);

    int raw_width = 0;
    int raw_height = 0;
    int raw_channels = 0;
    uint8_t* raw_pixels = nullptr;
    {
        MMLTK_PROFILE_SCOPE("compiler.pixels.load_png");
        raw_pixels = stbi_load(img_path.c_str(), &raw_width, &raw_height, &raw_channels, 3);
    }
    if (!raw_pixels) {
        throw std::runtime_error("failed to load image file: " + img_path.string());
    }

    const uint8_t* src_pixels = raw_pixels;
    if (static_cast<uint32_t>(raw_width) != target_width || static_cast<uint32_t>(raw_height) != target_height) {
        const size_t resized_bytes = static_cast<size_t>(target_width) * target_height * 3;
        if (resize_scratch.capacity() < resized_bytes) {
            MMLTK_PROFILE_ADD("compiler.pixels.resize_scratch_grows", 1);
            MMLTK_PROFILE_ADD("compiler.pixels.resize_scratch_bytes", resized_bytes);
        }
        {
            MMLTK_PROFILE_SCOPE("compiler.pixels.resize");
            resize_scratch.resize(resized_bytes);
            image_resizer.resize(raw_pixels, raw_width, raw_height, resize_scratch.data(), width, height);
        }
        MMLTK_PROFILE_ADD("compiler.pixels.resize_count", 1);
        src_pixels = resize_scratch.data();
    }

    MMLTK_PROFILE_ADD("compiler.pixels.convert_bytes", static_cast<size_t>(width) * height * 3);
    hwc_uint8_to_nchw_float(src_pixels, dst, height, width);
    stbi_image_free(raw_pixels);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void decode_pixel_worker(const std::filesystem::path& split_dir, const WritablePixelRange& pixel_blob,
                         std::atomic<uint32_t>& next_image, uint32_t num_images, uint32_t target_width,
                         uint32_t target_height, size_t image_stride, int resize_threads_per_image,
                         ProgressCounter* completed_images) {
    MMLTK_PROFILE_SCOPE("compiler.pixels.decode_worker");
    RgbImageResizer image_resizer(resize_threads_per_image);
    std::vector<uint8_t> resize_scratch;
    while (true) {
        const uint32_t image_index = next_image.fetch_add(1, std::memory_order_relaxed);
        if (image_index >= num_images) {
            break;
        }
        if (completed_images != nullptr) {
            completed_images->begin_work();
        }
        try {
            decode_pixel_image(split_dir, pixel_blob, image_index, target_width, target_height, image_resizer,
                               image_stride, resize_scratch);
            if (completed_images != nullptr) {
                completed_images->finish_work();
            }
        } catch (...) {
            if (completed_images != nullptr) {
                completed_images->end_work();
            }
            throw;
        }
    }
}
// NOLINTEND(bugprone-easily-swappable-parameters)

#undef MMLTK_RESTRICT

}  // namespace

void write_pixel_blob(const FileHandle& fd, const PixelBlobWriteRequest& request) {
    MMLTK_PROFILE_SCOPE("compiler.pixels.write_blob");
    const size_t pixel_bytes = static_cast<size_t>(request.num_images) * request.image_stride;
    MMLTK_PROFILE_SET("compiler.pixels.total_bytes", pixel_bytes);
    WritablePixelRange pixel_blob(fd, request.pixel_offset, pixel_bytes);

    if (request.num_images == 0) {
        return;
    }

    const ResizeWorkerPlan resize_plan =
        plan_rgb_resize_workers(request.num_workers, request.any_resize, request.any_downscale);
    MMLTK_PROFILE_SET("compiler.pixels.worker.count", static_cast<size_t>(resize_plan.image_workers));
    MMLTK_PROFILE_SET("compiler.pixels.image_workers", static_cast<size_t>(resize_plan.image_workers));
    MMLTK_PROFILE_SET("compiler.pixels.resize_threads_per_image",
                      static_cast<size_t>(resize_plan.resize_threads_per_image));
    std::atomic<uint32_t> next_image{0};

    parallel_for_range_indexed<int>(0, resize_plan.image_workers, resize_plan.image_workers, [&](int, int, int) {
        decode_pixel_worker(request.split_dir, pixel_blob, next_image, request.num_images, request.width,
                            request.height, request.image_stride, resize_plan.resize_threads_per_image,
                            request.completed_images);
    });
}

}  // namespace mmltk::compiler_internal
