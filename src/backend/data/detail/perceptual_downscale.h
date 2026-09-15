// SPDX-License-Identifier: MIT
// Öztireli/Gross (2015) perceptual downscaling; provenance in perceptual_downscale_math.h.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "src/backend/data/image_resize.h"
#include "src/backend/data/detail/perceptual_downscale_math.h"

namespace mmltk::backend::data::perceptual {
std::size_t checked_product(std::size_t a, std::size_t b);
std::size_t validate_view(const void* pointer, const RgbImageLayout& layout);
struct ValidatedResize {
    std::size_t source_extent,destination_extent;
    bool identity;
    explicit operator bool() const noexcept {return identity;}
};
// Shared byte/stride arithmetic supplies physical admission with exact spans.
ValidatedResize validate_pair(RgbConstImageView source, RgbMutableImageView destination);
void copy_identity(RgbConstImageView source, RgbMutableImageView destination);

class CpuDownscaler final {
 public:
    CpuDownscaler();
    void run(RgbConstImageView source, RgbMutableImageView destination);
 private:
    void prepare(const RgbImageLayout& source, const RgbImageLayout& destination);
    template<RgbPixelFormat Format, bool Integer> void execute(RgbConstImageView source, RgbMutableImageView destination);
    TransferTable transfer_;
    std::uint32_t source_width_ = 0, source_height_ = 0, width_ = 0, height_ = 0;
    std::vector<Footprint> x_, y_;
    std::array<std::vector<Moment>, 2> moments_;
    std::array<std::vector<Coefficient>, 2> coefficients_;
    std::array<std::vector<float>, 2> alpha_;
};
} // namespace mmltk::backend::data::perceptual
