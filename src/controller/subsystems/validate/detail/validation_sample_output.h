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
namespace mmltk::controller::detail {
// Run selection and save policy stay with Validation. One source capture feeds
// required native output and optional presentation through immutable handles.
class ValidationSampleOutput final {
public:
 ValidationSampleOutput(DirectComputeConfiguration, VisualDeviceSettings, ComputeArtifactSink, mmltk::backend::imaging::raster::RenderedImageWriter::PngEncoder = {});
 ~ValidationSampleOutput();
 void Begin(std::filesystem::path, contracts::ValidationRunPreview, std::span<const std::uint32_t>);
 void UseCaptureContext(mmltk::frameworks::gpu::DeviceContext);
 [[nodiscard]] std::shared_ptr<const PredictionPreviewFrame> Capture(mmltk::backend::models::rfdetr::ValidationSampleView);
 void Finish(bool require_complete);

private:
 class Impl;
 std::shared_ptr<Impl> impl_;
};
}  // namespace mmltk::controller::detail
