#include "validation_sample_output.h"
#include "src/backend/imaging/raster/caption_raster.h"
#include "src/backend/imaging/raster/class_palette.h"
#include "src/backend/imaging/raster/rendered_image_writer.h"
#include "src/frameworks/gpu/image_failure.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>
namespace mmltk::controller::detail {
namespace gpu = mmltk::frameworks::gpu;
namespace rfdetr = mmltk::backend::models::rfdetr;
namespace raster = mmltk::backend::imaging::raster;
class ValidationSampleOutput::Impl final {
public:
 Impl(DirectComputeConfiguration configuration, VisualDeviceSettings visual, ComputeArtifactSink published, raster::RenderedImageWriter::PngEncoder encoder)
 : configuration_(std::move(configuration)), visual_(visual), published_(std::move(published)), encoder_(std::move(encoder)) {}
 void Ensure() {
  if (pool_) return;
  const auto execution = context_ ? *context_->execution() : visual_.valid() ? resolve_visual_device_execution(visual_) : configuration_.execution.value_or(gpu::DeviceExecution{});
  if (execution.device < 0) throw std::runtime_error("validation sample output has no compute device");
  if (!context_) context_.emplace(CreatePredictionPreviewContext(execution, retirement_));
  pool_ = std::make_unique<PredictionPreviewPool>(execution, *context_, PredictionPreviewPool::TransferOperations{&cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize, &cuMemHostRegister}, retirement_, 18U);
 }
 void Flush() {
  if (!writer_) return;
  const auto completed = writer_->Flush();
  if (!completed.empty() && published_) published_(completed);
 }
 void Draw(const std::shared_ptr<const PredictionPreviewFrame>& raw, VisualExtent extent, std::uint32_t index) {
  Flush();
  if (!runtime_) {
   runtime_ = std::make_unique<gpu::SystemImageRuntime>(gpu::SystemImageRuntimeConfig{.device = context_->device(), .output_layout = gpu::ImageProductLayout::CleanAndSemantic,
    .numa_node = context_->execution()->placement.numa_node, .execution = *context_->execution(), .adopted_context = *context_});
  }
  runtime_->BindContext();
  if (!captions_) captions_ = std::make_unique<raster::CaptionRaster>(*context_);
  if (!writer_) writer_ = std::make_unique<raster::RenderedImageWriter>(*context_,encoder_);
  // Prepare settles changed catalog uploads before any output candidate can
  // publish; Draw batches descriptor upload and painting on its image stream.
  captions_->Prepare(raw->classes());
  labels_.clear();
  labels_.reserve(raw->ground_truth().size()+raw->predictions().size());
  const auto append = [&](auto predictions, bool ground_truth) {
   for (const auto& prediction : predictions) {
    if (!ground_truth && prediction.score < options_.display.confidence_threshold) continue;
    const auto category = prediction.class_reference;
    if (category < 0 || static_cast<std::size_t>(category) >= raw->classes().size()) throw std::invalid_argument("validation caption category is absent");
    std::array<std::uint8_t,3> color{};
    raster::color::class_color(category, raw->class_count(), color[0], color[1], color[2]);
    if (ground_truth) for (auto& value : color) value = 255U - value;
    const auto coordinate = [](float value) {
     const double position = value;
     if (!std::isfinite(position) || position < std::numeric_limits<int>::min() || position > std::numeric_limits<int>::max())
      throw std::invalid_argument("validation caption coordinate is not finite and int-representable");
     return static_cast<int>(std::floor(position));
    };
    const int x = coordinate(prediction.bbox_xyxy[0]), y = coordinate(prediction.bbox_xyxy[1]);
    labels_.push_back({static_cast<std::uint32_t>(category), x, y > static_cast<int>(raster::kNativeCaptionHeight) ? y - static_cast<int>(raster::kNativeCaptionHeight) : 0, color});
   }
  };
  const auto& overlays = options_.overlays;
  if (overlays.ground_truth_layer && options_.ground_truth_labels) append(raw->ground_truth(), true);
  if (overlays.prediction_layer && options_.prediction_labels) append(raw->predictions(), false);
  auto candidate = runtime_->AcquireOutput();
  const std::array regions{PredictionPreviewComposition::Region{raw, {0U,0U,extent.width,extent.height}}};
  PredictionPreviewComposition::Draw(*runtime_, candidate, extent, regions,
   {overlays.prediction_layer && overlays.prediction_boxes, overlays.prediction_layer && overlays.prediction_masks,
    overlays.ground_truth_layer && overlays.ground_truth_boxes, overlays.ground_truth_layer && overlays.ground_truth_masks, true, false, options_.display.confidence_threshold}, nullptr,
   [&](auto clean, auto semantic, auto stream) { raster::CaptionRaster::Composite(clean, semantic, stream); captions_->Draw(clean, labels_, stream); });
  static_cast<void>(runtime_->CommitOutput(std::move(candidate)));
  writer_->Write(runtime_->Borrow(), directory_ / ("sample-" + std::to_string(index) + ".png"));
 }
 DirectComputeConfiguration configuration_;
 VisualDeviceSettings visual_;
 ComputeArtifactSink published_;
 raster::RenderedImageWriter::PngEncoder encoder_;
 std::shared_ptr<gpu::TerminalCudaRetirementOwner> retirement_ = std::make_shared<gpu::TerminalCudaRetirementOwner>(21U);
 std::optional<gpu::DeviceContext> context_;
 std::unique_ptr<PredictionPreviewPool> pool_;
 std::unique_ptr<gpu::SystemImageRuntime> runtime_;
 std::unique_ptr<raster::CaptionRaster> captions_;
 std::unique_ptr<raster::RenderedImageWriter> writer_;
 std::filesystem::path directory_;
 contracts::ValidationRunPreview options_;
 std::vector<raster::NamedCaption> labels_;
 std::array<std::uint32_t,rfdetr::kValidationSampleCapacity> indices_{};
 std::array<bool,rfdetr::kValidationSampleCapacity> captured_{};
 std::size_t count_ = 0;
 std::exception_ptr failure_;
};
ValidationSampleOutput::ValidationSampleOutput(DirectComputeConfiguration configuration, VisualDeviceSettings visual, ComputeArtifactSink published, raster::RenderedImageWriter::PngEncoder encoder)
 : impl_(std::make_shared<Impl>(std::move(configuration), visual, std::move(published),std::move(encoder))) {}
ValidationSampleOutput::~ValidationSampleOutput() = default;
void ValidationSampleOutput::Begin(std::filesystem::path directory, contracts::ValidationRunPreview options, std::span<const std::uint32_t> indices) {
 if (indices.size() > rfdetr::kValidationSampleCapacity) throw std::invalid_argument("validation sample selection exceeds six");
 if (!std::isfinite(options.display.confidence_threshold) || options.display.confidence_threshold < 0 || options.display.confidence_threshold > 1) throw std::invalid_argument("validation preview confidence must be between zero and one");
 impl_->Flush();
 impl_->directory_ = std::move(directory) / "samples";
 impl_->options_ = options;
 impl_->count_ = indices.size();
 for (std::size_t index = 0; index < indices.size(); ++index)
  if (std::find(indices.begin(), indices.begin()+index, indices[index]) != indices.begin()+index) throw std::invalid_argument("validation selected a duplicate sample");
 std::ranges::copy(indices, impl_->indices_.begin());
 impl_->captured_.fill(false);
 impl_->failure_ = {};
}
void ValidationSampleOutput::UseCaptureContext(gpu::DeviceContext context) {
 if (impl_->context_ && *impl_->context_ != context) throw std::logic_error("validation capture context changed while output is retained");
 impl_->context_ = std::move(context);
}
std::shared_ptr<const PredictionPreviewFrame> ValidationSampleOutput::Capture(rfdetr::ValidationSampleView sample) {
 const auto end = impl_->indices_.begin() + impl_->count_;
 const auto found = std::find(impl_->indices_.begin(), end, sample.prediction.dataset_index);
 if (found == end) throw std::logic_error("validation delivered an unselected sample");
 const auto slot = static_cast<std::size_t>(found - impl_->indices_.begin());
 if (impl_->captured_[slot]) throw std::logic_error("validation delivered a sample twice");
 impl_->captured_[slot] = true;
 std::shared_ptr<const PredictionPreviewFrame> raw;
 try {
  if (!sample.pixels.preview_failure.empty()) throw std::runtime_error("validation sample pixels unavailable: " + std::string(sample.pixels.preview_failure));
  if (!sample.annotations.class_catalog) throw std::invalid_argument("validation sample class catalog is unavailable");
  impl_->Ensure();
  raw = impl_->pool_->Capture(sample.pixels.chw, {sample.pixels.width,sample.pixels.height},sample.pixels.stream,sample.prediction.detections,sample.annotations,
   sample.annotations.class_catalog,static_cast<int>(sample.annotations.class_catalog->size()),sample.pixels.rgb8,std::move(sample.pixels.custody),sample.pixels.stop_source,sample.pixels.source_control,sample.ground_truth,true,sample.pixels.device);
  if (!raw) throw std::runtime_error("validation sample capture capacity is exhausted");
  if (!impl_->failure_) {
   auto lease = gpu::ReserveTerminalCudaLease(*impl_->retirement_);
   struct Custody { std::shared_ptr<Impl> state; gpu::TerminalCudaRetirementLease& lease; } custody{impl_,lease};
   gpu::CudaContextScope scope({&custody, [](void* value) noexcept {
    auto& owner = *static_cast<Custody*>(value);
    std::move(owner.lease).Install(gpu::TerminalCudaCustody::Share(std::move(owner.state)), cudaErrorUnknown);
   }});
   try { scope.Run([&] { impl_->Draw(raw, {sample.pixels.width,sample.pixels.height}, static_cast<std::uint32_t>(sample.prediction.dataset_index)); }); }
   catch (...) {
    if (!impl_->retirement_->admission_open() || gpu::is_image_execution_failure(std::current_exception())) {
     if (lease) std::move(lease).Install(gpu::TerminalCudaCustody::Share(std::shared_ptr<Impl>(impl_)), cudaErrorUnknown);
     throw mmltk::backend::ml::runtime::CudaOperationError{cudaErrorUnknown, "validation rendered output custody"};
    }
    throw;
   }
  }
 } catch (const mmltk::backend::ml::runtime::CudaOperationError&) { throw; }
 catch (...) { if (!impl_->failure_) impl_->failure_ = std::current_exception(); }
 return raw;
}
void ValidationSampleOutput::Finish(bool require_complete) {
 try { impl_->Flush(); } catch (...) { if (!impl_->failure_) impl_->failure_ = std::current_exception(); }
 if (impl_->failure_) std::rethrow_exception(impl_->failure_);
 if (require_complete && !std::all_of(impl_->captured_.begin(), impl_->captured_.begin() + impl_->count_, [](bool value) { return value; })) throw std::runtime_error("validation did not deliver every selected sample");
}
}
