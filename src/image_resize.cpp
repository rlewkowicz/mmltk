#include "image_resize.h"

#include "avir.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace mmltk {

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
};

ResizeWorkerPlan plan_rgb_resize_workers(int total_workers, bool any_resize, bool any_downscale) {
    (void)any_resize;
    (void)any_downscale;
    return {std::max(1, total_workers), 1};
}

RgbImageResizer::RgbImageResizer(int thread_count) {
    if (thread_count != 1) {
        throw std::runtime_error(
            "RgbImageResizer internal threading is disabled; use image-level parallelism and pass thread_count=1");
    }
    static std::once_flag avir_warmup_once;
    std::call_once(avir_warmup_once, warm_up_avir_rgb_resize_path);
    impl_ = std::make_unique<Impl>();
}

RgbImageResizer::~RgbImageResizer() = default;

RgbImageResizer::RgbImageResizer(RgbImageResizer&&) noexcept = default;

RgbImageResizer& RgbImageResizer::operator=(RgbImageResizer&&) noexcept = default;

void RgbImageResizer::resize(const uint8_t* src, int src_width, int src_height, uint8_t* dst, int dst_width,
                             int dst_height) {
    if (src == nullptr || dst == nullptr) {
        throw std::runtime_error("RgbImageResizer requires non-null input and output buffers");
    }
    if (src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0) {
        throw std::runtime_error("RgbImageResizer dimensions must be positive");
    }
    if (src_width == dst_width && src_height == dst_height) {
        std::memcpy(dst, src, static_cast<size_t>(src_width) * static_cast<size_t>(src_height) * 3);
        return;
    }

    impl_->resizer.resizeImage(src, src_width, src_height, 0, dst, dst_width, dst_height, 3, 0.0, nullptr);
}

}  // namespace mmltk
