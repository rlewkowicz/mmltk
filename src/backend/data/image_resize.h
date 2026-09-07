#pragma once

#include <cstdint>
#include <memory>

namespace mmltk::backend::data {

struct RgbLetterbox {
    std::uint32_t resized_width = 0;
    std::uint32_t resized_height = 0;
    std::uint32_t offset_x = 0;
    std::uint32_t offset_y = 0;
};

[[nodiscard]] RgbLetterbox compute_rgb_letterbox(std::uint32_t source_width, std::uint32_t source_height, std::uint32_t target_width,
                                                 std::uint32_t target_height);

void rgb_hwc_u8_to_nchw_f32(const std::uint8_t* source, float* destination, std::uint32_t width, std::uint32_t height);

void letterboxed_rgb_hwc_u8_to_nchw_f32(const std::uint8_t* source, float* destination, std::uint32_t source_width,
                                        std::uint32_t source_height, std::uint32_t destination_width, std::uint32_t destination_height,
                                        std::uint32_t offset_x, std::uint32_t offset_y);

struct ResizeWorkerPlan {
    int image_workers = 1;
    int resize_threads_per_image = 1;
};

ResizeWorkerPlan plan_rgb_resize_workers(int total_workers, bool any_resize, bool any_downscale);

class RgbImageResizer {
   public:
    explicit RgbImageResizer(int thread_count = 1);
    ~RgbImageResizer();

    RgbImageResizer(const RgbImageResizer&) = delete;
    RgbImageResizer& operator=(const RgbImageResizer&) = delete;
    RgbImageResizer(RgbImageResizer&&) noexcept;
    RgbImageResizer& operator=(RgbImageResizer&&) noexcept;

    void resize(const uint8_t* src, int src_width, int src_height, uint8_t* dst, int dst_width, int dst_height);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmltk::backend::data
