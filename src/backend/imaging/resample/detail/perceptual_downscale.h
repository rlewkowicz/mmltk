// SPDX-License-Identifier: MIT
// Öztireli/Gross (2015) perceptual downscaling; provenance in perceptual_downscale_math.h.
#pragma once
#include "src/common/types/workspace_buffer.h"
#include <span>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "src/backend/imaging/resample/image_resize.h"
#include "src/backend/imaging/resample/detail/perceptual_downscale_math.h"
namespace mmltk::backend::imaging::resample::perceptual {
void copy_identity(RgbConstImageView source, RgbMutableImageView destination);
// CPU-only projection of the scalar transfer authority; shared immutably by
// every downscaler, independently of supplied image storage and CUDA tables.
struct CpuTransferTable {
 static constexpr unsigned bucket_count = 4096;
 TransferTable thresholds;
 std::array<std::uint32_t, bucket_count + 1> base_code;
};
class CpuDownscaler final {
public:
 CpuDownscaler();
 [[nodiscard]] static std::size_t workspace_bytes(std::uint32_t width, std::uint32_t height, bool alpha);
 void bind(std::uint32_t width, std::uint32_t height, bool alpha, std::span<std::byte>);
 [[nodiscard]] std::size_t retained_bytes() const noexcept;
 void run(RgbConstImageView source, RgbMutableImageView destination);
 void run_quantized_planar(RgbConstImageView source, RgbMutableImageView destination);

private:
 friend class mmltk::backend::imaging::resample::RgbImageResizer;
 friend struct CpuDownscalerTestAccess;
 void run_selected(RgbConstImageView, RgbMutableImageView, RgbResizePath, bool quantized_planar = false, RgbResizeWork* observed = nullptr);
 void quantize_block(const float*, std::uint8_t*) const;
 // Owner-local, one-shot preparation seam; never exposed by the public
 // resizer or consulted during pixel execution.
 enum class PreparationStep : std::uint8_t { None, HorizontalAxis, VerticalAxis, FirstMoment, SecondMoment, FirstCoefficient, SecondCoefficient, FirstAlpha, SecondAlpha };
 PreparationStep fail_before_ = PreparationStep::None;
 void preparation_checkpoint(PreparationStep step);
 void prepare(const RgbImageLayout& source, const RgbImageLayout& destination, RgbResizeWork* observed);
 template <RgbPixelFormat Format, bool Integer, bool QuantizedPlanar = false>
 void execute(RgbConstImageView source, RgbMutableImageView destination);
 const CpuTransferTable& transfer_;
 bool prepared_ = false;
 std::uint32_t source_width_ = 0, source_height_ = 0, width_ = 0, height_ = 0;
 mmltk::common::types::WorkspaceBuffer<Footprint> x_, y_;
 std::array<mmltk::common::types::WorkspaceBuffer<float>, 2> moments_;
 std::array<mmltk::common::types::WorkspaceBuffer<float>, 2> coefficients_;
 std::array<mmltk::common::types::WorkspaceBuffer<float>, 2> alpha_;
};
}  // namespace mmltk::backend::imaging::resample::perceptual
