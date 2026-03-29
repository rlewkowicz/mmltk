#pragma once

#include <cstdint>
#include <memory>

namespace fastloader {

struct ResizeWorkerPlan {
    int image_workers = 1;
    int resize_threads_per_image = 1;
};

ResizeWorkerPlan plan_rgb_resize_workers(int total_workers,
                                         bool any_resize,
                                         bool any_downscale);

class RgbImageResizer {
public:
    explicit RgbImageResizer(int thread_count = 1);
    ~RgbImageResizer();

    RgbImageResizer(const RgbImageResizer&) = delete;
    RgbImageResizer& operator=(const RgbImageResizer&) = delete;
    RgbImageResizer(RgbImageResizer&&) noexcept;
    RgbImageResizer& operator=(RgbImageResizer&&) noexcept;

    void resize(const uint8_t* src,
                int src_width,
                int src_height,
                uint8_t* dst,
                int dst_width,
                int dst_height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fastloader
