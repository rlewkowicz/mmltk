#include "prediction_output.h"
#include "cuda_runtime_resources.h"
#include "prediction_sampling.h"
#include "src/backend/imaging/raster/caption_raster.h"
#include "src/backend/imaging/raster/rendered_image_writer.h"
#include "src/backend/imaging/raster/class_palette.h"
#include "src/backend/media/video/video_file_sink.h"
#include "src/frameworks/gpu/image_failure.h"
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <string>
#include <vector>
namespace mmltk::controller::detail {
namespace gpu = mmltk::frameworks::gpu;
namespace raster = mmltk::backend::imaging::raster;
namespace rfdetr = mmltk::backend::models::rfdetr;
namespace media = mmltk::backend::media::video;
struct PredictionOutput::Impl final {
 DirectComputeConfiguration configuration;
 rfdetr::PredictSourceKind kind;
 PredictionRunOutput options;
 bool enabled = false;
 bool full_video = false;
 bool catalog_ready = false;
 bool captions_ready = false;
 std::filesystem::path partial_path, completed_path;
 std::mt19937_64 random;
 raster::RenderedImageWriter::PngEncoder png_encoder;
 std::optional<PredictionSelection> selection;
 std::uint64_t expected_samples = 0, processing_ordinal = 0;
 std::optional<PredictionReservoir> reservoir;
 std::int64_t decided = -1;
 std::optional<std::uint64_t> slot;
 std::unordered_map<std::uint64_t, std::filesystem::path> slots;
 std::optional<std::uint64_t> pending_slot;
 std::shared_ptr<gpu::TerminalCudaRetirementOwner> retirement = std::make_shared<gpu::TerminalCudaRetirementOwner>(7U);
 std::optional<gpu::DeviceContext> context;
 std::unique_ptr<PredictionPreviewPool> pool;
 std::unique_ptr<gpu::SystemImageRuntime> runtime;
 std::unique_ptr<raster::CaptionRaster> captions;
 std::unique_ptr<raster::RenderedImageWriter> writer;
 std::unique_ptr<gpu::ImageStream> encode_stream;
 std::unique_ptr<media::VideoFileSink> video;
 std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> catalog;
 int classes = 0;
 bool foreground = false;
 std::vector<std::string> names;
 std::vector<raster::NamedCaption> labels;
 std::vector<std::array<char, 32>> suffixes;
 contracts::WorkflowOutputFacts facts;
 std::chrono::steady_clock::time_point last_publication{};
 Impl(DirectComputeConfiguration config, rfdetr::PredictSourceKind source, PredictionRunOutput requested, std::optional<gpu::DeviceContext> capture, raster::RenderedImageWriter::PngEncoder encoder,
  std::optional<std::uint64_t> seed)
     : configuration(std::move(config)), kind(source), options(std::move(requested)), context(std::move(capture)) {
  png_encoder = std::move(encoder);
  const auto& saving = options.saving;
  enabled = !options.directory.empty() && (kind == rfdetr::PredictSourceKind::CompiledDataset ? saving.compiled_enabled
                                           : kind == rfdetr::PredictSourceKind::VideoFile     ? saving.video_enabled
                                                                                              : saving.single_enabled);
  facts.directory = options.directory.string();
  if (!enabled) return;
  full_video = kind == rfdetr::PredictSourceKind::VideoFile && saving.video_mode == contracts::PredictionVideoSaving::Full;
  if (full_video) {
   partial_path = options.directory / "prediction.partial.mkv";
   completed_path = options.directory / "prediction.mkv";
  }
  const auto seed_random = [&] {
   if (seed)
    random.seed(*seed);
   else {
    std::random_device entropy;
    random.seed((static_cast<std::uint64_t>(entropy()) << 32U) | entropy());
   }
  };
  if (mmltk::frameworks::reflection::validate_reflected_fields(saving) || mmltk::frameworks::reflection::validate_reflected_fields(options.preview))
   throw std::invalid_argument("invalid prediction saving options");
  if (kind == rfdetr::PredictSourceKind::CompiledDataset) {
   const auto count = saving.compiled_mode == contracts::PredictionCompiledSampling::Percent ? PredictionSelection::Percent(options.population, saving.compiled_percent) : saving.compiled_total;
   const auto population = options.processing_population ? options.processing_population : options.population;
   if (count < population) seed_random();
   selection.emplace(population, count, random);
   expected_samples = count;
  }
  if (kind == rfdetr::PredictSourceKind::VideoFile && saving.video_mode == contracts::PredictionVideoSaving::Samples) {
   seed_random();
   reservoir.emplace(saving.video_samples);
  }
  if (kind != rfdetr::PredictSourceKind::VideoFile || reservoir) facts.samples_directory = (kind == rfdetr::PredictSourceKind::ImageFiles ? options.directory : options.directory / "samples").string();
 }
 void PrepareCaptions() {
  if (!captions_ready && captions && catalog_ready) {
   if (!names.empty()) captions->Prepare(names);
   captions_ready = true;
  }
 }
 void Ensure() {
  if (!context) {
   if (!configuration.execution) throw std::runtime_error("prediction output has no admitted device");
   context.emplace(CreatePredictionPreviewContext(*configuration.execution, retirement));
  }
  const auto execution = *context->execution();
  if (!pool)
   pool = std::make_unique<PredictionPreviewPool>(
    execution, *context, PredictionPreviewPool::TransferOperations{&cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize, &cuMemHostRegister}, retirement);
  if (!runtime)
   runtime = std::make_unique<gpu::SystemImageRuntime>(gpu::SystemImageRuntimeConfig{
    .device = context->device(), .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .numa_node = execution.placement.numa_node, .execution = execution, .adopted_context = *context});
  if (full_video && !encode_stream) encode_stream = std::make_unique<gpu::ImageStream>(*context);
  if (!captions) captions = std::make_unique<raster::CaptionRaster>(*context);
  if (!full_video && !writer) writer = std::make_unique<raster::RenderedImageWriter>(*context, png_encoder);
  PrepareCaptions();
 }
 void Publish(bool force = false) {
  const auto now = std::chrono::steady_clock::now();
  if (!force && now - last_publication < std::chrono::milliseconds(100)) return;
  last_publication = now;
  if (options.progress) options.progress(facts);
 }
 void Flush() {
  if (!writer) return;
  auto completed = writer->Flush();
  if (completed.empty()) return;
  if (pending_slot) {
   auto found = slots.find(*pending_slot);
   if (found != slots.end()) {
    // Replacement publication is already complete. A write failure never
    // removes the incumbent, and no later queued writer can own this slot.
    std::filesystem::remove(found->second);
    found->second = completed;
   } else {
    slots.emplace(*pending_slot, completed);
    ++facts.completed_samples;
   }
   pending_slot.reset();
  } else
   ++facts.completed_samples;
  facts.recent_sample = completed.string();
  Publish();
 }
 void Draw(const std::shared_ptr<const PredictionPreviewFrame>& raw, const rfdetr::PredictionRecord& record, rfdetr::PredictionPixels pixels) {
  Flush();
  runtime->BindContext();
  labels.clear();
  suffixes.clear();
  suffixes.resize(raw->predictions().size());
  labels.reserve(raw->predictions().size());
  std::size_t suffix_index = 0;
  for (const auto& prediction : raw->predictions()) {
   if (!options.preview.labels || prediction.score < options.preview.confidence_threshold) continue;
   const auto category = prediction.class_reference;
   if (category < 0 || category >= classes) throw std::invalid_argument("prediction caption class is outside its admitted domain");
   auto& suffix = suffixes[suffix_index++];
   char* cursor = suffix.data();
   *cursor++ = ' ';
   if (!foreground) {
    const auto result = std::to_chars(cursor, suffix.data() + suffix.size() - 1U, category);
    if (result.ec != std::errc{}) throw std::runtime_error("prediction slot caption exceeds its bound");
    cursor = result.ptr;
    *cursor++ = ' ';
   }
   const auto score = std::to_chars(cursor, suffix.data() + suffix.size(), prediction.score);
   if (score.ec != std::errc{}) throw std::runtime_error("prediction confidence caption exceeds its bound");
   const auto coordinate = [](float value) {
    if (!std::isfinite(value) || double(value) < std::numeric_limits<int>::min() || double(value) > std::numeric_limits<int>::max())
     throw std::invalid_argument("prediction caption coordinate is invalid");
    return static_cast<int>(std::floor(value));
   };
   const int x = coordinate(prediction.bbox_xyxy[0]), y = coordinate(prediction.bbox_xyxy[1]);
   std::array<std::uint8_t, 3> color{};
   raster::color::class_color(category, classes, color[0], color[1], color[2]);
   labels.push_back({foreground ? static_cast<std::uint32_t>(category) : 0U, x, y > static_cast<int>(raster::kNativeCaptionHeight) ? y - static_cast<int>(raster::kNativeCaptionHeight) : 0, color,
    {suffix.data(), static_cast<std::size_t>(score.ptr - suffix.data())}});
  }
  auto candidate = runtime->AcquireOutput();
  const VisualExtent extent{pixels.width, pixels.height};
  const std::array regions{PredictionPreviewComposition::Region{raw, {0U, 0U, extent.width, extent.height}}};
  PredictionPreviewComposition::Draw(*runtime, candidate, extent, regions, {options.preview.boxes, options.preview.masks, false, false, false, false, options.preview.confidence_threshold}, nullptr,
   [&](auto clean, auto semantic, auto stream) {
    raster::CaptionRaster::Composite(clean, semantic, stream);
    captions->Draw(clean, labels, stream);
   });
  static_cast<void>(runtime->CommitOutput(std::move(candidate)));
  if (video) {
   auto borrowed = runtime->Borrow();
   // Composition is settled by publication; the sink settles its read before
   // this receiver-owned output may be reused.
   try {
    video->Write(borrowed.plane(0).plane(), pixels.timing, encode_stream->native_handle());
   } catch (...) { encode_stream->RethrowAfterSettlement(std::current_exception()); }
  } else {
   const auto path = kind == rfdetr::PredictSourceKind::ImageFiles
                      ? options.directory / "sample.png"
                      : options.directory / "samples" / ((kind == rfdetr::PredictSourceKind::VideoFile ? "frame-" : "sample-") + std::to_string(record.dataset_index) + ".png");
   writer->Write(runtime->Borrow(), path);
   pending_slot = reservoir ? slot : std::nullopt;
  }
 }
};
PredictionOutput::PredictionOutput(DirectComputeConfiguration config, rfdetr::PredictSourceKind source, PredictionRunOutput options, std::optional<gpu::DeviceContext> context,
 raster::RenderedImageWriter::PngEncoder encoder, std::optional<std::uint64_t> seed)
    : impl_(std::make_shared<Impl>(std::move(config), source, std::move(options), std::move(context), std::move(encoder), seed)) {}
PredictionOutput::~PredictionOutput() = default;
bool PredictionOutput::Enabled() const noexcept { return impl_->enabled; }
bool PredictionOutput::Wants(std::int64_t index) {
 auto& s = *impl_;
 if (!s.enabled) return false;
 if (s.decided == index) return s.slot.has_value();
 if (index < 0 || (s.reservoir && index <= s.decided)) throw std::logic_error("prediction saving received out-of-order source decisions");
 s.decided = index;
 if (s.reservoir)
  s.slot = s.reservoir->Observe(s.random);
 else {
  if (s.selection && static_cast<std::uint64_t>(index) >= s.options.population) throw std::out_of_range("prediction source index exceeds admitted dataset");
  // GUI delivery is one image per batch. Demand and completed delivery repeat
  // the same source index; selection advances once in actual processing order.
  s.slot = !s.selection || s.selection->Contains(s.processing_ordinal++) ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(index)} : std::nullopt;
 }
 return s.slot.has_value();
}
void PredictionOutput::Begin(const rfdetr::PredictionRunResult& result) {
 if (!Enabled()) return;
 if (impl_->selection && result.source_images != impl_->options.population) throw std::runtime_error("prediction dataset population changed after admission");
 auto& s = *impl_;
 if (!result.class_catalog) throw std::invalid_argument("prediction saving class catalog is unavailable");
 s.catalog = result.class_catalog;
 s.foreground = result.class_domain == mmltk::backend::data::catalog::ClassReferenceDomain::Foreground;
 s.classes = s.foreground ? static_cast<int>(result.class_catalog->size()) : result.artifacts.config.num_classes;
 if (s.foreground)
  for (const auto& name : result.class_catalog->names()) s.names.push_back(name);
 else
  s.names = {"Raw slot"};
 s.catalog_ready = true;
 if (s.captions)
  RunWithRetainedCudaContext(impl_, *impl_->retirement, "prediction output CUDA custody", [&] {
   s.runtime->BindContext();
   s.PrepareCaptions();
  });
}
void PredictionOutput::Media(const media::VideoMediaInfo& info) {
 if (!Enabled() || !impl_->full_video) return;
 RunWithRetainedCudaContext(impl_, *impl_->retirement, "prediction output CUDA custody", [&] {
  impl_->Ensure();
  impl_->runtime->BindContext();
  try {
   impl_->video = std::make_unique<media::VideoFileSink>(impl_->partial_path, impl_->completed_path, info, impl_->context->device());
  } catch (...) {
   const auto& partial = impl_->partial_path;
   std::error_code error;
   if (std::filesystem::is_regular_file(partial, error)) {
    impl_->facts.partial_video = partial.string();
    impl_->Publish(true);
   }
   throw;
  }
 });
 impl_->facts.partial_video = impl_->partial_path.string();
 impl_->Publish(true);
}
void PredictionOutput::Audio(const media::VideoAudioPacket& packet) {
 if (impl_->video) impl_->video->Audio(packet);
}
std::shared_ptr<const PredictionPreviewFrame> PredictionOutput::Capture(
 const rfdetr::PredictionRecord& record, rfdetr::PredictionPixels pixels, const mmltk::backend::ml::runtime::AnalysisAnnotationStorage& annotations) {
 if (!Wants(record.dataset_index)) return {};
 if (!pixels.preview_failure.empty()) throw std::runtime_error("prediction saving pixels unavailable: " + std::string(pixels.preview_failure));
 std::shared_ptr<const PredictionPreviewFrame> raw;
 RunWithRetainedCudaContext(impl_, *impl_->retirement, "prediction output CUDA custody", [&] {
  impl_->Ensure();
  raw = impl_->pool->Capture(pixels.chw, {pixels.width, pixels.height}, pixels.stream, record.detections, annotations, impl_->catalog, impl_->classes, pixels.rgb8, pixels.custody, pixels.stop_source,
   pixels.source_control, {}, true, pixels.device);
  if (!raw) throw std::runtime_error("prediction saving capture capacity exhausted");
  impl_->Draw(raw, record, pixels);
 });
 return raw;
}
void PredictionOutput::Finish(bool success) {
 impl_->Flush();
 if (!success) {
  if (impl_->video) impl_->video->ClosePartial();
  impl_->Publish(true);
  return;
 }
 impl_->Publish(true);
 if (impl_->selection && impl_->facts.completed_samples != impl_->expected_samples) throw std::runtime_error("prediction did not deliver every selected sample");
 if (impl_->reservoir && impl_->reservoir->selected() < impl_->options.saving.video_samples)
  throw std::runtime_error("Requested " + std::to_string(impl_->options.saving.video_samples) + " samples; the video contained " + std::to_string(impl_->reservoir->observed()) + " frames. Saved " +
                           std::to_string(impl_->facts.completed_samples) + ".");
 if (impl_->video) {
  RunWithRetainedCudaContext(impl_, *impl_->retirement, "prediction output CUDA custody", [&] {
   impl_->runtime->BindContext();
   impl_->video->Complete();
  });
  impl_->facts.partial_video.clear();
  impl_->facts.artifacts.push_back(impl_->completed_path);
  impl_->Publish(true);
 }
}
}  // namespace mmltk::controller::detail
