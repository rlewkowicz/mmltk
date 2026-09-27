#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include "src/controller/contracts/validation_display.h"
#include "src/controller/subsystems/system/compute_runtime.h"
#include "src/controller/presentation/visual_runtime.h"
#include "src/backend/models/rfdetr/inference/validate.h"
#include "src/backend/imaging/raster/rendered_image_writer.h"
#include "src/controller/subsystems/system/detail/prediction_preview.h"
#include "src/frameworks/gpu/cuda/cuda_context_scope.h"
#include "src/frameworks/gpu/cuda/terminal_cuda_retirement_owner.h"
namespace mmltk::controller::detail {
// Run selection and save policy stay with Validation. One source capture feeds
// required native output and optional presentation through immutable handles.
class ValidationSampleOutput final {
public:
 ValidationSampleOutput(DirectComputeConfiguration, VisualDeviceSettings, ComputeArtifactSink, mmltk::backend::imaging::raster::RenderedImageWriter::PngEncoder = {},
  std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> = {}, mmltk::frameworks::gpu::CudaContextApi = {});
 ~ValidationSampleOutput();
 void Begin(std::filesystem::path, contracts::ValidationRunPreview, std::span<const std::uint32_t>, DirectComputeConfiguration = {});
 void UseCaptureContext(mmltk::frameworks::gpu::DeviceContext);
 [[nodiscard]] std::shared_ptr<const PredictionPreviewFrame> Capture(mmltk::backend::models::rfdetr::ValidationSampleView);
 void Finish(bool require_complete);
 [[nodiscard]] bool HasUnsafeCustody() const noexcept;
 [[nodiscard]] const std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner>& RetirementAuthority() const noexcept;

private:
 class Impl;
 std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement_;
 std::shared_ptr<Impl> impl_;
};
}  // namespace mmltk::controller::detail
