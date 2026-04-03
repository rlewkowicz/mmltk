#include "image_resize.h"

#include "avir.h"
#include "avir_float4_sse.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace mmltk {

struct RgbImageResizer::Impl {
    // For 3-channel RGB on this x86_64 build, AVIR's interleaved float4 SIMD path
    // outperformed both plain float and float8_dil in local benchmarks.
    avir::CImageResizer<avir::fpclass_float4> resizer{8};
};

ResizeWorkerPlan plan_rgb_resize_workers(int total_workers,
                                         bool any_resize,
                                         bool any_downscale) {
    (void) any_resize;
    (void) any_downscale;
    return {std::max(1, total_workers), 1};
}

RgbImageResizer::RgbImageResizer(int thread_count) {
    if (thread_count != 1) {
        throw std::runtime_error(
            "RgbImageResizer internal threading is disabled; use image-level parallelism and pass thread_count=1");
    }
    impl_ = std::make_unique<Impl>();
}

RgbImageResizer::~RgbImageResizer() = default;

RgbImageResizer::RgbImageResizer(RgbImageResizer&&) noexcept = default;

RgbImageResizer& RgbImageResizer::operator=(RgbImageResizer&&) noexcept = default;

void RgbImageResizer::resize(const uint8_t* src,
                             int src_width,
                             int src_height,
                             uint8_t* dst,
                             int dst_width,
                             int dst_height) {
    if (src == nullptr || dst == nullptr) {
        throw std::runtime_error("RgbImageResizer requires non-null input and output buffers");
    }
    if (src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0) {
        throw std::runtime_error("RgbImageResizer dimensions must be positive");
    }
    if (src_width == dst_width && src_height == dst_height) {
        std::memcpy(dst,
                    src,
                    static_cast<size_t>(src_width) * static_cast<size_t>(src_height) * 3);
        return;
    }

    impl_->resizer.resizeImage(src,
                               src_width,
                               src_height,
                               0,
                               dst,
                               dst_width,
                               dst_height,
                               3,
                               0.0,
                               nullptr);
}

} // namespace mmltk
