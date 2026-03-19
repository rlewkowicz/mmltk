#include "dataset_compiler_internal.h"
#include "profile_utils.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

namespace fastloader::compiler_internal {

namespace {

#if defined(__GNUC__) || defined(__clang__)
#define FASTLOADER_RESTRICT __restrict__
#else
#define FASTLOADER_RESTRICT
#endif

class WritablePixelRange {
public:
    WritablePixelRange(const FileHandle& fd, size_t offset, size_t bytes) : bytes_(bytes) {
        if (bytes_ == 0) {
            return;
        }
        void* ptr = mmap(nullptr,
                         bytes_,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         fd.get(),
                         checked_cast<off_t>(offset, "pixel mmap offset overflow"));
        if (ptr == MAP_FAILED) {
            throw errno_error("mmap failed");
        }
        data_ = static_cast<float*>(ptr);
    }

    WritablePixelRange(const WritablePixelRange&) = delete;
    WritablePixelRange& operator=(const WritablePixelRange&) = delete;

    WritablePixelRange(WritablePixelRange&& other) noexcept
        : data_(other.data_), bytes_(other.bytes_) {
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

    ~WritablePixelRange() { reset(); }

    float* image(uint32_t image_index, size_t image_stride) const {
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

void hwc_uint8_to_nchw_float_scalar(const uint8_t* FASTLOADER_RESTRICT src,
                                    float* FASTLOADER_RESTRICT dst,
                                    int height,
                                    int width) {
    const int hw = height * width;
    const float scale = 1.0f / 255.0f;
    float* FASTLOADER_RESTRICT dst_r = dst;
    float* FASTLOADER_RESTRICT dst_g = dst + hw;
    float* FASTLOADER_RESTRICT dst_b = dst + static_cast<ptrdiff_t>(hw) * 2;
    int i = 0;
    for (; i + 3 < hw; i += 4) {
        dst_r[0] = static_cast<float>(src[0]) * scale;
        dst_g[0] = static_cast<float>(src[1]) * scale;
        dst_b[0] = static_cast<float>(src[2]) * scale;
        dst_r[1] = static_cast<float>(src[3]) * scale;
        dst_g[1] = static_cast<float>(src[4]) * scale;
        dst_b[1] = static_cast<float>(src[5]) * scale;
        dst_r[2] = static_cast<float>(src[6]) * scale;
        dst_g[2] = static_cast<float>(src[7]) * scale;
        dst_b[2] = static_cast<float>(src[8]) * scale;
        dst_r[3] = static_cast<float>(src[9]) * scale;
        dst_g[3] = static_cast<float>(src[10]) * scale;
        dst_b[3] = static_cast<float>(src[11]) * scale;
        src += 12;
        dst_r += 4;
        dst_g += 4;
        dst_b += 4;
    }
    for (; i < hw; ++i) {
        dst_r[0] = static_cast<float>(src[0]) * scale;
        dst_g[0] = static_cast<float>(src[1]) * scale;
        dst_b[0] = static_cast<float>(src[2]) * scale;
        src += 3;
        ++dst_r;
        ++dst_g;
        ++dst_b;
    }
}

void hwc_uint8_to_nchw_float(const uint8_t* src, float* dst, int height, int width) {
    FASTLOADER_PROFILE_SCOPE("compiler.pixels.convert.scalar");
    hwc_uint8_to_nchw_float_scalar(src, dst, height, width);
}

void decode_pixel_range(const std::filesystem::path& split_dir,
                        const WritablePixelRange& pixel_blob,
                        uint32_t image_start,
                        uint32_t image_end,
                        uint32_t target_width,
                        uint32_t target_height,
                        size_t image_stride,
                        std::vector<uint8_t>& resize_scratch,
                        uint32_t num_images_total,
                        std::atomic<size_t>& completed_images,
                        std::mutex& progress_mtx,
                        const std::function<void(size_t, size_t)>& progress_cb) {
    FASTLOADER_PROFILE_SCOPE("compiler.pixels.decode_range");
    const int width = checked_cast<int>(target_width, "image width too large");
    const int height = checked_cast<int>(target_height, "image height too large");

    for (uint32_t image_index = image_start; image_index < image_end; ++image_index) {
        const std::filesystem::path img_path = image_path(split_dir, image_index);
        float* dst = pixel_blob.image(image_index, image_stride);

        int raw_width = 0;
        int raw_height = 0;
        int raw_channels = 0;
        uint8_t* raw_pixels = nullptr;
        {
            FASTLOADER_PROFILE_SCOPE("compiler.pixels.load_png");
            raw_pixels = stbi_load(img_path.c_str(), &raw_width, &raw_height, &raw_channels, 3);
        }
        if (!raw_pixels) {
            throw std::runtime_error("failed to load image file: " + img_path.string());
        }

        const uint8_t* src_pixels = raw_pixels;
        if (static_cast<uint32_t>(raw_width) != target_width ||
            static_cast<uint32_t>(raw_height) != target_height) {
            const size_t resized_bytes = static_cast<size_t>(target_width) * target_height * 3;
            if (resize_scratch.capacity() < resized_bytes) {
                FASTLOADER_PROFILE_ADD("compiler.pixels.resize_scratch_grows", 1);
                FASTLOADER_PROFILE_ADD("compiler.pixels.resize_scratch_bytes", resized_bytes);
            }
            {
                FASTLOADER_PROFILE_SCOPE("compiler.pixels.resize");
                resize_scratch.resize(resized_bytes);
                stbir_resize_uint8_linear(
                    raw_pixels,
                    raw_width,
                    raw_height,
                    0,
                    resize_scratch.data(),
                    width,
                    height,
                    0,
                    static_cast<stbir_pixel_layout>(3));
            }
            FASTLOADER_PROFILE_ADD("compiler.pixels.resize_count", 1);
            src_pixels = resize_scratch.data();
        }

        FASTLOADER_PROFILE_ADD("compiler.pixels.convert_bytes",
                               static_cast<size_t>(width) * height * 3);
        hwc_uint8_to_nchw_float(src_pixels, dst, height, width);
        stbi_image_free(raw_pixels);

        if (progress_cb) {
            const size_t done = completed_images.fetch_add(1, std::memory_order_relaxed) + 1;
            if (done % 100 == 0 || done == num_images_total) {
                std::lock_guard<std::mutex> lock(progress_mtx);
                progress_cb(done, num_images_total);
            }
        }
    }
}

#undef FASTLOADER_RESTRICT

} // namespace

void write_pixel_blob(const FileHandle& fd,
                      const std::filesystem::path& split_dir,
                      uint32_t num_images,
                      uint32_t width,
                      uint32_t height,
                      size_t image_stride,
                      int num_workers,
                      size_t pixel_offset,
                      const std::function<void(size_t, size_t)>& progress_cb) {
    FASTLOADER_PROFILE_SCOPE("compiler.pixels.write_blob");
    const size_t pixel_bytes = static_cast<size_t>(num_images) * image_stride;
    FASTLOADER_PROFILE_SET("compiler.pixels.total_bytes", pixel_bytes);
    WritablePixelRange pixel_blob(fd, pixel_offset, pixel_bytes);

    if (num_images == 0) {
        if (progress_cb) {
            progress_cb(0, 0);
        }
        return;
    }

    FASTLOADER_PROFILE_SET("compiler.pixels.chunk_size", num_images);

    std::atomic<size_t> completed_images{0};
    std::mutex progress_mtx;
    parallel_for_range<uint32_t>(0, num_images, num_workers, [&](uint32_t local_start,
                                                                 uint32_t local_end) {
        FASTLOADER_PROFILE_ADD("compiler.pixels.chunk_count", 1);
        std::vector<uint8_t> resize_scratch;
        decode_pixel_range(split_dir,
                           pixel_blob,
                           local_start,
                           local_end,
                           width,
                           height,
                           image_stride,
                           resize_scratch,
                           num_images,
                           completed_images,
                           progress_mtx,
                           progress_cb);
    });
}

} // namespace fastloader::compiler_internal
