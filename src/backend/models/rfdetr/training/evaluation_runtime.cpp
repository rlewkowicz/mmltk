#include "src/backend/ml/cuda/numa_host_tensor.h"
#include <ATen/Context.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime.h>
#include <torch/torch.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "detection_types.h"
#include "mask_pack_cuda.h"
#include "postprocess.h"
#include "src/backend/data/dataset_loader.h"
#include "detail/evaluation_runtime.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "torch_api.h"
#include "torch_cuda_utils.h"
import mmltk.backend.ml.cuda.gpu_quiescence;
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
namespace mmltk::backend::models::rfdetr::evaluator_detail {
using CudaGuard = ::c10::cuda::CUDAGuard;
using CudaStreamGuard = ::c10::cuda::CUDAStreamGuard;
using DeviceIndex = ::c10::DeviceIndex;
using ::cudaError_t;
using ::cudaEvent_t;
using ::cudaEventCreate;
using ::cudaEventCreateWithFlags;
using ::cudaEventDestroy;
using ::cudaEventElapsedTime;
using ::cudaEventRecord;
using ::cudaEventSynchronize;
using ::cudaGetDevice;
using ::cudaSetDevice;
using ::cudaStream_t;
using ::cudaStreamSynchronize;
using ::cudaSuccess;
using ::c10::cuda::getStreamFromExternal;
using ::mmltk::backend::ml::cuda::checked_device_index;
inline constexpr unsigned int kCudaEventDisableTiming = cudaEventDisableTiming;
}  // namespace mmltk::backend::models::rfdetr::evaluator_detail
namespace mmltk::backend::models::rfdetr {
namespace torch_api = mmltk::backend::ml::torch_api;
using evaluator_detail::checked_device_index;
using evaluator_detail::cudaError_t;
using evaluator_detail::cudaEvent_t;
using evaluator_detail::cudaEventCreate;
using evaluator_detail::cudaEventCreateWithFlags;
using evaluator_detail::cudaEventDestroy;
using evaluator_detail::cudaEventElapsedTime;
using evaluator_detail::cudaEventRecord;
using evaluator_detail::cudaEventSynchronize;
using evaluator_detail::cudaGetDevice;
using evaluator_detail::CudaGuard;
using evaluator_detail::cudaSetDevice;
using evaluator_detail::cudaStream_t;
using evaluator_detail::CudaStreamGuard;
using evaluator_detail::cudaStreamSynchronize;
using evaluator_detail::cudaSuccess;
using evaluator_detail::DeviceIndex;
using evaluator_detail::getStreamFromExternal;
using evaluator_detail::kCudaEventDisableTiming;
using mmltk::frameworks::gpu::ensure_cuda_ok;
void capture_sdp_backend_flags(EvaluationProfileRecord& profile) {
    const auto& context = at::globalContext();
    profile.sdp_flash_enabled = context.userEnabledFlashSDP();
    profile.sdp_mem_efficient_enabled = context.userEnabledMemEfficientSDP();
    profile.sdp_math_enabled = context.userEnabledMathSDP();
    profile.sdp_cudnn_enabled = context.userEnabledCuDNNSDP();
}
void accumulate_prediction_transfer_bytes(EvaluationProfileRecord& profile, const PostprocessedBatch& processed, const size_t image_count) {
    const size_t prediction_count = image_count * static_cast<size_t>(processed.scores.size(1));
    const size_t bbox_bytes = prediction_count * (static_cast<size_t>(processed.scores.element_size()) + static_cast<size_t>(processed.labels.element_size()) +
                                                  4U * static_cast<size_t>(processed.boxes.element_size()));
    size_t mask_bytes = 0;
    if (processed.masks.has_value()) {
        const size_t pixels_per_mask = static_cast<size_t>(processed.masks->size(2)) * static_cast<size_t>(processed.masks->size(3));
        mask_bytes = prediction_count * ((pixels_per_mask + 7U) / 8U);
    }
    profile.transferred_bytes += bbox_bytes + mask_bytes;
    profile.mask_transferred_bytes += mask_bytes;
}
const char* evaluation_precision_name(const at::ScalarType scalar_type) noexcept {
    switch (scalar_type) {
        case at::kFloat: return "fp32";
        case at::kHalf: return "fp16";
        case at::kBFloat16: return "bf16";
        default: return "other";
    }
}
EvaluationCudaBatchTiming::EvaluationCudaBatchTiming() {
    for (cudaEvent_t& event : events_) {
        const cudaError_t status = cudaEventCreate(&event);
        if (status != cudaSuccess) {
            destroy_events();
            ensure_cuda_ok(status, "cudaEventCreate for validation profile timing");
        }
    }
}
EvaluationCudaBatchTiming::~EvaluationCudaBatchTiming() { destroy_events(); }
void EvaluationCudaBatchTiming::record_start(const Phase phase, cudaStream_t stream) {
    ensure_cuda_ok(cudaEventRecord(events_[event_index(phase, false)], stream), "cudaEventRecord for validation profile phase start");
}
void EvaluationCudaBatchTiming::record_stop(const Phase phase, cudaStream_t stream) {
    ensure_cuda_ok(cudaEventRecord(events_[event_index(phase, true)], stream), "cudaEventRecord for validation profile phase stop");
}
void EvaluationCudaBatchTiming::accumulate(EvaluationProfileRecord& profile) const {
    ensure_cuda_ok(cudaEventSynchronize(events_[event_index(Phase::Postprocess, true)]), "cudaEventSynchronize for validation profile timing");
    profile.preprocessing_seconds += elapsed_seconds(Phase::Preprocessing);
    profile.model_forward_seconds += elapsed_seconds(Phase::ModelForward);
    profile.postprocess_seconds += elapsed_seconds(Phase::Postprocess);
}
double EvaluationCudaBatchTiming::elapsed_seconds(const Phase phase) const {
    float elapsed_milliseconds = 0.0F;
    ensure_cuda_ok(cudaEventElapsedTime(&elapsed_milliseconds, events_[event_index(phase, false)], events_[event_index(phase, true)]),
                   "cudaEventElapsedTime for validation profile phase");
    return static_cast<double>(elapsed_milliseconds) / 1000.0;
}
void EvaluationCudaBatchTiming::destroy_events() noexcept {
    for (cudaEvent_t& event : events_) {
        if (event != nullptr) {
            cudaEventDestroy(event);
            event = nullptr;
        }
    }
}
EvaluationCudaTimingPool::EvaluationCudaTimingPool(const size_t slot_count) {
    if (slot_count == 0) { throw std::invalid_argument("validation CUDA timing pool requires at least one slot"); }
    slots_.reserve(slot_count);
    free_slots_.reserve(slot_count);
    for (size_t slot_index = 0; slot_index < slot_count; ++slot_index) {
        slots_.push_back(std::make_unique<EvaluationCudaBatchTiming>());
        free_slots_.push_back(slot_count - slot_index - 1U);
    }
}
EvaluationCudaTimingLease EvaluationCudaTimingPool::acquire() {
    if (free_slots_.empty()) { throw std::logic_error("validation CUDA timing pool exhausted"); }
    const size_t slot_index = free_slots_.back();
    free_slots_.pop_back();
    return EvaluationCudaTimingLease{slots_[slot_index].get(), slot_index};
}
void EvaluationCudaTimingPool::release(EvaluationCudaTimingLease& lease) {
    if (!lease || lease.slot_index >= slots_.size() || slots_[lease.slot_index].get() != lease.timing) {
        throw std::logic_error("invalid validation CUDA timing lease release");
    }
    free_slots_.push_back(lease.slot_index);
    lease = {};
}
namespace {
struct SelectedPredictions {
    std::vector<Prediction> predictions;
    std::vector<int64_t> mask_source_indices;
};
SelectedPredictions select_predictions(int image_id, const torch_api::Tensor& scores, const torch_api::Tensor& labels, const torch_api::Tensor& boxes,
                                       size_t category_count, size_t max_dets_per_image) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_select_predictions{"rfdetr.native.eval.select_predictions"};
    const auto* score_ptr = scores.data_ptr<float>();
    const auto* label_ptr = labels.data_ptr<int64_t>();
    const auto* box_ptr = boxes.data_ptr<float>();
    const int64_t total = scores.size(0);
    SelectedPredictions out;
    out.predictions.reserve(std::min<size_t>(static_cast<size_t>(total), max_dets_per_image));
    out.mask_source_indices.reserve(out.predictions.capacity());
    for (int64_t index = 0; index < total; ++index) {
        const int64_t label = label_ptr[index];
        if (label < 0 || label >= static_cast<int64_t>(category_count)) { continue; }
        Prediction prediction;
        prediction.image_id = image_id;
        prediction.class_reference = static_cast<int>(label);
        prediction.score = score_ptr[index];
        prediction.bbox_xyxy = xyxy_clamped(box_ptr + index * 4);
        out.predictions.push_back(std::move(prediction));
        out.mask_source_indices.push_back(index);
        if (out.predictions.size() >= max_dets_per_image) { break; }
    }
    return out;
}
}  // namespace
PredictionBufferSlotPool::PredictionBufferSlotPool(const size_t slot_count, const PredictionBufferConfig& config) {
    if (slot_count == 0) { throw std::runtime_error("prediction buffer slot pool requires at least one slot"); }
    if (config.batch_capacity <= 0 || config.prediction_capacity <= 0 || config.device_id < 0) {
        throw std::invalid_argument("prediction buffer configuration requires positive batch and prediction capacity");
    }
    if (config.mask_shape && (config.mask_shape->first == 0 || config.mask_shape->second == 0)) {
        throw std::invalid_argument("prediction mask buffer configuration requires positive dimensions");
    }
    CudaGuard device_guard(checked_device_index(config.device_id));
    slots_.reserve(slot_count);
    const auto& mask_shape = config.mask_shape;
    for (size_t slot_index = 0; slot_index < slot_count; ++slot_index) {
        auto slot = std::make_shared<PinnedPredictionBuffers>();
        slot->allow_mask_reconfiguration = config.allow_mask_reconfiguration;
        slot->bbox.ensure_capacity(config.batch_capacity, config.prediction_capacity);
        if (mask_shape) {
            PinnedMaskPredictionBuffers& mask_buffers = slot->mask.emplace();
            mask_buffers.ensure_capacity(config.batch_capacity, config.prediction_capacity, config.batch_capacity, config.prediction_capacity,
                                         mask_shape->first, mask_shape->second, config.device_id);
        }
        slot->ensure_ready_event(config.device_id);
        slots_.push_back(std::move(slot));
        free_slots_.push_back(slot_index);
    }
}
PredictionBufferLease PredictionBufferSlotPool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !free_slots_.empty(); });
    const size_t slot_index = free_slots_.front();
    free_slots_.pop_front();
    PredictionBufferLease lease;
    lease.buffers = slots_.at(slot_index);
    lease.buffers->transition(PredictionSlotState::Free, PredictionSlotState::GpuFilling, "prediction slot acquisition");
    lease.release_guard = std::shared_ptr<void>(nullptr, [pool = shared_from_this(), slot_index](void*) { pool->release(slot_index); });
    return lease;
}
void PredictionBufferSlotPool::release(size_t slot_index) {
    const std::shared_ptr<PinnedPredictionBuffers>& slot = slots_.at(slot_index);
    if (slot->state.load(std::memory_order_acquire) == PredictionSlotState::D2HPending && slot->ready_event != nullptr) {
        CudaGuard device_guard(checked_device_index(slot->event_device_id));
        static_cast<void>(cudaEventSynchronize(slot->ready_event));
    }
    slot->state.store(PredictionSlotState::Free, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        free_slots_.push_back(slot_index);
    }
    cv_.notify_one();
}
PinnedPredictionBuffers::~PinnedPredictionBuffers() {
    if (ready_event != nullptr) {
        int previous_device_id = -1;
        const bool restore_device = event_device_id >= 0 && cudaGetDevice(&previous_device_id) == cudaSuccess && previous_device_id != event_device_id &&
                                    cudaSetDevice(event_device_id) == cudaSuccess;
        cudaEventDestroy(ready_event);
        if (restore_device) { cudaSetDevice(previous_device_id); }
        ready_event = nullptr;
    }
}
void PinnedBBoxPredictionBuffers::ensure_capacity(int64_t batch_count, int64_t prediction_count) {
    if (batch_capacity >= batch_count && prediction_capacity >= prediction_count && scores_cpu.defined() && labels_cpu.defined() && boxes_cpu.defined()) {
        return;
    }
    batch_capacity = std::max<int64_t>(batch_capacity, batch_count);
    prediction_capacity = std::max<int64_t>(prediction_capacity, prediction_count);
    scores_cpu = mmltk::backend::ml::cuda::numa_empty({batch_capacity, prediction_capacity}, torch_api::kFloat32);
    labels_cpu = mmltk::backend::ml::cuda::numa_empty({batch_capacity, prediction_capacity}, torch_api::kInt64);
    boxes_cpu = mmltk::backend::ml::cuda::numa_empty({batch_capacity, prediction_capacity, 4}, torch_api::kFloat32);
}
void PinnedMaskPredictionBuffers::ensure_capacity(const int64_t batch_count, const int64_t prediction_count, const int64_t batch_capacity,
                                                  const int64_t prediction_capacity, const uint32_t height, const uint32_t width, const int device_id) {
    const int64_t required_bytes = (mmltk::common::math::checked_cast<int64_t>(height, "prediction mask height overflow") *
                                        mmltk::common::math::checked_cast<int64_t>(width, "prediction mask width overflow") +
                                    7) /
                                   8;
    if (masks_cpu.defined() && masks_cpu.dim() == 3 && masks_cpu.size(0) >= batch_count && masks_cpu.size(1) >= prediction_count && masks_gpu.defined() &&
        masks_gpu.device().is_cuda() && masks_gpu.get_device() == device_id && mask_height == height && mask_width == width &&
        packed_mask_bytes == required_bytes) {
        return;
    }
    mask_height = height;
    mask_width = width;
    packed_mask_bytes = required_bytes;
    const std::vector<int64_t> packed_shape{batch_capacity, prediction_capacity, packed_mask_bytes};
    masks_cpu = mmltk::backend::ml::cuda::numa_empty(packed_shape, torch_api::kUInt8, device_id);
    masks_gpu = torch_api::empty(packed_shape, torch_api::TensorOptions().dtype(torch_api::kUInt8).device(torch_api::kCUDA, device_id));
}
void PinnedPredictionBuffers::ensure_ready_event(int device_id) {
    if (ready_event != nullptr && event_device_id == device_id) { return; }
    if (ready_event != nullptr) {
        CudaGuard previous_device_guard(checked_device_index(event_device_id));
        ensure_cuda_ok(cudaEventDestroy(ready_event), "cudaEventDestroy for prediction staging slot");
        ready_event = nullptr;
    }
    CudaGuard device_guard(checked_device_index(device_id));
    ensure_cuda_ok(cudaEventCreateWithFlags(&ready_event, kCudaEventDisableTiming), "cudaEventCreateWithFlags for prediction staging slot");
    event_device_id = device_id;
}
void PinnedPredictionBuffers::transition(const PredictionSlotState expected, const PredictionSlotState next, const char* operation) {
    PredictionSlotState observed = expected;
    if (!state.compare_exchange_strong(observed, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
        throw std::logic_error(std::string(operation) + " encountered an invalid prediction slot state");
    }
}
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
StagedPredictionBatch stage_prediction_batch(std::vector<PredictionBatchMetadata> images, PostprocessedBatch batch, size_t category_count,
                                             size_t max_dets_per_image, PredictionBufferLease lease, int device_id, void* stream_handle) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_stage_prediction_batch{"rfdetr.native.eval.stage_prediction_batch"};
    if (!lease.buffers) { throw std::runtime_error("stage_prediction_batch requires a valid prediction buffer lease"); }
    if (!batch.scores.defined() || batch.scores.dim() != 2 || !batch.labels.defined() || batch.labels.dim() != 2 || !batch.boxes.defined() ||
        batch.boxes.dim() != 3 || batch.boxes.size(2) != 4) {
        throw std::runtime_error("staged prediction tensors must be scores[B,K], labels[B,K], and boxes[B,K,4]");
    }
    const int64_t batch_count = mmltk::common::math::checked_cast<int64_t>(images.size(), "prediction batch size overflow");
    const int64_t prediction_count = batch.scores.size(1);
    if (batch_count <= 0 || batch.size() < batch_count || batch.labels.size(0) < batch_count || batch.labels.size(1) != prediction_count ||
        batch.boxes.size(0) < batch_count || batch.boxes.size(1) != prediction_count) {
        throw std::runtime_error("prediction metadata and staged tensor shapes are not aligned");
    }
    PinnedPredictionBuffers& buffers = *lease.buffers;
    if (buffers.bbox.batch_capacity < batch_count || buffers.bbox.prediction_capacity < prediction_count || !buffers.bbox.scores_cpu.defined() ||
        !buffers.bbox.labels_cpu.defined() || !buffers.bbox.boxes_cpu.defined()) {
        throw std::runtime_error("prediction batch exceeds the configured bbox staging capacity");
    }
    if (buffers.ready_event == nullptr || buffers.event_device_id != device_id) {
        throw std::runtime_error("prediction batch device does not match the configured staging slot");
    }
    const auto score_view = buffers.bbox.scores_cpu.narrow(0, 0, batch_count).narrow(1, 0, prediction_count);
    const auto label_view = buffers.bbox.labels_cpu.narrow(0, 0, batch_count).narrow(1, 0, prediction_count);
    const auto box_view = buffers.bbox.boxes_cpu.narrow(0, 0, batch_count).narrow(1, 0, prediction_count);
    if (score_view.stride(1) != 1 || label_view.stride(1) != 1 || box_view.stride(1) != 4 || box_view.stride(2) != 1) {
        throw std::logic_error("pinned prediction bbox buffers have unexpected strides");
    }
    auto stream = reinterpret_cast<cudaStream_t>(stream_handle);
    if (stream == nullptr) { throw std::runtime_error("stage_prediction_batch requires a CUDA stream"); }
    std::optional<StagedMaskPredictionBatch> staged_mask;
    try {
        const DeviceIndex checked_index = checked_device_index(device_id);
        CudaGuard device_guard(checked_index);
        CudaStreamGuard stream_guard(getStreamFromExternal(stream, checked_index));
        {
            mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_d2h_boxes_labels_scores{"rfdetr.native.eval.d2h_boxes_labels_scores"};
            score_view.copy_(batch.scores.narrow(0, 0, batch_count), true);
            label_view.copy_(batch.labels.narrow(0, 0, batch_count), true);
            box_view.copy_(batch.boxes.narrow(0, 0, batch_count), true);
        }
        if (batch.masks.has_value()) {
            const torch_api::Tensor& source_masks = *batch.masks;
            if (source_masks.dim() != 4 || source_masks.size(0) < batch_count || source_masks.size(1) != prediction_count) {
                throw std::runtime_error("predicted masks must be [batch,num_predictions,height,width]");
            }
            const uint32_t mask_height = mmltk::common::math::checked_cast<uint32_t>(source_masks.size(2), "prediction mask height overflow");
            const uint32_t mask_width = mmltk::common::math::checked_cast<uint32_t>(source_masks.size(3), "prediction mask width overflow");
            const bool mask_configuration_matches = buffers.mask && buffers.mask->masks_cpu.defined() && buffers.mask->masks_gpu.defined() &&
                                                    buffers.mask->masks_gpu.device().is_cuda() && buffers.mask->masks_gpu.get_device() == device_id &&
                                                    buffers.mask->mask_height == mask_height && buffers.mask->mask_width == mask_width &&
                                                    buffers.mask->masks_cpu.size(0) >= batch_count && buffers.mask->masks_cpu.size(1) >= prediction_count;
            if (!mask_configuration_matches) {
                if (!buffers.allow_mask_reconfiguration) { throw std::runtime_error("prediction masks do not match the configured staging capacity"); }
                if (!buffers.mask) { buffers.mask.emplace(); }
                buffers.mask->ensure_capacity(batch_count, prediction_count, buffers.bbox.batch_capacity, buffers.bbox.prediction_capacity, mask_height,
                                              mask_width, device_id);
            }
            auto mask_view = buffers.mask->masks_cpu.narrow(0, 0, batch_count).narrow(1, 0, prediction_count);
            auto packed_gpu_view = buffers.mask->masks_gpu.narrow(0, 0, batch_count).narrow(1, 0, prediction_count);
            if (mask_view.stride(1) != buffers.mask->packed_mask_bytes || mask_view.stride(2) != 1 || !packed_gpu_view.is_contiguous()) {
                throw std::logic_error("pinned prediction mask buffer has unexpected strides");
            }
            mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_d2h_masks{"rfdetr.native.eval.d2h_masks"};
            pack_bool_masks_cuda_into(source_masks.narrow(0, 0, batch_count), packed_gpu_view);
            mask_view.copy_(packed_gpu_view, true);
            staged_mask = StagedMaskPredictionBatch{std::move(mask_view), mask_height, mask_width};
        } else if (buffers.mask) {
            throw std::runtime_error("prediction batch omitted masks required by the configured staging slot");
        }
        ensure_cuda_ok(cudaEventRecord(buffers.ready_event, stream), "cudaEventRecord for prediction staging slot");
        buffers.transition(PredictionSlotState::GpuFilling, PredictionSlotState::D2HPending, "prediction D2H submission");
    } catch (...) {
        static_cast<void>(cudaStreamSynchronize(stream));
        throw;
    }
    return StagedPredictionBatch{
        std::move(images),
        StagedBBoxPredictionBatch{score_view, label_view, box_view},
        std::move(staged_mask),
        std::move(batch),
        category_count,
        max_dets_per_image,
        static_cast<size_t>(batch_count),
        std::move(lease),
    };
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
namespace {
void synchronize_staged_prediction_batch(StagedPredictionBatch& staged, EvaluationProfileRecord* profile) {
    if (!staged.lease.buffers || staged.lease.buffers->ready_event == nullptr) {
        throw std::runtime_error("encoded prediction batch is missing its staging event");
    }
    PinnedPredictionBuffers& buffers = *staged.lease.buffers;
    std::lock_guard<std::mutex> consume_lock(buffers.consume_mutex);
    if (buffers.state.load(std::memory_order_acquire) == PredictionSlotState::CpuMatching) { return; }
    CudaGuard device_guard(checked_device_index(buffers.event_device_id));
    const auto started = profile != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ensure_cuda_ok(cudaEventSynchronize(buffers.ready_event), "cudaEventSynchronize for prediction staging slot");
    buffers.transition(PredictionSlotState::D2HPending, PredictionSlotState::CpuMatching, "prediction CPU consumption");
    if (profile != nullptr) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
        profile->d2h_wait_nanoseconds.fetch_add(static_cast<uint64_t>(elapsed.count()), std::memory_order_relaxed);
    }
    staged.pending_gpu = {};
}
PredictionBatchItem encode_staged_prediction_image(StagedPredictionBatch& staged, const size_t image_index, EvaluationProfileRecord* profile,
                                                   const EvaluationDatasetOwner* evaluation_dataset) {
    mmltk::common::logging::ScopedNvtxRange nvtx_encode_staged_prediction_image{"encode_staged_prediction_image", mmltk::common::logging::nvtx_color_orange};
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_consume_staged_image{"rfdetr.native.eval.consume_staged_image"};
    if (image_index >= staged.active_image_count || image_index >= staged.images.size()) {
        throw std::out_of_range("staged prediction image index exceeds the active image count");
    }
    synchronize_staged_prediction_batch(staged, profile);
    PredictionBatchItem result;
    result.dataset_index = staged.images[image_index].dataset_index;
    result.image_id = staged.images[image_index].image_id;
    result.source_name = std::move(staged.images[image_index].source_name);
    if (evaluation_dataset != nullptr) {
        const auto image_offset = static_cast<int64_t>(image_index);
        const BBoxPredictionView bbox_view{
            static_cast<int>(result.image_id),
            staged.bbox.scores_cpu.data_ptr<float>() + image_offset * staged.bbox.scores_cpu.stride(0),
            staged.bbox.labels_cpu.data_ptr<std::int64_t>() + image_offset * staged.bbox.labels_cpu.stride(0),
            staged.bbox.boxes_cpu.data_ptr<float>() + image_offset * staged.bbox.boxes_cpu.stride(0),
            static_cast<size_t>(staged.bbox.scores_cpu.size(1)),
            staged.bbox.scores_cpu.stride(1),
            staged.bbox.labels_cpu.stride(1),
            staged.bbox.boxes_cpu.stride(1),
        };
        std::optional<PackedMaskPredictionView> mask_view;
        if (staged.mask) {
            mask_view = PackedMaskPredictionView{
                staged.mask->masks_cpu.data_ptr<std::uint8_t>() + image_offset * staged.mask->masks_cpu.stride(0),
                staged.mask->masks_cpu.stride(1),
                staged.mask->height,
                staged.mask->width,
            };
        }
        const auto match_started = profile != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        result.evaluation_matches = evaluation_dataset->match_predictions(result.dataset_index, bbox_view, mask_view, staged.max_dets_per_image);
        if (profile != nullptr) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - match_started);
            profile->cpu_match_nanoseconds.fetch_add(static_cast<std::uint64_t>(elapsed.count()), std::memory_order_relaxed);
            profile->iou_candidate_count.fetch_add(result.evaluation_matches->bbox_iou_candidate_count + result.evaluation_matches->mask_iou_candidate_count,
                                                   std::memory_order_relaxed);
            profile->mask_iou_candidate_count.fetch_add(result.evaluation_matches->mask_iou_candidate_count, std::memory_order_relaxed);
            if (mask_view) profile->mask_task_count.fetch_add(1U, std::memory_order_relaxed);
        }
        return result;
    }
    const bool has_masks = staged.mask.has_value();
    const std::uint8_t* mask_values = has_masks ? staged.mask->masks_cpu.data_ptr<std::uint8_t>() : nullptr;
    const int64_t mask_batch_stride = has_masks ? staged.mask->masks_cpu.stride(0) : 0;
    const int64_t mask_prediction_stride = has_masks ? staged.mask->masks_cpu.stride(1) : 0;
    const auto started = profile != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    SelectedPredictions selected =
        select_predictions(static_cast<int>(staged.images[image_index].image_id), staged.bbox.scores_cpu.select(0, static_cast<int64_t>(image_index)),
                           staged.bbox.labels_cpu.select(0, static_cast<int64_t>(image_index)),
                           staged.bbox.boxes_cpu.select(0, static_cast<int64_t>(image_index)), staged.category_count, staged.max_dets_per_image);
    if (has_masks) {
        for (size_t prediction_index = 0; prediction_index < selected.predictions.size(); ++prediction_index) {
            const int64_t source_index = selected.mask_source_indices[prediction_index];
            const std::uint8_t* source = mask_values + static_cast<int64_t>(image_index) * mask_batch_stride + source_index * mask_prediction_stride;
            selected.predictions[prediction_index].mask = encode_mask_from_packed_data(source, staged.mask->height, staged.mask->width);
            selected.predictions[prediction_index].has_mask = true;
        }
    }
    result.predictions = std::move(selected.predictions);
    if (profile != nullptr) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
        profile->cpu_encode_nanoseconds.fetch_add(static_cast<uint64_t>(elapsed.count()), std::memory_order_relaxed);
    }
    return result;
}
}  // namespace
namespace {
std::vector<PredictionBatchItem> settle_prediction_batch_encoding(PendingPredictionBatchEncoding& pending, std::exception_ptr failure = {}) {
    std::vector<PredictionBatchItem> results;
    if (!failure) {
        try {
            results.reserve(pending.images.size());
        } catch (...) { failure = std::current_exception(); }
    }
    for (auto& image : pending.images) {
        // A previous collection may already have consumed this receipt.
        if (!image.valid()) continue;
        try {
            auto result = image.get();
            if (!failure) results.push_back(std::move(result));
        } catch (...) {
            if (!failure) failure = std::current_exception();
        }
    }
    if (failure) std::rethrow_exception(failure);
    return results;
}
}  // namespace
PendingPredictionBatchEncoding enqueue_prediction_batch_encoding(mmltk::common::concurrency::WorkerPool& cpu_pool, StagedPredictionBatch&& staged,
                                                                 EvaluationProfileRecord* profile, const EvaluationDatasetOwner* evaluation_dataset) {
    auto shared_staged = std::make_shared<StagedPredictionBatch>(std::move(staged));
    PendingPredictionBatchEncoding pending;
    pending.images.reserve(shared_staged->active_image_count);
    try {
        for (size_t image_index = 0; image_index < shared_staged->active_image_count; ++image_index) {
            // Capacity is reserved and future movement cannot throw after admission.
            pending.images.push_back(cpu_pool.enqueue([shared_staged, image_index, profile, evaluation_dataset]() mutable {
                // Release staging custody before packaged_task makes its future ready.
                auto staged_owner = std::move(shared_staged);
                return encode_staged_prediction_image(*staged_owner, image_index, profile, evaluation_dataset);
            }));
        }
    } catch (...) {
        // Preserve the admission failure even if an admitted image also failed.
        static_cast<void>(settle_prediction_batch_encoding(pending, std::current_exception()));
        throw;
    }
    return pending;
}
std::vector<PredictionBatchItem> collect_prediction_batch_encoding(PendingPredictionBatchEncoding&& pending) {
    return settle_prediction_batch_encoding(pending);
}
inline int64_t image_id_for_dataset_index(const std::vector<int>& image_ids, int64_t dataset_index) {
    if (dataset_index >= 0 && static_cast<size_t>(dataset_index) < image_ids.size()) { return image_ids[static_cast<size_t>(dataset_index)]; }
    return dataset_index + 1;
}
// Projects a loader batch onto the per-image metadata every prediction and evaluation path consumes.
// Batch slot -> dataset index -> image id is one mapping; it lives here rather than being rebuilt at
// each call site.
std::vector<PredictionBatchMetadata> make_prediction_batch_metadata(const mmltk::backend::data::Batch& batch, const std::vector<int>& image_ids) {
    std::vector<PredictionBatchMetadata> metadata;
    metadata.reserve(batch.num_images);
    for (size_t image_index = 0; image_index < batch.num_images; ++image_index) {
        const auto dataset_index = static_cast<int64_t>(batch.image_indices[image_index]);
        metadata.push_back(PredictionBatchMetadata{
            dataset_index,
            image_id_for_dataset_index(image_ids, dataset_index),
            {},
        });
    }
    return metadata;
}
}  // namespace mmltk::backend::models::rfdetr
