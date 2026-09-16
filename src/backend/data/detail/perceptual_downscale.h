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
void copy_identity(RgbConstImageView source, RgbMutableImageView destination);
class CpuDownscaler final {
   public:
    CpuDownscaler();
    void run(RgbConstImageView source, RgbMutableImageView destination);

   private:
    friend struct CpuDownscalerTestAccess;
    // Owner-local, one-shot preparation seam; never exposed by the public
    // resizer or consulted during pixel execution.
    enum class PreparationStep : std::uint8_t {
        None,
        HorizontalAxis,
        VerticalAxis,
        FirstMoment,
        SecondMoment,
        FirstCoefficient,
        SecondCoefficient,
        FirstAlpha,
        SecondAlpha
    };
    PreparationStep fail_before_ = PreparationStep::None;
    void preparation_checkpoint(PreparationStep step);
    void prepare(const RgbImageLayout& source, const RgbImageLayout& destination);
    template <RgbPixelFormat Format, bool Integer>
    void execute(RgbConstImageView source, RgbMutableImageView destination);
    TransferTable transfer_;
    bool prepared_ = false;
    std::uint32_t source_width_ = 0, source_height_ = 0, width_ = 0, height_ = 0;
    std::vector<Footprint> x_, y_;
    std::array<std::vector<Moment>, 2> moments_;
    std::array<std::vector<Coefficient>, 2> coefficients_;
    std::array<std::vector<float>, 2> alpha_;
};
}  // namespace mmltk::backend::data::perceptual
