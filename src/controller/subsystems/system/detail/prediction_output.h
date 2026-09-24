#pragma once
#include <filesystem>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include "src/controller/contracts/prediction_output.h"
#include "src/controller/contracts/workflow_output.h"
#include "src/controller/subsystems/system/compute_runtime.h"
#include "src/backend/models/rfdetr/inference/prediction_delivery.h"
#include "prediction_preview.h"
#include "src/backend/imaging/raster/rendered_image_writer.h"
namespace mmltk::controller { struct PredictionRunOutput; }
namespace mmltk::controller::detail {

class PredictionOutput final {
public:
 PredictionOutput(DirectComputeConfiguration, mmltk::backend::models::rfdetr::PredictSourceKind, PredictionRunOutput, std::optional<mmltk::frameworks::gpu::DeviceContext>,
  mmltk::backend::imaging::raster::RenderedImageWriter::PngEncoder = {}, std::optional<std::uint64_t> seed = {});
 ~PredictionOutput();
 [[nodiscard]] bool Enabled() const noexcept;
 [[nodiscard]] bool Wants(std::int64_t index);
 void Begin(const mmltk::backend::models::rfdetr::PredictionRunResult&);
 void Media(const mmltk::backend::media::video::VideoMediaInfo&);
 void Audio(const mmltk::backend::media::video::VideoAudioPacket&);
 [[nodiscard]] std::shared_ptr<const PredictionPreviewFrame> Capture(const mmltk::backend::models::rfdetr::PredictionRecord&, mmltk::backend::models::rfdetr::PredictionPixels,
  const mmltk::backend::ml::runtime::AnalysisAnnotationStorage&);
 void Finish(bool success);
private:
 struct Impl;
 std::shared_ptr<Impl> impl_;
};
}
