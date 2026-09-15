#include "prediction_preview.h"
#include "src/backend/data/image_resize.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/backend/imaging/raster/chw_image.h"
#include "src/backend/models/rfdetr/contract/prediction_limits.h"
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
import mmltk.backend.imaging.raster;
namespace mmltk::controller::detail {
namespace gpu = mmltk::frameworks::gpu;
namespace raster = mmltk::backend::imaging::raster;
namespace rfdetr = mmltk::backend::models::rfdetr;
namespace {
void checked(cudaError_t status) { if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status)); }
}
gpu::DeviceContext CreatePredictionPreviewContext(const gpu::DeviceExecution& execution,
    const std::shared_ptr<gpu::TerminalCudaRetirementOwner>& retirement, gpu::CudaContextApi api) {
    if (!retirement) throw std::invalid_argument("prediction preview retirement authority is unavailable");
    auto lease = gpu::ReserveTerminalCudaLease(*retirement);
    auto candidate = std::make_shared<std::optional<gpu::DeviceContext>>();
    struct Construction final {
        gpu::TerminalCudaRetirementLease& lease;
        std::shared_ptr<std::optional<gpu::DeviceContext>>& candidate;
    } construction{lease, candidate};
    if (cuInit(0U) != CUDA_SUCCESS) throw std::runtime_error("prediction CUDA initialization failed");
    gpu::CudaContextScope scope({&construction, [](void* owner) noexcept {
        auto& value = *static_cast<Construction*>(owner);
        auto retained = value.candidate;
        std::move(value.lease).Install(gpu::TerminalCudaCustody::Share(std::move(retained)), cudaErrorUnknown);
    }}, api);
    scope.Run([&] {
        candidate->emplace(execution.device, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated,
            execution.placement.numa_node, execution);
    });
    return **candidate;
}
struct PredictionPreviewFrame::State final {
    explicit State(const gpu::DeviceContext& receiver, gpu::CudaContextApi api, std::shared_ptr<void> source)
        : context(receiver), context_api(api), source_custody(std::move(source)) {}
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
    const std::uint8_t* rgb8 = nullptr;
    decltype(&cuMemHostRegister) register_host = &cuMemHostRegister;
    decltype(&cudaMemcpyAsync) upload = &cudaMemcpyAsync;
    decltype(&raster::chw_float_to_rgba) convert = &raster::chw_float_to_rgba;
    mmltk::common::system::ExecutionPlacement placement;
    std::shared_ptr<void> decoded_source;
    std::unique_ptr<gpu::PinnedHostBuffer> pinned;
    std::shared_ptr<void> source_custody;
    std::atomic<cudaError_t> unsafe{cudaSuccess};
    gpu::CudaHighWaterAllocation<void*> storage;
    std::size_t capacity = 0U;
    VisualExtent extent;
    std::size_t boxes_offset{}, labels_offset{}, masks_offset{}, colors_offset{};
    bool masks = false;
    int category_count = 0;
    std::shared_ptr<const std::vector<std::string>> catalog;
    std::vector<rfdetr::Prediction> predictions;
};
PredictionPreviewFrame::PredictionPreviewFrame(const gpu::DeviceContext& context,
    std::shared_ptr<gpu::TerminalCudaRetirementOwner> retirement, std::shared_ptr<void> source, gpu::CudaContextApi api)
    : retirement_(std::move(retirement)) {
    auto lease = retirement_->Reserve();
    if (!lease) throw std::runtime_error("prediction preview retirement admission is closed");
    lease_ = std::move(*lease);
    // No GPU work occurs until this exact partially initialized state is retainable.
    state_ = std::make_shared<State>(context, api, std::move(source));
}
gpu::CudaContextScope PredictionPreviewFrame::ContextScope() const noexcept {
    return gpu::CudaContextScope({const_cast<PredictionPreviewFrame*>(this), [](void* owner) noexcept {
        static_cast<PredictionPreviewFrame*>(owner)->RetainUnsafe(cudaErrorUnknown);
    }}, state_->context_api);
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
std::span<const rfdetr::Prediction> PredictionPreviewFrame::predictions() const noexcept { return state_->predictions; }
const std::vector<std::string>& PredictionPreviewFrame::classes() const noexcept { return *state_->catalog; }
int PredictionPreviewFrame::class_count() const noexcept { return state_->category_count; }
PredictionPreviewPool::PredictionPreviewPool(gpu::DeviceExecution execution, gpu::DeviceContext context, TransferOperations operations,
                                           std::shared_ptr<gpu::TerminalCudaRetirementOwner> retirement)
    : operations_(operations), execution_(std::move(execution)), context_(std::move(context)),
      retirement_(retirement ? std::move(retirement) : std::make_shared<gpu::TerminalCudaRetirementOwner>(kSlotCapacity)) {
    if (!retirement_->admission_open()) throw std::runtime_error("prediction preview retirement admission is closed");
    if (!operations_.copy || !operations_.record || !operations_.settle || !operations_.register_host || !operations_.upload || !operations_.convert || !operations_.context_api.get || !operations_.context_api.set)
        throw std::invalid_argument("prediction transfer operations are incomplete");
    context_.ValidateSelection(execution_.device, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated,
                               execution_.placement.numa_node, execution_);
}
std::shared_ptr<const PredictionPreviewFrame> PredictionPreviewPool::Capture(const float* pixels, VisualExtent extent,
    std::uintptr_t source_stream, std::span<const rfdetr::Prediction> predictions,
    const mmltk::backend::ml::runtime::AnalysisAnnotationStorage& annotations, std::shared_ptr<const std::vector<std::string>> catalog, int classes, const std::uint8_t* rgb8,
    std::shared_ptr<void> source_custody, void (*stop_source)(void*), void* source_control) {
    if (!retirement_->admission_open()) throw std::runtime_error("prediction preview CUDA retirement failed");
    auto available = std::ranges::find_if(slots_, [](const auto& slot) {
        return !slot || (slot.use_count() == 1 && slot->state_->unsafe == cudaSuccess);
    });
    if (available == slots_.end()) return {};
    if ((!pixels && !rgb8) || (pixels && rgb8) || !source_custody || !extent.valid() || predictions.size() > contracts::kAnnotationObjectCapacity || !catalog || classes <= 0 || static_cast<std::size_t>(classes) > contracts::kAnnotationCategoryCapacity)
        throw std::invalid_argument("prediction preview source is invalid");
    const auto count = predictions.size();
    for (const auto& prediction : predictions) {
        const auto category = prediction.category_id - 1;
        if (category >= 0 && static_cast<std::size_t>(category) < catalog->size() && (*catalog)[category].size() > mmltk::frameworks::reflection::kMaximumNameBytes)
            throw std::invalid_argument("prediction label exceeds the visual name capacity");
    }
    const auto pixel_count = rfdetr::checked_prediction_extent(extent.width, extent.height, rfdetr::kMaximumEncodedMaskPixels);
    const auto pixel_bytes = rfdetr::checked_prediction_extent(pixel_count, 3U * sizeof(float), rfdetr::kMaximumPredictionTensorBytes);
    const auto mask_bytes = annotations.masks.address && count ? rfdetr::checked_prediction_extent(pixel_count, count, rfdetr::kMaximumPredictionTensorBytes) : 0U;
    const auto bytes = pixel_bytes + count * (4U * sizeof(float) + sizeof(std::int32_t) + 3U) + mask_bytes;
    if (bytes > rfdetr::kMaximumPredictionTensorBytes) throw std::invalid_argument("prediction raw preview exceeds storage capacity");
    try {
        if (!*available) *available = std::shared_ptr<PredictionPreviewFrame>(new PredictionPreviewFrame(context_, retirement_, source_custody, operations_.context_api));
        auto& state = *(*available)->state_;
        std::unique_lock state_lock(state.mutex, std::try_to_lock);
        if (!state_lock) return {};
        checked(state.unsafe);
        state.source_custody = source_custody;
        auto scope = (*available)->ContextScope();
        scope.Run([&] {
            if (!state.source_context) {
                state.source_context.emplace(execution_.device, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::PrimaryInterop,
                    execution_.placement.numa_node, execution_);
            }
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
            state.rgb8 = rgb8;
            state.decoded_source = rgb8 ? source_custody : std::shared_ptr<void>{};
            state.register_host = operations_.register_host;
            state.upload = operations_.upload;
            state.convert = operations_.convert;
            state.placement = execution_.placement;
            state.extent = extent;
            state.boxes_offset = pixel_bytes;
            state.labels_offset = state.boxes_offset + count * 4U * sizeof(float);
            state.masks_offset = state.labels_offset + count * sizeof(std::int32_t);
            state.colors_offset = state.masks_offset + mask_bytes;
            state.masks = mask_bytes != 0U;
            state.catalog = std::move(catalog);
            state.category_count = classes;
            state.predictions.clear();
            state.predictions.reserve(count);
            for (const auto& prediction : predictions)
                state.predictions.push_back({.category_id = prediction.category_id, .score = prediction.score, .bbox_xyxy = prediction.bbox_xyxy});
            auto* destination = static_cast<std::uint8_t*>(state.storage.active());
            bool source_submitted = false;
            try {
                const auto receiver_context = scope.Current();
                auto source_scope = (*available)->ContextScope();
                source_scope.Run([&] {
                    state.source_context->Bind();
                    const auto source_context = source_scope.Current();
                    // Custody reads share the source stream. Reuse is ordered even if a later
                    // copy or event record fails; no receiver-stream wait can be lost.
                    const auto copy = [&](void* target, const void* source, std::size_t size) {
                        source_submitted = true;
                        if (operations_.copy(reinterpret_cast<CUdeviceptr>(target), receiver_context,
                                              reinterpret_cast<CUdeviceptr>(source), source_context, size,
                                              reinterpret_cast<CUstream>(source_stream)) != CUDA_SUCCESS)
                            throw std::runtime_error("prediction raw custody copy failed");
                    };
                    if (pixels) copy(destination, pixels, pixel_bytes);
                    if (count) {
                        copy(destination + state.boxes_offset, reinterpret_cast<const void*>(annotations.boxes_xyxy.address), count * 4U * sizeof(float));
                        copy(destination + state.labels_offset, reinterpret_cast<const void*>(annotations.category_ids.address), count * sizeof(std::int32_t));
                        if (mask_bytes) copy(destination + state.masks_offset, reinterpret_cast<const void*>(annotations.masks.address), mask_bytes);
                    }
                    checked(operations_.record(state.source_ready, reinterpret_cast<cudaStream_t>(source_stream)));
                    state.source_recorded = true;
                });
            } catch (...) {
                const auto failure = std::current_exception();
                try { std::rethrow_exception(failure); }
                catch (const gpu::CudaContextFailure& error) { if (error.terminal()) throw; }
                catch (...) {}
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
            if (stop_source) { try { stop_source(source_control); } catch (...) {} }
            throw mmltk::backend::ml::runtime::CudaOperationError{cudaErrorUnknown, "prediction caller context custody"};
        }
        std::rethrow_exception(failure);
    }
}
bool PredictionPreviewFrame::CompatibleWith(const gpu::SystemImageRuntime& runtime) const noexcept {
    return runtime.UsesContext(state_->context);
}
void PredictionPreviewFrame::Draw(gpu::SystemImageRuntime& runtime, gpu::SystemImageRuntime::OutputCandidate& candidate) const {
    if (!CompatibleWith(runtime)) throw std::runtime_error("Prediction preview belongs to a retired visual context");
    auto& state = *state_;
    std::lock_guard state_lock(state.mutex);
    checked(state.unsafe);
    try {
        auto scope = ContextScope();
        scope.Run([&] {
            state.context.Bind();
            if (state.rgb8 && !state.pinned)
                state.pinned = std::make_unique<gpu::PinnedHostBuffer>(scope.Current(), state.placement, false, state.register_host);
            runtime.PublishRetained(candidate, state.extent.width, state.extent.height, [&](auto clean, auto semantic, auto stream) {
                const auto cuda_stream = reinterpret_cast<cudaStream_t>(stream);
                checked(cudaStreamWaitEvent(cuda_stream, state.source_ready, 0U));
                auto* data = static_cast<std::uint8_t*>(state.storage.active());
                if (state.rgb8) {
                    const auto pixel_bytes = static_cast<std::size_t>(state.extent.width) * state.extent.height * 3U * sizeof(float);
                    state.pinned->ensure_bytes(pixel_bytes);
                    mmltk::backend::data::rgb_hwc_u8_to_nchw_f32(state.rgb8, static_cast<float*>(state.pinned->data()), state.extent.width, state.extent.height);
                    checked(state.upload(data, state.pinned->data(), pixel_bytes, cudaMemcpyHostToDevice, cuda_stream));
                }
                checked(static_cast<cudaError_t>(state.convert(reinterpret_cast<const float*>(data), state.extent.width, state.extent.height,
                    reinterpret_cast<std::uint8_t*>(clean.data), clean.descriptor.pitch_bytes, stream)));
                checked(cudaMemset2DAsync(reinterpret_cast<void*>(semantic.data), semantic.descriptor.pitch_bytes, 0,
                    semantic.descriptor.row_bytes(), semantic.descriptor.height, cuda_stream));
                if (!state.predictions.empty()) {
                    checked(static_cast<cudaError_t>(raster::build_category_colors_cuda({reinterpret_cast<const int*>(data + state.labels_offset), state.predictions.size(),
                        state.category_count, data + state.colors_offset, {reinterpret_cast<void*>(stream)}})));
                    checked(static_cast<cudaError_t>(raster::raster_instance_overlay_rgba({
                        .overlay = {reinterpret_cast<std::uint8_t*>(semantic.data), semantic.descriptor.pitch_bytes, static_cast<int>(state.extent.width), static_cast<int>(state.extent.height)},
                        .instances = {reinterpret_cast<const float*>(data + state.boxes_offset), data + state.colors_offset,
                            reinterpret_cast<const int*>(data + state.labels_offset), static_cast<int>(state.predictions.size())},
                        .masks = state.masks ? reinterpret_cast<const bool*>(data + state.masks_offset) : nullptr,
                        .mask_alpha = 96U, .box_thickness = 2, .stream = {reinterpret_cast<void*>(stream)}})));
                }
            });
            state.rgb8 = nullptr;
            if (state.decoded_source) {
                state.source_context->Bind();
                state.decoded_source.reset();
            }
        });
    } catch (const gpu::CudaContextFailure& error) {
        if (error.terminal()) {
            static_cast<void>(runtime.Retire(std::current_exception()));
            throw gpu::ImageStreamExecutionFailure(std::current_exception());
        }
        throw;
    } catch (...) {
        const auto failure = std::current_exception();
        if (gpu::is_image_execution_failure(failure)) RetainUnsafe(cudaErrorUnknown);
        if (state.unsafe != cudaSuccess) static_cast<void>(runtime.Retire(failure));
        throw;
    }
}
}
