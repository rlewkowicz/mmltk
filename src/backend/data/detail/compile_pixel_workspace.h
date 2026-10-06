#pragma once
#include "src/backend/data/detail/compile_image_decoder.h"
#include "src/backend/data/detail/compile_resources.h"
#include "src/backend/imaging/resample/image_resize.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::backend::data::detail {
enum class CompilePixelStage : std::uint8_t { Validation, Preparation, Decode, Destination, ResizeStore, Complete };
struct CompilePixelObservation {
 CompileImageEncoding codec = CompileImageEncoding::Stb;
 imaging::resample::RgbResizePath path = imaging::resample::RgbResizePath::Identity;
 imaging::resample::RgbResizeMethod method = imaging::resample::RgbResizeMethod::Native;
 std::uint32_t source_width = 0, source_height = 0, target_width = 0, target_height = 0;
 std::uint64_t encoded_bytes = 0, requested_bytes = 0, opaque_bytes = 0, output_bytes = 0;
 std::uint64_t validation_ns = 0, preparation_ns = 0, decode_ns = 0, destination_ns = 0, resize_store_ns = 0, duration_ns = 0;
 bool failed = false;
 CompilePixelStage stage = CompilePixelStage::Validation;
 std::uint64_t failed_stage_ns = 0;
 std::uint64_t resize_preparations = 0, resize_allocation_requests = 0, resize_allocation_bytes = 0;
 std::uint64_t cpu_ns = 0;
 bool cpu_incomplete = false;
};
MMLTK_REFLECT_FIELDS(CompilePixelObservation)
using CompilePixelObserver = std::function<void(const CompilePixelObservation&)>;
struct CompilePixelPolicy {
 std::uint32_t width = 0, height = 0;
 imaging::resample::ImageResizeMode mode = imaging::resample::ImageResizeMode::Stretch;
 bool perceptual = false;
 imaging::resample::RgbResizeMethod method = imaging::resample::RgbResizeMethod::Native;
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
 [[nodiscard]] const imaging::resample::RgbResizeWorkspacePlan& resize_plan() const noexcept { return resize_; }

private:
 friend class CompilePixelWorkspace;
 friend CompilePixelWorkspacePlan compile_pixel_workspace_plan(const CompileImageHeader&, CompilePixelPolicy, imaging::resample::RgbResizeWork*);
 CompilePixelWorkspacePlan(const CompileImageHeader&, CompilePixelPolicy, imaging::resample::RgbResizeWork*);
 CompileImageHeader header_;
 CompilePixelPolicy policy_;
 CompileImagePlan decoder_;
 imaging::resample::RgbResizeWorkspacePlan resize_;
 std::size_t decoder_extent_ = 0;
 CompileResources resources_;
 imaging::resample::RgbImageLayout destination_;
};
[[nodiscard]] CompilePixelWorkspacePlan compile_pixel_workspace_plan(const CompileImageHeader&, CompilePixelPolicy, imaging::resample::RgbResizeWork* observed = nullptr);
[[nodiscard]] CompilePixelWorkspacePlan compile_pixel_workspace_plan(
 const CompileImageHeader&, std::uint32_t resolution, imaging::resample::ImageResizeMode, bool perceptual, imaging::resample::RgbResizeMethod = imaging::resample::RgbResizeMethod::Native);
// Session-owned controls bind only to the current arena. Pixels go directly
// into the prepared destination; neither controls nor borrowed scratch escape.
// A supplied storage preparation runs after successful decoding, before stores.
class CompilePixelWorkspace final {
public:
 // Both optional observations borrow their targets only for this synchronous
 // call. Resize aggregates require no detail emitter or per-pixel clocks.
 void write(const CompilePixelWorkspacePlan&, std::span<const std::uint8_t> encoded, const CompileImageHeader&, std::span<std::byte> workspace, imaging::resample::RgbMutableImageView destination,
  std::function_ref<void()> prepare_destination = +[] {}, const CompilePixelObserver& observer = {}, imaging::resample::RgbResizeWork* resize_observed = nullptr);

private:
 CompileImageDecoder decoder_;
 imaging::resample::RgbImageResizer resizer_{1, false};
};
}  // namespace mmltk::backend::data::detail
