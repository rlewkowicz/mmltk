#pragma once
#include "src/backend/data/detail/compile_image_decoder.h"
#include "src/backend/data/detail/compile_resources.h"
#include "src/backend/imaging/resample/image_resize.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
namespace mmltk::backend::data::detail {
struct CompilePixelPolicy {
 std::uint32_t width = 0, height = 0;
 imaging::resample::ImageResizeMode mode = imaging::resample::ImageResizeMode::Stretch;
 bool perceptual = false;
 bool operator==(const CompilePixelPolicy&) const = default;
};
// Factory-checked immutable value: the resource demand and execution geometry
// cannot be changed independently. Image/source custody stays with the caller.
class CompilePixelWorkspacePlan final {
public:
 [[nodiscard]] const CompileImageHeader& header() const noexcept { return header_; }
 [[nodiscard]] CompilePixelPolicy policy() const noexcept { return policy_; }
 [[nodiscard]] CompileResources resources() const noexcept { return resources_; }
 [[nodiscard]] std::size_t bytes() const noexcept { return resources_.bytes; }
 [[nodiscard]] const imaging::resample::RgbImageLayout& destination() const noexcept { return destination_; }

private:
 friend class CompilePixelWorkspace;
 friend CompilePixelWorkspacePlan compile_pixel_workspace_plan(const CompileImageHeader&, CompilePixelPolicy);
 CompilePixelWorkspacePlan(const CompileImageHeader&, CompilePixelPolicy);
 CompileImageHeader header_;
 CompilePixelPolicy policy_;
 CompileImagePlan decoder_;
 imaging::resample::RgbResizeWorkspacePlan resize_;
 std::size_t decoder_extent_ = 0;
 CompileResources resources_;
 imaging::resample::RgbImageLayout destination_;
};
[[nodiscard]] CompilePixelWorkspacePlan compile_pixel_workspace_plan(const CompileImageHeader&, CompilePixelPolicy);
[[nodiscard]] CompilePixelWorkspacePlan compile_pixel_workspace_plan(const CompileImageHeader&, std::uint32_t resolution, imaging::resample::ImageResizeMode, bool perceptual);
// Session-owned controls bind only to the current arena. Pixels go directly
// into the prepared destination; neither controls nor borrowed scratch escape.
// A supplied storage preparation runs after successful decoding, before stores.
class CompilePixelWorkspace final {
public:
 void write(const CompilePixelWorkspacePlan&, std::span<const std::uint8_t> encoded, const CompileImageHeader&, std::span<std::byte> workspace, imaging::resample::RgbMutableImageView destination,
  std::function_ref<void()> prepare_destination = +[] {});

private:
 CompileImageDecoder decoder_;
 imaging::resample::RgbImageResizer resizer_{1, false};
};
}  // namespace mmltk::backend::data::detail
