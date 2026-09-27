#pragma once
#include "src/frameworks/gpu/memory/pinned_host_buffer.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#include "src/backend/media/video/video_media.h"
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include "src/backend/models/rfdetr/contract/execution_plan.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
namespace mmltk::backend::models::rfdetr {
struct PredictionRecord {
 std::int64_t dataset_index = 0;
 std::int64_t image_id = 0;
 std::string source_name{};
 std::vector<Prediction> detections{};
};
struct PredictionRunResult {
 ResolvedModelArtifacts artifacts{};
 std::string backend_name{};
 std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> class_catalog{};
 mmltk::backend::data::catalog::ClassReferenceDomain class_domain = mmltk::backend::data::catalog::ClassReferenceDomain::RawOutputSlot;
 std::size_t candidate_count = 0;
 bool masks_available = false;
 bool cancelled = false;
 std::size_t processed_images = 0;
 std::uint64_t source_images = 0;
 ExecutionFacts execution{};
 PhaseTiming timing{};
};
// Current-record pixels are either CHW device storage or owned decoded RGB8.
// GPU receiver copies run on `stream`, so source reuse follows every read.
// `custody` owns the exact source and compact annotations for failed-copy retirement.
// A receiver that cannot settle that stream must retain custody, stop source workers,
// and propagate CudaOperationError; it may not return a contained visual failure.
struct PredictionPixels final {
 const float* chw = nullptr;
 std::uint32_t width = 0U;
 std::uint32_t height = 0U;
 int device = -1;
 std::uintptr_t stream = 0U;
 const std::uint8_t* rgb8 = nullptr;
 std::shared_ptr<void> custody{};
 void (*stop_source)(void*) = nullptr;
 void* source_control = nullptr;
 std::string_view preview_failure{};
 mmltk::backend::media::video::VideoTiming timing{};
};
struct PredictionDemand final {
 // Bounded preview pixels follow the delivery's visual extent limits.
 bool source_pixels = false;
 // Selected saved products use source geometry within physical storage bounds.
 bool native_pixels = false;
 bool encoded_masks = false;
 bool preview_masks = false;
};
struct PredictionDelivery final {
 std::stop_token stop{};
 // Compact annotation planes accompany requested current pixels only. Without
 // raw preview demand, completed still receives every semantic record (including RLE),
 // while its annotation storage has no available device planes.
 bool source_pixels = false;
 // Standalone semantic delivery retains its mask contract. Application
 // consumers explicitly disable this when no JSON/RLE sink is attached.
 bool encoded_masks = true;
 // Additional per-source demand is resolved before the native batch forward.
 std::function<PredictionDemand(std::int64_t)> demand{};
 std::uint32_t maximum_pixel_width = UINT32_MAX;
 std::uint32_t maximum_pixel_height = UINT32_MAX;
 std::function<bool()> before_source{};
 std::function<bool(std::optional<double>, double)> before_frame{};
 std::function<void(const mmltk::backend::media::video::VideoMediaInfo&)> media_begin{};
 std::function<void(const mmltk::backend::media::video::VideoAudioPacket&)> audio{};
 std::function<void(const PredictionRunResult&)> begin{};
 std::function<void(const PredictionRecord&, PredictionPixels, const mmltk::backend::ml::runtime::AnalysisAnnotationStorage&)> completed{};
 std::function<void(std::size_t completed, std::size_t total)> progress{};
 std::function<void(std::size_t decoded, std::size_t total)> decoded{};
 std::function<void(const ExecutionFacts&)> admitted{};
 std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement{};
 mmltk::frameworks::gpu::PinnedHostBuffer::Operations registered_host_operations{};
};
}  // namespace mmltk::backend::models::rfdetr
