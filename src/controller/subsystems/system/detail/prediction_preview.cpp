#include "src/backend/imaging/raster/image_containment.h"
#include "prediction_preview.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/backend/imaging/raster/chw_image.h"
#include "src/backend/models/rfdetr/contract/prediction_limits.h"
#include "src/backend/models/rfdetr/contract/class_layout.h"
#include "src/frameworks/gpu/cuda_high_water_allocation.h"
#include "src/frameworks/gpu/image_failure.h"
#include "src/controller/contracts/annotation.h"
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <mutex>
#include <utility>
#include <cstring>
#include <cmath>
#include <limits>
import mmltk.backend.imaging.raster;
namespace mmltk::controller::detail {
namespace gpu = mmltk::frameworks::gpu;
namespace raster = mmltk::backend::imaging::raster;
namespace rfdetr = mmltk::backend::models::rfdetr;
namespace {
void checked(cudaError_t status) {
 if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
}
std::size_t preview_slot_count(std::size_t slots) {
 if (slots == 0U || slots > PredictionPreviewComposition::kMaximumFrames) throw std::invalid_argument("preview slot capacity is outside its bound");
 return slots;
}
}  // namespace
gpu::DeviceContext CreatePredictionPreviewContext(const gpu::DeviceExecution& execution, const std::shared_ptr<gpu::TerminalCudaRetirementOwner>& retirement, gpu::CudaContextApi api) {
 if (!retirement) throw std::invalid_argument("prediction preview retirement authority is unavailable");
 auto lease = gpu::ReserveTerminalCudaLease(*retirement);
 auto candidate = std::make_shared<std::optional<gpu::DeviceContext>>();
 struct Construction final {
  gpu::TerminalCudaRetirementLease& lease;
  std::shared_ptr<std::optional<gpu::DeviceContext>>& candidate;
 } construction{lease, candidate};
 if (cuInit(0U) != CUDA_SUCCESS) throw std::runtime_error("prediction CUDA initialization failed");
 gpu::CudaContextScope scope({&construction,
                              [](void* owner) noexcept {
                               auto& value = *static_cast<Construction*>(owner);
                               auto retained = value.candidate;
                               std::move(value.lease).Install(gpu::TerminalCudaCustody::Share(std::move(retained)), cudaErrorUnknown);
                              }},
  api);
 scope.Run([&] { candidate->emplace(execution.device, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution); });
 return **candidate;
}
struct PredictionPreviewFrame::State final {
 explicit State(const gpu::DeviceContext& receiver, gpu::CudaContextApi api, std::shared_ptr<void> source) : context(receiver), context_api(api), source_custody(std::move(source)) {}
 void Reserve(std::size_t bytes) {
  if (bytes <= capacity) return;
  checked(storage.RetryPending([](void* address) noexcept { return cudaFree(address); }).failure);
  checked(storage.AllocateCandidate([bytes](void*& address) noexcept { return cudaMalloc(&address, bytes); }).failure);
  checked(storage.PromoteCandidate([](void* address) noexcept { return cudaFree(address); }).failure);
  capacity = bytes;
 }
 std::mutex mutex;
 gpu::DeviceContext context;
 gpu::CudaContextApi context_api;
 std::optional<gpu::DeviceContext> source_context;
 cudaEvent_t source_ready = nullptr;
 bool source_recorded = false;
 enum class Pixels { ChwFloat, Rgb8 };
 Pixels pixels = Pixels::ChwFloat;
 const std::uint8_t* rgb8 = nullptr;
 decltype(&cuMemHostRegister) register_host = &cuMemHostRegister;
 decltype(PredictionPreviewPool::TransferOperations::upload) upload = &cudaMemcpyAsync;
 decltype(&raster::chw_float_to_rgba) convert = &raster::chw_float_to_rgba;
 decltype(&raster::rgb8_to_rgba) convert_rgb8 = &raster::rgb8_to_rgba;
 mmltk::common::system::ExecutionPlacement placement;
 std::shared_ptr<void> decoded_source;
 std::unique_ptr<gpu::PinnedHostBuffer> pinned;
 std::shared_ptr<void> source_custody;
 std::atomic<cudaError_t> unsafe{cudaSuccess};
 gpu::CudaHighWaterAllocation<void*> storage;
 std::size_t capacity = 0U;
 VisualExtent extent;
 std::size_t boxes_offset{}, confidences_offset{}, labels_offset{}, masks_offset{}, colors_offset{};
 bool masks = false;
 int category_count = 0;
 std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> catalog;
 std::vector<rfdetr::Prediction> predictions;
 std::vector<rfdetr::Prediction> ground_truth;
 std::vector<std::uint32_t> ground_truth_runs;
 std::vector<std::uint8_t> ground_truth_colors;
 std::size_t ground_truth_offset = 0, scratch_offset = 0;
 bool composition = false;
 decltype(&cudaMemset2DAsync) clear_semantic = &cudaMemset2DAsync;
 std::uint64_t capture = 0U;
 struct Preparation final {
  bool clean = false, semantic = false, ground_truth = false, colors = false;
  PredictionPreviewComposition::Options options;
 } prepared, pending;
};
PredictionPreviewFrame::PredictionPreviewFrame(const gpu::DeviceContext& context, std::shared_ptr<gpu::TerminalCudaRetirementOwner> retirement, std::shared_ptr<void> source, gpu::CudaContextApi api)
    : retirement_(std::move(retirement)) {
 auto lease = retirement_->Reserve();
 if (!lease) throw std::runtime_error("prediction preview retirement admission is closed");
 lease_ = std::move(*lease);
 // No GPU work occurs until this exact partially initialized state is retainable.
 state_ = std::make_shared<State>(context, api, std::move(source));
}
gpu::CudaContextScope PredictionPreviewFrame::ContextScope() const noexcept {
 return gpu::CudaContextScope(
  {const_cast<PredictionPreviewFrame*>(this), [](void* owner) noexcept { static_cast<PredictionPreviewFrame*>(owner)->RetainUnsafe(cudaErrorUnknown); }}, state_->context_api);
}
gpu::CudaContextScope PredictionPreviewFrame::CompositionScope() const noexcept {
 // The enclosing submission has already reserved custody of this actual state
 // and every other participant. A context failure marks it for that aggregate.
 return gpu::CudaContextScope({state_.get(), [](void* owner) noexcept { static_cast<State*>(owner)->unsafe.store(cudaErrorUnknown); }}, state_->context_api);
}
PredictionPreviewFrame::~PredictionPreviewFrame() {
 if (!state_ || state_->unsafe != cudaSuccess) return;
 try {
  auto scope = ContextScope();
  scope.Run([&] {
   if (state_->source_ready) {
    state_->source_context->Bind();
    if (state_->source_recorded) checked(cudaEventSynchronize(state_->source_ready));
    checked(cudaEventDestroy(state_->source_ready));
    state_->source_ready = nullptr;
   }
   if (state_->source_context) {
    state_->source_context->Bind();
    state_->source_custody.reset();
    state_->decoded_source.reset();
   }
   state_->context.Bind();
   state_->source_context.reset();
   checked(state_->storage.ReleaseAll([](void* address) noexcept { return cudaFree(address); }).failure);
   if (state_->pinned && state_->pinned->ReleaseSettled() != CUDA_SUCCESS) throw std::runtime_error("prediction pinned release failed");
   state_->pinned.reset();
  });
 } catch (const gpu::CudaContextFailure& error) {
  if (error.terminal()) RetainUnsafe(cudaErrorUnknown);
 } catch (...) { RetainUnsafe(cudaErrorUnknown); }
 // The callback's state stays alive until explicit restoration has finished.
}
void PredictionPreviewFrame::RetainUnsafe(cudaError_t failure) const noexcept {
 // The frame's transaction lock (or sole-owner destruction) serializes installation.
 if (state_->unsafe.exchange(failure) == cudaSuccess) {
  auto retained = state_;
  std::move(lease_).Install(gpu::TerminalCudaCustody::Share(std::move(retained)), failure);
 }
}
std::span<const rfdetr::Prediction> PredictionPreviewFrame::ground_truth() const noexcept { return state_->ground_truth; }
std::span<const rfdetr::Prediction> PredictionPreviewFrame::predictions() const noexcept { return state_->predictions; }
std::span<const std::string> PredictionPreviewFrame::classes() const noexcept { return state_->catalog->names(); }
int PredictionPreviewFrame::class_count() const noexcept { return state_->category_count; }
PredictionPreviewPool::PredictionPreviewPool(gpu::DeviceExecution execution, gpu::DeviceContext context)
    : PredictionPreviewPool(std::move(execution), std::move(context), {&cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize, &cuMemHostRegister}) {}
PredictionPreviewPool::PredictionPreviewPool(
 gpu::DeviceExecution execution, gpu::DeviceContext context, TransferOperations operations, std::shared_ptr<gpu::TerminalCudaRetirementOwner> retirement, std::size_t slots)
    : operations_(operations),
      execution_(std::move(execution)),
      context_(std::move(context)),
      retirement_(retirement ? std::move(retirement) : std::make_shared<gpu::TerminalCudaRetirementOwner>(preview_slot_count(slots) + 1U)),
      slots_(preview_slot_count(slots)) {
 if (!retirement_->admission_open()) throw std::runtime_error("prediction preview retirement admission is closed");
 if (!operations_.wait || !operations_.copy || !operations_.record || !operations_.settle || !operations_.register_host || !operations_.upload || !operations_.clear_semantic || !operations_.convert ||
     !operations_.convert_rgb8 || !operations_.context_api.get || !operations_.context_api.set)
  throw std::invalid_argument("prediction transfer operations are incomplete");
 // Compatibility follows the retained context, device and execution owner.
 // A backend decorating CUDA operations does not create a different context.
 context_.ValidateSelection(execution_.device, {}, gpu::DeviceContextMode::Isolated, execution_.placement.numa_node, execution_);
}
std::shared_ptr<const PredictionPreviewFrame> PredictionPreviewPool::Capture(const float* pixels, VisualExtent extent, std::uintptr_t source_stream, std::span<const rfdetr::Prediction> predictions,
 const mmltk::backend::ml::runtime::AnalysisAnnotationStorage& annotations, std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> catalog, int classes, const std::uint8_t* rgb8,
 std::shared_ptr<void> source_custody, void (*stop_source)(void*), void* source_control, std::span<const rfdetr::Prediction> ground_truth, bool composition) {
 if (!retirement_->admission_open()) throw std::runtime_error("prediction preview CUDA retirement failed");
 auto available = std::ranges::find_if(slots_, [](const auto& slot) { return !slot || (slot.use_count() == 1 && slot->state_->unsafe == cudaSuccess); });
 if (available == slots_.end()) return {};
 if ((!pixels && !rgb8) || (pixels && rgb8) || !source_custody || !extent.valid() ||
     (predictions.size() > contracts::kAnnotationObjectCapacity || ground_truth.size() > contracts::kAnnotationObjectCapacity) || !catalog || classes < 0 ||
     static_cast<std::size_t>(classes) > rfdetr::kMaximumClassOutputSlots)
  throw std::invalid_argument("prediction preview source is invalid");
 const auto count = predictions.size();
 for (const auto& prediction : predictions) {
  const auto category = prediction.class_reference;
  if (category < 0 || category >= classes || prediction.class_domain != annotations.class_domain ||
      (prediction.class_domain == mmltk::backend::data::catalog::ClassReferenceDomain::Foreground && static_cast<std::size_t>(category) >= catalog->size()))
   throw std::invalid_argument("prediction reference disagrees with its declared domain");
  if (category >= 0 && static_cast<std::size_t>(category) < catalog->size() && catalog->names()[category].size() > mmltk::frameworks::reflection::kMaximumNameBytes)
   throw std::invalid_argument("prediction label exceeds the visual name capacity");
 }
 if (count && (annotations.count.value() != count || !annotations.confidences.address || annotations.confidences.capacity_bytes < count * sizeof(float) || (annotations.masks_available && !annotations.masks.address))) throw std::invalid_argument("prediction annotations disagree with produced values");
 const auto pixel_count = rfdetr::checked_prediction_extent(extent.width, extent.height, rfdetr::kMaximumEncodedMaskPixels);
 const auto pixel_bytes = rfdetr::checked_prediction_extent(pixel_count, 3U * sizeof(float), rfdetr::kMaximumPredictionTensorBytes);
 const auto mask_bytes = annotations.masks_available && annotations.masks.address && count ? rfdetr::checked_prediction_extent(pixel_count, count, rfdetr::kMaximumPredictionTensorBytes) : 0U;
 std::size_t ground_truth_words = 0U;
 for (const auto& gt : ground_truth) {
  if (gt.class_reference < 0 || static_cast<std::size_t>(gt.class_reference) >= catalog->size()) throw std::invalid_argument("preview ground truth category is invalid");
  if (gt.mask.runs.size() > rfdetr::kMaximumPredictionMaskRuns - ground_truth_words / 2U) throw std::invalid_argument("preview ground truth RLE exceeds capacity");
  ground_truth_words += gt.mask.runs.size() * 2U;
 }
 const auto raw_bytes = pixel_bytes + count * (5U * sizeof(float) + sizeof(std::int32_t) + 3U) + mask_bytes;
 const auto admitted_ground_truth_offset = (raw_bytes + 3U) & ~std::size_t{3U};
 const auto admitted_scratch_offset = admitted_ground_truth_offset + ground_truth_words * sizeof(std::uint32_t);
 const auto admitted_bytes = admitted_scratch_offset + (composition ? pixel_count * 8U : 0U);
 if (admitted_bytes > rfdetr::kMaximumPredictionTensorBytes) throw std::invalid_argument("prediction raw preview exceeds storage capacity");
 // Admission above deliberately retains the former float and full aggregate bounds.
 // Every physical extent is bounded by that admitted layout; RGB's aligned prefix
 // cannot exceed 12P, including odd pixel counts.
 const auto physical_pixels = rgb8 ? pixel_count * 3U : pixel_bytes;
 const auto boxes_offset = (physical_pixels + alignof(float) - 1U) & ~(alignof(float) - 1U);
 const auto physical_raw_bytes = boxes_offset + (raw_bytes - pixel_bytes);
 const auto ground_truth_offset = (physical_raw_bytes + 3U) & ~std::size_t{3U};
 const auto scratch_offset = ground_truth_offset + ground_truth_words * sizeof(std::uint32_t);
 const auto bytes = scratch_offset + (composition ? pixel_count * 8U : 0U);
 if (bytes > admitted_bytes) throw std::invalid_argument("prediction compact preview exceeds admitted storage");
 try {
  if (!*available) *available = std::shared_ptr<PredictionPreviewFrame>(new PredictionPreviewFrame(context_, retirement_, source_custody, operations_.context_api));
  auto& state = *(*available)->state_;
  std::unique_lock state_lock(state.mutex, std::try_to_lock);
  if (!state_lock) return {};
  checked(state.unsafe);
  state.source_custody = source_custody;
  auto scope = (*available)->ContextScope();
  scope.Run([&] {
   if (!state.source_context) { state.source_context.emplace(execution_.device, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::PrimaryInterop, execution_.placement.numa_node, execution_); }
   if (!state.source_ready) {
    state.source_context->Bind();
    checked(cudaEventCreateWithFlags(&state.source_ready, cudaEventDisableTiming));
   }
   if (bytes > state.capacity && state.source_recorded) {
    auto source_scope = (*available)->ContextScope();
    source_scope.Run([&] {
     state.source_context->Bind();
     checked(cudaEventSynchronize(state.source_ready));
    });
   }
   state.context.Bind();
   state.Reserve(bytes);
   state.pixels = rgb8 ? PredictionPreviewFrame::State::Pixels::Rgb8 : PredictionPreviewFrame::State::Pixels::ChwFloat;
   state.rgb8 = rgb8;
   state.decoded_source = rgb8 ? source_custody : std::shared_ptr<void>{};
   state.register_host = operations_.register_host;
   state.upload = operations_.upload;
   state.convert = operations_.convert;
   state.convert_rgb8 = operations_.convert_rgb8;
   state.clear_semantic = operations_.clear_semantic;
   if (state.capture == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("preview capture identity exhausted");
   ++state.capture;
   state.prepared = state.pending = {};
   state.placement = execution_.placement;
   state.extent = extent;
   state.boxes_offset = boxes_offset;
   state.confidences_offset = state.boxes_offset + count * 4U * sizeof(float);
   state.labels_offset = state.confidences_offset + count * sizeof(float);
   state.masks_offset = state.labels_offset + count * sizeof(std::int32_t);
   state.colors_offset = state.masks_offset + mask_bytes;
   state.masks = mask_bytes != 0U;
   state.ground_truth.assign(ground_truth.begin(), ground_truth.end());
   state.ground_truth_runs.clear();
   state.ground_truth_runs.reserve(ground_truth_words);
   for (const auto& gt : ground_truth)
    for (const auto& [start, length] : gt.mask.runs) {
     state.ground_truth_runs.push_back(start);
     state.ground_truth_runs.push_back(length);
    }
   std::vector<int> ground_truth_labels;
   ground_truth_labels.reserve(ground_truth.size());
   for (const auto& gt : ground_truth) ground_truth_labels.push_back(gt.class_reference);
   state.ground_truth_colors = raster::category_colors(ground_truth_labels, classes);
   state.ground_truth_offset = ground_truth_offset;
   state.scratch_offset = scratch_offset;
   state.composition = composition;
   state.catalog = std::move(catalog);
   state.category_count = classes;
   state.predictions.clear();
   state.predictions.reserve(count);
   for (const auto& prediction : predictions)
    state.predictions.push_back({.class_reference = prediction.class_reference, .class_domain = prediction.class_domain, .score = prediction.score, .bbox_xyxy = prediction.bbox_xyxy});
   auto* destination = static_cast<std::uint8_t*>(state.storage.active());
   bool source_submitted = false;
   try {
    const auto receiver_context = scope.Current();
    auto source_scope = (*available)->ContextScope();
    source_scope.Run([&] {
     state.source_context->Bind();
     const auto source_context = source_scope.Current();
     // A sole CPU reference does not prove the preceding peer write
     // complete. Order reuse on this source stream before re-recording
     // the event, including same-capacity never-drawn captures.
     if (state.source_recorded) {
      source_submitted = true;
      checked(operations_.wait(reinterpret_cast<cudaStream_t>(source_stream), state.source_ready, 0U));
     }
     // Custody reads share the source stream. Reuse is ordered even if a later
     // copy or event record fails; no receiver-stream wait can be lost.
     const auto copy = [&](void* target, const void* source, std::size_t size) {
      source_submitted = true;
      if (operations_.copy(reinterpret_cast<CUdeviceptr>(target), receiver_context, reinterpret_cast<CUdeviceptr>(source), source_context, size, reinterpret_cast<CUstream>(source_stream)) !=
          CUDA_SUCCESS)
       throw std::runtime_error("prediction raw custody copy failed");
     };
     if (pixels) copy(destination, pixels, pixel_bytes);
     if (count) {
      copy(destination + state.boxes_offset, reinterpret_cast<const void*>(annotations.boxes_xyxy.address), count * 4U * sizeof(float));
      copy(destination + state.confidences_offset, reinterpret_cast<const void*>(annotations.confidences.address), count * sizeof(float));
      copy(destination + state.labels_offset, reinterpret_cast<const void*>(annotations.class_references.address), count * sizeof(std::int32_t));
      if (mask_bytes) copy(destination + state.masks_offset, reinterpret_cast<const void*>(annotations.masks.address), mask_bytes);
     }
     checked(operations_.record(state.source_ready, reinterpret_cast<cudaStream_t>(source_stream)));
     state.source_recorded = true;
    });
   } catch (...) {
    const auto failure = std::current_exception();
    try {
     std::rethrow_exception(failure);
    } catch (const gpu::CudaContextFailure& error) {
     if (error.terminal()) throw;
    } catch (...) {}
    cudaError_t source_status = cudaSuccess;
    if (source_submitted) {
     try {
      auto source_scope = (*available)->ContextScope();
      source_scope.Run([&] {
       state.source_context->Bind();
       source_status = operations_.settle(reinterpret_cast<cudaStream_t>(source_stream));
      });
     } catch (...) { source_status = cudaErrorUnknown; }
    }
    if (source_status != cudaSuccess) {
     unsafe_source_ = true;
     state.source_custody = std::move(source_custody);
     (*available)->RetainUnsafe(source_status);
     throw gpu::CudaContextFailure(true);
    }
    std::rethrow_exception(failure);
   }
  });
  state.source_custody.reset();
  return *available;
 } catch (...) {
  const auto failure = std::current_exception();
  // Destruction itself can discover an unproved source/restore outcome.
  // Inspect authority only after that exact owner has settled or retained.
  available->reset();
  if (!retirement_->admission_open()) {
   unsafe_source_ = true;
   if (stop_source) {
    try {
     stop_source(source_control);
    } catch (...) {}
   }
   throw mmltk::backend::ml::runtime::CudaOperationError{cudaErrorUnknown, "prediction caller context custody"};
  }
  std::rethrow_exception(failure);
 }
}
bool PredictionPreviewFrame::CompatibleWith(const gpu::SystemImageRuntime& runtime) const noexcept { return runtime.UsesContext(state_->context); }
void PredictionPreviewFrame::Draw(gpu::SystemImageRuntime& runtime, gpu::SystemImageRuntime::OutputCandidate& candidate) const {
 const std::array regions{PredictionPreviewComposition::Region{shared_from_this(), {0U, 0U, state_->extent.width, state_->extent.height}}};
 PredictionPreviewComposition::Draw(runtime, candidate, state_->extent, regions, {});
}
void PredictionPreviewComposition::Draw(
 gpu::SystemImageRuntime& runtime, gpu::SystemImageRuntime::OutputCandidate& candidate, VisualExtent extent, std::span<const Region> regions, Options options, PredictionPreviewComposition* retained) {
 if (!extent.valid() || regions.size() > kMaximumFrames) throw std::invalid_argument("preview composition extent or count is invalid");
 if (retained && regions.size() > 6U) throw std::invalid_argument("retained preview exceeds six regions");
 std::shared_ptr<gpu::TerminalCudaRetirementOwner> retirement;
 for (const auto& region : regions) {
  if (!region.frame || !region.crop.width || !region.crop.height || region.crop.x > extent.width || region.crop.width > extent.width - region.crop.x || region.crop.y > extent.height ||
      region.crop.height > extent.height - region.crop.y)
   throw std::invalid_argument("preview composition region is invalid");
  if (!region.frame->CompatibleWith(runtime)) throw std::runtime_error("Preview belongs to a retired visual context");
  if (retirement && retirement != region.frame->retirement_) throw std::invalid_argument("preview composition spans resource owners");
  retirement = region.frame->retirement_;
 }
 // Reserve before reads, and allocate/capture the complete real aggregate before
 // publishing. Installation after failure cannot allocate or lose an earlier tile.
 gpu::TerminalCudaRetirementLease lease;
 if (retirement) lease = gpu::ReserveTerminalCudaLease(*retirement);
 struct Submission final {
  VisualExtent extent;
  Options options;
  std::vector<Region> regions;
  gpu::SystemImageRuntime::UnsafeCustody output;
 };
 auto submission = std::make_shared<Submission>(extent, options, std::vector<Region>(regions.begin(), regions.end()), gpu::SystemImageRuntime::UnsafeCustody{});
 Allocation* allocation = nullptr;
 Allocation staged;
 const auto prior_allocations = candidate.allocations();
 try {
  runtime.PublishRetained(candidate, extent.width, extent.height, [submission, &runtime, retained, &allocation, &staged, prior_allocations](auto clean, auto semantic, auto stream) {
   const auto clear = [&](auto plane) {
    checked(cudaMemset2DAsync(reinterpret_cast<void*>(plane.data), plane.descriptor.pitch_bytes, 0, plane.descriptor.row_bytes(), plane.descriptor.height, reinterpret_cast<cudaStream_t>(stream)));
   };
   bool initialize = true;
   if (retained) {
    auto found = std::ranges::find_if(retained->allocations_, [&](const auto& value) {
     return (value.clean == clean.allocation && value.semantic == semantic.allocation) ||
            (prior_allocations[0].identity != 0U && value.clean == prior_allocations[0] && value.semantic == prior_allocations[1]);
    });
    allocation = found == retained->allocations_.end() ? &retained->allocations_[retained->replacement_++ % retained->allocations_.size()] : &*found;
    initialize = !allocation->initialized || allocation->clean != clean.allocation || allocation->semantic != semantic.allocation || allocation->extent != submission->extent ||
                 allocation->padding != submission->options.atlas_padding;
    // A removed or moved region exposes padding and changes the layout.
    for (std::size_t index = 0U; !initialize && index < allocation->count; ++index)
     initialize = std::ranges::none_of(submission->regions, [&](const auto& region) { return region.crop == allocation->cells[index].crop; });
    if (initialize) *allocation = {};
    staged = *allocation;
    staged.clean = clean.allocation;
    staged.semantic = semantic.allocation;
    staged.extent = submission->extent;
    staged.padding = submission->options.atlas_padding;
    staged.initialized = true;
    staged.count = submission->regions.size();
   }
   if (initialize && (submission->regions.size() != 1U || submission->regions.front().crop != VisualRegion{0U, 0U, submission->extent.width, submission->extent.height})) {
    if (submission->options.atlas_padding) {
     constexpr auto color = raster::kAtlasPadding;
     constexpr unsigned packed = unsigned(color.r) | (unsigned(color.g) << 8U) | (unsigned(color.b) << 16U) | (unsigned(color.a) << 24U);
     const auto status = cuMemsetD2D32Async(clean.data, clean.descriptor.pitch_bytes, packed, clean.descriptor.width, clean.descriptor.height, reinterpret_cast<CUstream>(stream));
     if (status != CUDA_SUCCESS) throw std::runtime_error("validation atlas padding failed: " + std::to_string(status));
    } else
     clear(clean);
    clear(semantic);
   }
   const auto plane_region = [](gpu::ImagePlaneView plane, VisualRegion crop) {
    plane.data += static_cast<std::size_t>(crop.y) * plane.descriptor.pitch_bytes + static_cast<std::size_t>(crop.x) * 4U;
    plane.descriptor.width = crop.width;
    plane.descriptor.height = crop.height;
    return plane;
   };
   const auto& overlays = submission->options;
   for (std::size_t index = 0U; index < submission->regions.size(); ++index) {
    const auto& region = submission->regions[index];
    bool write_clean = true, write_semantic = true;
    if (allocation && !initialize) {
     for (std::size_t previous = 0U; previous < allocation->count; ++previous) {
      auto& cell = allocation->cells[previous];
      if (cell.crop != region.crop) continue;
      const bool same = cell.frame.lock() == region.frame && cell.capture == region.frame->state_->capture;
      write_clean = !same || !cell.clean;
      write_semantic = !same || !cell.semantic || cell.options != overlays;
      // Invalidate the real destination before its first possible write.
      if (write_clean) cell.clean = false;
      if (write_semantic) cell.semantic = false;
      break;
     }
    }
    if (write_clean || write_semantic)
     region.frame->DrawRegion(runtime, plane_region(clean, region.crop), plane_region(semantic, region.crop), stream, overlays.prediction_boxes, overlays.prediction_masks, overlays.ground_truth_boxes,
      overlays.ground_truth_masks, overlays.complementary_layers, write_clean, write_semantic, overlays.confidence_threshold);
    if (allocation) staged.cells[index] = {region.frame, region.frame->state_->capture, region.crop, overlays, true, true};
   }
  });
  if (allocation) *allocation = std::move(staged);
  // PublishRetained's completion, including its outer stream settlement,
  // proves every source read and staging upload complete before release.
  for (const auto& region : submission->regions) {
   const auto& frame = *region.frame;
   std::lock_guard lock(frame.state_->mutex);
   auto scope = frame.CompositionScope();
   scope.Run([&] {
    frame.state_->prepared = frame.state_->pending;
    frame.state_->rgb8 = nullptr;
    if (frame.state_->decoded_source) {
     frame.state_->source_context->Bind();
     frame.state_->decoded_source.reset();
    }
   });
  }
 } catch (...) {
  const auto failure = std::current_exception();
  // Failed submission may have partially overwritten scratch or upload storage.
  // Only previously settled, untouched preparation survives an ordinary failure.
  for (const auto& region : submission->regions) { region.frame->state_->pending = region.frame->state_->prepared; }
  const bool unsafe_frame = std::ranges::any_of(submission->regions, [](const auto& region) { return region.frame->state_->unsafe.load() != cudaSuccess; });
  if (gpu::is_image_execution_failure(failure) || unsafe_frame || (retirement && !retirement->admission_open())) {
   submission->output = runtime.Retire(failure).custody;
   if (retirement) {
    // These are the actual shared frame owners, retaining raw storage,
    // pinned staging, scratch, source contexts and decoded sources.
    // Install closes pool admission before the transaction can release.
    for (const auto& region : submission->regions) region.frame->state_->unsafe.store(cudaErrorUnknown);
    std::move(lease).Install(gpu::TerminalCudaCustody::Share(std::move(submission)), cudaErrorUnknown);
   }
   if (!gpu::is_image_execution_failure(failure)) throw gpu::ImageStreamExecutionFailure(failure);
  }
  std::rethrow_exception(failure);
 }
}
void PredictionPreviewFrame::DrawRegion(gpu::SystemImageRuntime& runtime, gpu::ImagePlaneView clean, gpu::ImagePlaneView semantic, std::uintptr_t stream, bool prediction_boxes, bool prediction_masks,
 bool ground_truth_boxes, bool ground_truth_masks, bool complementary_layers, bool write_clean, bool write_semantic, std::optional<float> confidence_threshold) const {
 if (!CompatibleWith(runtime)) throw std::runtime_error("Preview belongs to a retired visual context");
 auto& state = *state_;
 std::lock_guard state_lock(state.mutex);
 checked(state.unsafe);
 {
  auto scope = CompositionScope();
  scope.Run([&] {
   state.context.Bind();
   const auto cuda_stream = reinterpret_cast<cudaStream_t>(stream);
   checked(cudaStreamWaitEvent(cuda_stream, state.source_ready, 0U));
   auto* data = static_cast<std::uint8_t*>(state.storage.active());
   state.pending = state.prepared;
   const bool upload_gt = write_semantic && ground_truth_masks && !state.prepared.ground_truth && !state.ground_truth_runs.empty();
   if ((state.rgb8 || upload_gt) && !state.pinned) state.pinned = std::make_unique<gpu::PinnedHostBuffer>(scope.Current(), state.placement, false, state.register_host);
   const auto rgb_bytes = state.rgb8 ? static_cast<std::size_t>(state.extent.width) * state.extent.height * 3U : 0U;
   const auto gt_bytes = upload_gt ? state.ground_truth_runs.size() * sizeof(std::uint32_t) : 0U;
   if (rgb_bytes + gt_bytes != 0U) state.pinned->ensure_bytes(rgb_bytes + gt_bytes);
   if (state.rgb8) {
    const auto bytes = rgb_bytes;
    std::memcpy(state.pinned->data(), state.rgb8, bytes);
    checked(state.upload(data, state.pinned->data(), bytes, cudaMemcpyHostToDevice, cuda_stream));
   }
   const bool scale = clean.descriptor.width != state.extent.width || clean.descriptor.height != state.extent.height;
   if (scale && !state.composition) throw std::invalid_argument("preview composition storage was not admitted");
   const auto pitch = static_cast<std::size_t>(state.extent.width) * 4U;
   auto* clean_pixels = scale ? data + state.scratch_offset : reinterpret_cast<std::uint8_t*>(clean.data);
   auto* semantic_pixels = scale ? data + state.scratch_offset + pitch * state.extent.height : reinterpret_cast<std::uint8_t*>(semantic.data);
   const auto clean_pitch = scale ? pitch : clean.descriptor.pitch_bytes;
   const auto semantic_pitch = scale ? pitch : semantic.descriptor.pitch_bytes;
   raster::MutableBytes overlay{semantic_pixels, semantic_pitch, static_cast<int>(state.extent.width), static_cast<int>(state.extent.height)};
   if (write_clean && (!scale || !state.prepared.clean)) {
    if (scale) state.prepared.clean = false;
    const auto converted = state.pixels == State::Pixels::Rgb8 ? state.convert_rgb8(data, state.extent.width, state.extent.height, clean_pixels, clean_pitch, cuda_stream)
                                                               : state.convert(reinterpret_cast<const float*>(data), state.extent.width, state.extent.height, clean_pixels, clean_pitch, cuda_stream);
    checked(static_cast<cudaError_t>(converted));
    if (scale) state.pending.clean = true;
   }
   const PredictionPreviewComposition::Options semantic_options{prediction_boxes, prediction_masks, ground_truth_boxes, ground_truth_masks, complementary_layers, false, confidence_threshold};
   if (write_semantic && (!scale || !state.prepared.semantic || state.prepared.options != semantic_options)) {
    if (scale) state.prepared.semantic = false;
    checked(state.clear_semantic(semantic_pixels, semantic_pitch, 0, pitch, state.extent.height, cuda_stream));
    const auto draw_prediction = [&] {
     if (!state.predictions.empty() && (prediction_boxes || prediction_masks)) {
      if (!state.prepared.colors) {
       checked(static_cast<cudaError_t>(raster::build_category_colors_cuda(
        {reinterpret_cast<const int*>(data + state.labels_offset), state.predictions.size(), state.category_count, data + state.colors_offset, {reinterpret_cast<void*>(stream)}})));
       state.pending.colors = true;
      }
      checked(static_cast<cudaError_t>(raster::raster_instance_overlay_rgba({.overlay = overlay,
       .instances = {reinterpret_cast<const float*>(data + state.boxes_offset), data + state.colors_offset, reinterpret_cast<const int*>(data + state.labels_offset),
        static_cast<int>(state.predictions.size())},
       .masks = prediction_masks && state.masks ? reinterpret_cast<const bool*>(data + state.masks_offset) : nullptr,
       .mask_alpha = 96U,
       .box_thickness = prediction_boxes ? 2 : 0,
       .stream = {reinterpret_cast<void*>(stream)},
       .labels = !state.composition,
       .add_rgb_to_existing = complementary_layers && !state.ground_truth.empty() && (ground_truth_boxes || ground_truth_masks),
       .confidences = confidence_threshold ? reinterpret_cast<const float*>(data + state.confidences_offset) : nullptr,
       .confidence_threshold = confidence_threshold.value_or(0.0F)})));
     }
    };
    if (!complementary_layers) draw_prediction();
    if (upload_gt) {
     const auto bytes = state.ground_truth_runs.size() * sizeof(std::uint32_t);
     auto* staging = static_cast<std::uint8_t*>(state.pinned->data()) + rgb_bytes;
     std::memcpy(staging, state.ground_truth_runs.data(), bytes);
     checked(state.upload(data + state.ground_truth_offset, staging, bytes, cudaMemcpyHostToDevice, cuda_stream));
     state.pending.ground_truth = true;
    }
    std::size_t word = 0U, gt_index = 0U;
    for (const auto& gt : state.ground_truth) {
     raster::RgbColor color{state.ground_truth_colors[gt_index * 3U], state.ground_truth_colors[gt_index * 3U + 1U], state.ground_truth_colors[gt_index * 3U + 2U]};
     if (complementary_layers) {
      color.r = 255U - color.r;
      color.g = 255U - color.g;
      color.b = 255U - color.b;
     }
     ++gt_index;
     if (ground_truth_masks && !gt.mask.runs.empty())
      checked(static_cast<cudaError_t>(raster::raster_mask_runs_rgba({.overlay = overlay,
       .run_pairs = reinterpret_cast<const std::uint32_t*>(data + state.ground_truth_offset) + word,
       .run_count = static_cast<std::uint32_t>(gt.mask.runs.size()),
       .color = {color.r, color.g, color.b, 96U},
       .stream = {reinterpret_cast<void*>(stream)}})));
     if (ground_truth_boxes) {
      const raster::IntRect box{
       static_cast<int>(std::floor(gt.bbox_xyxy[0])), static_cast<int>(std::floor(gt.bbox_xyxy[1])), static_cast<int>(std::ceil(gt.bbox_xyxy[2])), static_cast<int>(std::ceil(gt.bbox_xyxy[3]))};
      checked(static_cast<cudaError_t>(raster::raster_box_outline_rgba({.overlay = overlay,
       .box = box,
       .color = {color.r, color.g, color.b},
       .thickness = 1,
       .stream = {reinterpret_cast<void*>(stream)},
       .clip = {static_cast<int>(std::max<std::int64_t>(0, std::int64_t{box.x1} - 1)), static_cast<int>(std::max<std::int64_t>(0, std::int64_t{box.y1} - 1)),
        static_cast<int>(std::min<std::int64_t>(overlay.width, std::int64_t{box.x2} + 1)), static_cast<int>(std::min<std::int64_t>(overlay.height, std::int64_t{box.y2} + 1))}})));
     }
     word += gt.mask.runs.size() * 2U;
    }
    if (complementary_layers) draw_prediction();
    if (scale) {
     state.pending.semantic = true;
     state.pending.options = semantic_options;
    }
   }
   if (scale) {
    if (write_clean)
     checked(static_cast<cudaError_t>(raster::scale_rgba({clean_pixels, clean_pitch, overlay.width, overlay.height},
      {reinterpret_cast<std::uint8_t*>(clean.data), clean.descriptor.pitch_bytes, static_cast<int>(clean.descriptor.width), static_cast<int>(clean.descriptor.height)}, stream)));
    if (write_semantic)
     checked(static_cast<cudaError_t>(raster::scale_rgba({semantic_pixels, semantic_pitch, overlay.width, overlay.height},
      {reinterpret_cast<std::uint8_t*>(semantic.data), semantic.descriptor.pitch_bytes, static_cast<int>(semantic.descriptor.width), static_cast<int>(semantic.descriptor.height)}, stream)));
   }
  });
  // The enclosing PublishRetained owns completion and partial-write settlement.
  // Keep staging and decoded custody alive until that actual boundary.
 }
}
}  // namespace mmltk::controller::detail
