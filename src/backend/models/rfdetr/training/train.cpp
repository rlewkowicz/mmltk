#include "src/backend/models/rfdetr/core/evaluator.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/ml/cuda/numa_host_tensor.h"
#include "src/backend/models/rfdetr/core/detail/matcher_workspace.h"
#include "src/backend/models/rfdetr/training/train.h"
#include "telemetry_writer.h"
#include "detail/training_scalar_packet.h"

#include <cuda_runtime.h>
#include <format>
#include <print>
#include <spdlog/spdlog.h>

#include "archive_utils.h"
#include "detection_ops.h"
#include "detection_types.h"
#include "draw.h"
#include "execution_precision.h"
#include "model_state_access.h"
#include "model_state_technical.h"
#include "model_technical.h"
#include "postprocess.h"
#include "scalar_type_utils.h"
#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/backend/models/rfdetr/core/model_info.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/training/distributed_train_launcher.h"
#include "src/backend/models/rfdetr/training/train_recipe.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/system/execution_policy.h"
#include "src/common/io/json_file.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/cuda_priority.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include "torch_api.h"
#include "torch_cuda_utils.h"
#include "detail/checkpoint_private.h"
#if defined(USE_C10D_NCCL)
#include <torch/csrc/distributed/c10d/FileStore.hpp>
#include <torch/csrc/distributed/c10d/ProcessGroupNCCL.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <format>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "spdmon/spdmon.hpp"

// CLEANUP-IGNORE: Training directly imports its own pipeline dependencies; shared module names are not duplicated
// logic.
import mmltk.backend.models.rfdetr.training.checkpoint;
import mmltk.backend.models.rfdetr.core.artifact_resolution;
import mmltk.backend.models.rfdetr.core.dataset_limit_resolution; // CLEANUP-IGNORE: Training directly imports its dataset policy owner.
import mmltk.backend.models.rfdetr.core.runtime;
import mmltk.backend.models.rfdetr.core.tool_launch_utils;
import mmltk.backend.models.rfdetr.core.model;

import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
import mmltk.backend.ml.cuda.gpu_quiescence;
import mmltk.backend.ml.cuda.shared_cuda_event;

#include "detail/gpu_augment_private.h"
#include "detail/native_optimizer_private.h"
#include "detail/target_builder_private.h"
#include "detail/training_ops_private.h"
#include "model_access.h"
#include "model_state_access.h"

#include "detail/evaluation_runtime.h"

namespace mmltk::backend::models::rfdetr {

namespace torch_api = mmltk::backend::ml::torch_api;
namespace torch_cuda = mmltk::backend::ml::cuda;

using mmltk::common::math::checked_cast;

using mmltk::frameworks::gpu::ensure_cuda_ok;
namespace {

std::size_t prediction_lane_slot_count(const RuntimeSplit& split, const std::size_t batch_size) {
    const auto lanes = static_cast<std::size_t>(std::max(1, split.lane_threads));
    const auto cpus = static_cast<std::size_t>(std::max(1, split.cpu_threads));
    const auto wave = lanes * std::max<std::size_t>(1U, batch_size);
    return std::max<std::size_t>(2U, 1U + (cpus + wave - 1U) / wave);
}

std::size_t prediction_cpu_batch_limit(const RuntimeSplit& split, const std::size_t batch_size) {
    const auto cpus = static_cast<std::size_t>(std::max(1, split.cpu_threads));
    const auto images = std::max<std::size_t>(1U, batch_size);
    return std::max<std::size_t>(2U, (cpus + images - 1U) / images);
}

PhaseTiming elapsed_timing(const std::chrono::steady_clock::time_point start, const std::size_t images) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return {
        seconds,
        seconds > 0.0 ? static_cast<double>(images) / seconds : 0.0,
        images,
    };
}

struct OptimizerBuildResult {
    NativeOptimizer optimizer;
    std::vector<double> base_lrs;
};

struct EvalPassResult {
    std::optional<double> loss;
    EvalSummary summary;
    PhaseTiming timing;
};

struct CapturedEvalSample {
    torch_api::Tensor image;
    torch_api::Tensor boxes;
    torch_api::Tensor labels;
    torch_api::Tensor masks;
};

struct ResumeState {
    std::string attempt_id;
    int start_epoch = 0;
    double best_regular = -std::numeric_limits<double>::infinity();
    double best_ema = -std::numeric_limits<double>::infinity();
    std::optional<ModelEma> restored_ema;
    std::optional<NativeOptimizer> optimizer_candidate;
    std::optional<float> scaler_scale;
    std::optional<int> scaler_growth_tracker;
};

int rfdetr_output_class_count(uint32_t dataset_class_count) { return static_cast<int>(dataset_class_count) + 1; }

int checked_inference_batch_size(size_t batch_size) {
    const size_t effective_batch_size = std::max<size_t>(1, batch_size);
    if (effective_batch_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("batch_size exceeds supported inference compilation range");
    }
    return static_cast<int>(effective_batch_size);
}

std::string phase_progress_label(const char* phase, int epoch, int total_epochs) {
    return std::format("{} {}/{}", phase, epoch + 1, total_epochs);
}

OutputLayer narrow_output_layer_batch(const OutputLayer& layer, int64_t count) {
    OutputLayer narrowed;
    narrowed.pred_logits = layer.pred_logits.narrow(0, 0, count);
    narrowed.pred_boxes = layer.pred_boxes.narrow(0, 0, count);
    if (layer.query_features.has_value()) { narrowed.query_features = layer.query_features->narrow(0, 0, count); }
    narrowed.query_layout = layer.query_layout;
    if (layer.pred_masks.has_value()) { narrowed.pred_masks = layer.pred_masks->narrow(0, 0, count); }
    if (layer.sparse_pred_masks.has_value()) {
        narrowed.sparse_pred_masks = OutputLayer::SparsePredMasks{
            layer.sparse_pred_masks->spatial_features.narrow(0, 0, count),
            layer.sparse_pred_masks->query_features.narrow(0, 0, count),
            layer.sparse_pred_masks->bias,
        };
    }
    return narrowed;
}

ModelOutputs narrow_model_outputs_batch(const ModelOutputs& outputs, int64_t count) {
    ModelOutputs narrowed;
    narrowed.main = narrow_output_layer_batch(outputs.main, count);
    narrowed.aux_outputs.reserve(outputs.aux_outputs.size());
    for (const auto& layer : outputs.aux_outputs) {
        narrowed.aux_outputs.push_back(narrow_output_layer_batch(layer, count));
    }
    if (outputs.enc_outputs.has_value()) { narrowed.enc_outputs = narrow_output_layer_batch(*outputs.enc_outputs, count); }
    return narrowed;
}

class TrainingValidationRuntime {
   public:
    TrainingValidationRuntime(const TrainRequest& options, RuntimeContext& runtime,
                              std::unique_ptr<mmltk::backend::data::DatasetLoader> loader, size_t batch_size, bool enable_loss,
                              EvaluationMetricSet metric_set, const int64_t prediction_capacity, std::string split_name,
                              const bool query_count_automatic)
        : runtime_(runtime),
          loader_(std::move(loader)),
          batch_size_(std::max<size_t>(1, batch_size)),
          target_scratch_(enable_loss ? std::make_unique<TargetScratch>() : nullptr),
          lane_pool_(static_cast<size_t>(runtime.split().lane_threads), runtime.lane_cpus(), "rfdtrnlane", 0U,
                     &runtime.execution().placement, false),
          split_name_(std::move(split_name)),
          query_count_automatic_(query_count_automatic) {
        if (!loader_) { throw std::invalid_argument("training validation runtime requires a dataset loader"); }
        detection_limit_ = resolve_dataset_limit(loader_->max_instances_per_image(), options.eval_max_dets);

        torch_cuda::TorchCudaDeviceGuard device_guard(torch_cuda::checked_device_index(options.device_id));
        amp_enabled_ = options.amp;
        inference_dtype_ = amp_enabled_ ? resolve_cuda_autocast_dtype() : torch_api::kFloat;
        const auto batch_capacity = static_cast<int64_t>(batch_size_);
        batch_tensors_.ensure(batch_capacity, static_cast<int>(loader_->image_height()), static_cast<int>(loader_->image_width()),
                              options.device_id);
        preprocessor_ =
            std::make_unique<GpuBatchPreprocessor>(batch_capacity, static_cast<int>(loader_->image_height()),
                                                   static_cast<int>(loader_->image_width()), options.device_id, inference_dtype_);

        const size_t slot_count = prediction_lane_slot_count(runtime.split(), batch_size_);
        const size_t encoding_capacity = prediction_cpu_batch_limit(runtime.split(), batch_size_);
        evaluation_run_ = std::make_unique<TrainingEvaluationRunOwner>(EvaluationRunConfig{
            metric_set,
            batch_size_,
            static_cast<size_t>(prediction_capacity),
            static_cast<size_t>(runtime.split().lane_threads),
            slot_count,
            encoding_capacity,
            checked_cast<std::uint32_t>(loader_->image_height(), "RF-DETR image height exceeds uint32_t"),
            checked_cast<std::uint32_t>(loader_->image_width(), "RF-DETR image width exceeds uint32_t"),
            options.device_id,
            options.validation_profile,
        });
        evaluation_run_->load_dataset(*loader_);
        image_ids_ = evaluation_run_->image_ids();
    }

    void begin_pass() {
        evaluation_run_->begin();
        loader_->begin_epoch();
    }

    torch_api::Tensor preprocess(const mmltk::backend::data::Batch& batch) {
        return preprocessor_->run(batch, static_cast<int64_t>(batch_size_));
    }
    void record_preprocess_consumer(cudaStream_t stream) { preprocessor_->record_consumer(stream); }

    RuntimeContext& runtime() { return runtime_; }
    size_t batch_size() const { return batch_size_; }
    mmltk::backend::data::DatasetLoader& loader() { return *loader_; }
    const std::vector<int>& image_ids() const { return image_ids_; }
    bool amp_enabled() const noexcept { return amp_enabled_; }
    torch_api::ScalarType inference_dtype() const noexcept { return inference_dtype_; }
    torch_api::Tensor nested_mask() const { return batch_tensors_.nested_mask_view(static_cast<int64_t>(batch_size_)); }
    TargetScratch& target_scratch() {
        if (!target_scratch_) { throw std::logic_error("validation target scratch requested while validation loss is disabled"); }
        return *target_scratch_;
    }
    mmltk::common::concurrency::WorkerPool& lane_pool() { return lane_pool_; }
    TrainingEvaluationRunOwner& evaluation_run() noexcept { return *evaluation_run_; }
    std::string_view split_name() const noexcept { return split_name_; }
    const ResolvedDatasetLimit& detection_limit() const noexcept { return detection_limit_; }
    bool query_count_automatic() const noexcept { return query_count_automatic_; }

   private:
    RuntimeContext& runtime_;
    std::unique_ptr<mmltk::backend::data::DatasetLoader> loader_;
    size_t batch_size_ = 1;
    std::unique_ptr<TrainingEvaluationRunOwner> evaluation_run_;
    std::vector<int> image_ids_;
    BatchStaticTensors batch_tensors_;
    std::unique_ptr<GpuBatchPreprocessor> preprocessor_;
    std::unique_ptr<TargetScratch> target_scratch_;
    mmltk::common::concurrency::WorkerPool lane_pool_;
    std::string split_name_;
    ResolvedDatasetLimit detection_limit_;
    torch_api::ScalarType inference_dtype_ = torch_api::kFloat;
    bool query_count_automatic_ = false;
    bool amp_enabled_ = false;
};

struct TrainLaneContext {
    TrainLaneContext(torch_cuda::TorchCudaStream lane_stream, const std::size_t staging_depth)
        : stream(std::move(lane_stream)), target_scratch(staging_depth) {}

    torch_cuda::TorchCudaStream stream;
    std::unique_ptr<GpuBatchAugmenter> augmenter;
    TargetScratch target_scratch;
    std::shared_ptr<NativeRfDetrModel> model;
    std::vector<torch_api::Tensor> grad_params;
    size_t synced_parameter_version = std::numeric_limits<size_t>::max();
};

struct TrainLaneResult {
    torch_api::Tensor loss;
    torch_api::Tensor class_loss;
    torch_api::Tensor box_loss;
    scalar_packet::Tensors scalars;
    std::vector<torch_api::Tensor> gradients;
    std::optional<mmltk::backend::ml::cuda::CudaEventPool::Lease> ready_event;
};

enum class TrainingSupervisionRoute : std::uint8_t {
    Hungarian,
    MatchFree,
    HungarianDenoising,
    MatchFreeDenoising,
};

[[nodiscard]] TrainingSupervisionRoute supervision_route(const TrainingSupervisionConfig& config) noexcept {
    const bool match_free = config.assignment == TrainAssignmentKind::MatchFree;
    const bool denoising = config.denoising.enabled;
    if (match_free && denoising) return TrainingSupervisionRoute::MatchFreeDenoising;
    if (match_free) return TrainingSupervisionRoute::MatchFree;
    if (denoising) return TrainingSupervisionRoute::HungarianDenoising;
    return TrainingSupervisionRoute::Hungarian;
}

[[nodiscard]] bool route_is_active(const TrainingSupervisionRoute route) noexcept { return route != TrainingSupervisionRoute::Hungarian; }

[[nodiscard]] bool route_uses_match_free(const TrainingSupervisionRoute route) noexcept {
    return route == TrainingSupervisionRoute::MatchFree || route == TrainingSupervisionRoute::MatchFreeDenoising;
}

[[nodiscard]] bool route_uses_denoising(const TrainingSupervisionRoute route) noexcept {
    return route == TrainingSupervisionRoute::HungarianDenoising || route == TrainingSupervisionRoute::MatchFreeDenoising;
}

class SupervisionTimingLease final {
   public:
    enum class Kind : std::uint8_t { Step, Criterion };

    SupervisionTimingLease(detail::NativeModelTechnicalOwner& owner, const bool active, const Kind kind)
        : owner_(active ? &owner : nullptr), kind_(kind) {
        if (owner_ != nullptr) { begin(); }
    }

    ~SupervisionTimingLease() noexcept {
        if (owner_ != nullptr) {
            try {
                end();
            } catch (...) {}
        }
    }

    SupervisionTimingLease(const SupervisionTimingLease&) = delete;
    SupervisionTimingLease& operator=(const SupervisionTimingLease&) = delete;

    void finish() {
        if (owner_ != nullptr) {
            end();
            owner_ = nullptr;
        }
    }

   private:
    void begin() {
        if (kind_ == Kind::Step) {
            owner_->begin_supervised_step_timing();
        } else {
            owner_->begin_criterion_timing();
        }
    }

    void end() {
        if (kind_ == Kind::Step) {
            owner_->end_supervised_step_timing();
        } else {
            owner_->end_criterion_timing();
        }
    }

    detail::NativeModelTechnicalOwner* owner_;
    Kind kind_;
};

int64_t prepared_target_count(const PreparedTargets& targets) {
    return std::accumulate(targets.counts.begin(), targets.counts.end(), int64_t{0});
}

class TrainingEventOwner final {
   public:
    TrainingEventOwner(const int device, const std::size_t capacity)
        : device_(device),
          retirement_owner_(1U),
          event_pool_(mmltk::frameworks::gpu::make_cuda_device_owner<TrainingEventOwner, &TrainingEventOwner::record_failure>(this, device),
                      capacity, retirement_owner_) {}

    void record_failure(const cudaError_t failure) noexcept { mmltk::frameworks::gpu::record_first_cuda_failure(first_failure_, failure); }

    [[nodiscard]] mmltk::backend::ml::cuda::CudaEventPool& pool() noexcept { return event_pool_; }

   private:
    int device_ = -1;
    std::atomic<cudaError_t> first_failure_{cudaSuccess};
    mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement_owner_;
    mmltk::backend::ml::cuda::CudaEventPool event_pool_;
};

struct TrainingMetricSnapshot {
    double loss_sum = 0.0;
    double class_loss_sum = 0.0;
    double box_loss_sum = 0.0;
    double step_loss = 0.0;
    double step_class_loss = 0.0;
    double step_box_loss = 0.0;
    TrainingScalars scalars;
    bool loss_finite = true;
    bool gradients_finite = true;
};

class TrainingMetricHandoff {
   public:
    explicit TrainingMetricHandoff(int device_id)
        : device_id_(device_id),
          event_pool_(mmltk::frameworks::gpu::make_cuda_device_owner<TrainingMetricHandoff, &TrainingMetricHandoff::record_failure>(
                          this, device_id),
                      1U, retirement_owner_) {
        const auto device_options = torch_api::TensorOptions().dtype(torch_api::kFloat32).device(cuda_device(device_id_));
        device_values_ = torch_api::zeros({10 + static_cast<int64_t>(scalar_packet::size)}, device_options);
        device_values_.select(0, 6).fill_(1.0f);
        scalar_values_ = torch_api::empty({static_cast<int64_t>(scalar_packet::size)}, device_options);
        unavailable_ = torch_api::full({}, std::numeric_limits<float>::quiet_NaN(), device_options);
        host_values_ = torch_cuda::numa_empty({10 + static_cast<int64_t>(scalar_packet::size)}, torch_api::kFloat32, device_id_);
        torch_cuda::TorchCudaDeviceGuard device_guard(torch_cuda::checked_device_index(device_id_));
        ensure_cuda_ok(cudaStreamCreateWithFlags(&settlement_stream_, cudaStreamNonBlocking), "create training metric settlement stream");
    }

    ~TrainingMetricHandoff() noexcept {
        if (settlement_stream_ != nullptr) {
            mmltk::frameworks::gpu::CudaDeviceScope scope(device_id_);
            const cudaError_t status = scope ? cudaStreamDestroy(settlement_stream_) : scope.status();
            static_cast<void>(scope.FinalizeStatus(status));
        }
    }

    TrainingMetricHandoff(const TrainingMetricHandoff&) = delete;
    TrainingMetricHandoff& operator=(const TrainingMetricHandoff&) = delete;

    void record_failure(const cudaError_t failure) noexcept { mmltk::frameworks::gpu::record_first_cuda_failure(first_failure_, failure); }

    void reset_epoch() {
        device_values_.zero_();
        device_values_.select(0, 6).fill_(1.0f);
    }

    void begin_wave() { device_values_.narrow(0, 3, 3).zero_(); }

    void accumulate(const torch_api::Tensor& loss, const torch_api::Tensor& class_loss, const torch_api::Tensor& box_loss, const scalar_packet::Tensors& scalars) {
        for (std::size_t i = 0; i < scalar_packet::size; ++i)
            scalar_sources_[i] = scalars[i].defined() ? scalars[i] : unavailable_;
        at::stack_out(scalar_values_, scalar_sources_);
        device_values_.narrow(0, 10, scalar_packet::size).add_(scalar_values_);
        const auto detached_loss = loss.detach();
        const auto detached_class_loss = class_loss.detach();
        const auto detached_box_loss = box_loss.detach();
        device_values_.select(0, 0).add_(detached_loss);
        device_values_.select(0, 1).add_(detached_class_loss);
        device_values_.select(0, 2).add_(detached_box_loss);
        device_values_.select(0, 3).add_(detached_loss);
        device_values_.select(0, 4).add_(detached_class_loss);
        device_values_.select(0, 5).add_(detached_box_loss);
        device_values_.select(0, 6).mul_(torch_api::isfinite(detached_loss).to(torch_api::kFloat32));
    }

    TrainingMetricSnapshot complete_step(const torch_api::Tensor& found_inf, int64_t wave_micro_batches, int64_t epoch_micro_batches) {
        device_values_.select(0, 7).copy_(found_inf);
        const auto* values = complete_values();
        const double wave_divisor = static_cast<double>(std::max<int64_t>(1, wave_micro_batches));
        TrainingMetricSnapshot snapshot;
        snapshot.scalars = scalar_packet::project(values + 10, static_cast<double>(epoch_micro_batches));
        snapshot.loss_sum = static_cast<double>(values[0]);
        snapshot.class_loss_sum = static_cast<double>(values[1]);
        snapshot.box_loss_sum = static_cast<double>(values[2]);
        snapshot.step_loss = static_cast<double>(values[3]) / wave_divisor;
        snapshot.step_class_loss = static_cast<double>(values[4]) / wave_divisor;
        snapshot.step_box_loss = static_cast<double>(values[5]) / wave_divisor;
        snapshot.loss_finite = values[6] != 0.0f;
        snapshot.gradients_finite = values[7] == 0.0f;
        device_values_.select(0, 6).fill_(1.0f);
        device_values_.select(0, 7).zero_();
        return snapshot;
    }

    [[nodiscard]] torch_api::Tensor loss_sum() const { return device_values_.select(0, 0); }

    [[nodiscard]] torch_api::Tensor epoch_count(std::int64_t count) {
        auto value = device_values_.select(0, 9);
        value.fill_(static_cast<float>(count));
        return value;
    }
    [[nodiscard]] double epoch_average() {
        const auto* values = complete_values();
        return static_cast<double>(values[0]) / std::max(1.0, static_cast<double>(values[9]));
    }
    void begin_validation() { device_values_.select(0, 8).zero_(); }
    void accumulate_validation(const torch_api::Tensor& loss) { device_values_.select(0, 8).add_(loss.detach()); }
    [[nodiscard]] double validation_average(std::size_t count) {
        const auto* values = complete_values();
        return count ? static_cast<double>(values[8]) / static_cast<double>(count) : 0.0;
    }

   private:
    const float* complete_values() {
        host_values_.copy_(device_values_, true);
        const auto device_index = torch_cuda::checked_device_index(device_id_);
        const auto stream = torch_cuda::current_torch_cuda_stream_object(device_index);
        auto completion = event_pool_.record(reinterpret_cast<std::uintptr_t>(stream.stream()), "record training metric handoff");
        if (!completion) { throw std::runtime_error("training metric event pool is unavailable"); }
        completion->wait(reinterpret_cast<std::uintptr_t>(settlement_stream_), "wait for training metric handoff");
        ensure_cuda_ok(cudaStreamSynchronize(settlement_stream_), "synchronize training metric settlement stream");
        completion->retire();

        return host_values_.data_ptr<float>();
    }
    int device_id_ = 0;
    torch_api::Tensor device_values_;
    torch_api::Tensor host_values_;
    torch_api::Tensor scalar_values_;
    torch_api::Tensor unavailable_;
    scalar_packet::Tensors scalar_sources_;
    std::atomic<cudaError_t> first_failure_{cudaSuccess};
    mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement_owner_{1U};
    mmltk::backend::ml::cuda::CudaEventPool event_pool_;
    cudaStream_t settlement_stream_ = nullptr;
};

int effective_train_lanes(const TrainRequest& options) {
    if (options.lanes < 0) { throw std::runtime_error("RF-DETR train --lanes must be non-negative"); }
    return std::max(1, options.lanes);
}

size_t micro_batches_per_optimizer_step(const TrainRequest& options, int train_lane_count) {
    return static_cast<size_t>(std::max(1, options.grad_accum_steps)) * static_cast<size_t>(std::max(1, train_lane_count));
}

size_t effective_batch_per_rank(const TrainRequest& options, int train_lane_count) {
    return static_cast<size_t>(options.batch_size) * micro_batches_per_optimizer_step(options, train_lane_count);
}

size_t effective_batch_global(const TrainRequest& options, const DistributedContext& distributed, int train_lane_count) {
    return effective_batch_per_rank(options, train_lane_count) * static_cast<size_t>(std::max(1, distributed.world_size));
}

std::optional<mmltk::backend::ml::cuda::CudaEventPool::Lease> record_current_stream_event(
    mmltk::backend::ml::cuda::CudaEventPool& event_pool, const int device_id, const char* context) {
    return event_pool.record(reinterpret_cast<std::uintptr_t>(
                                 torch_cuda::current_torch_cuda_stream_object(torch_cuda::checked_device_index(device_id)).stream()),
                             context);
}

void wait_for_lane_result(TrainLaneResult& lane_result, int device_id) {
    if (!lane_result.ready_event) { throw std::runtime_error("parallel RF-DETR train result is missing a completion event"); }
    lane_result.ready_event->wait(reinterpret_cast<std::uintptr_t>(
                                      torch_cuda::current_torch_cuda_stream_object(torch_cuda::checked_device_index(device_id)).stream()),
                                  "wait for parallel train lane result");
    lane_result.ready_event->retire();
}

void record_lane_result_on_current_stream(const TrainLaneResult& lane_result, int device_id) {
    const auto stream = torch_cuda::current_torch_cuda_stream_object(torch_cuda::checked_device_index(device_id));
    const auto record_tensor = [&stream](const torch_api::Tensor& value) {
        if (value.defined() && value.device().is_cuda()) { value.record_stream(stream); }
    };
    record_tensor(lane_result.loss);
    record_tensor(lane_result.class_loss);
    record_tensor(lane_result.box_loss);
    for (const auto& scalar : lane_result.scalars) record_tensor(scalar);
    for (const auto& gradient : lane_result.gradients) {
        record_tensor(gradient);
    }
}

void merge_lane_gradients(TrainLaneResult& lane_result, std::vector<torch_api::Tensor>& parameters, int device_id) {
    wait_for_lane_result(lane_result, device_id);
    record_lane_result_on_current_stream(lane_result, device_id);

    torch_api::NoGradGuard no_grad;
    const size_t limit = std::min(parameters.size(), lane_result.gradients.size());
    for (size_t index = 0; index < limit; ++index) {
        const auto& gradient = lane_result.gradients[index];
        if (!gradient.defined()) { continue; }
        if (!parameters[index].grad().defined()) {
            parameters[index].mutable_grad() = gradient.detach().clone();
        } else {
            parameters[index].mutable_grad().add_(gradient);
        }
    }
}

void copy_module_state(torch_api::Module& destination, const torch_api::Module& source) {
    torch_api::NoGradGuard no_grad;

    auto destination_parameters = destination.named_parameters(true);
    for (const auto& item : source.named_parameters(true)) {
        auto* target = destination_parameters.find(item.key());
        if (target == nullptr) { throw std::runtime_error("parallel RF-DETR lane is missing parameter: " + item.key()); }
        target->copy_(item.value());
        target->requires_grad_(item.value().requires_grad());
    }

    auto destination_buffers = destination.named_buffers(true);
    for (const auto& item : source.named_buffers(true)) {
        auto* target = destination_buffers.find(item.key());
        if (target == nullptr) { throw std::runtime_error("parallel RF-DETR lane is missing buffer: " + item.key()); }
        target->copy_(item.value());
    }
}

std::shared_ptr<NativeRfDetrModel> make_train_lane_model(NativeRfDetrModel& model, int device_id) {
    auto lane_model = std::make_shared<NativeRfDetrModel>(model.config(), model.class_layout()->record());
    auto& lane_module = detail::native_model_owner(*lane_model).module();
    lane_module.to(cuda_device(device_id));
    lane_module.train();
    copy_module_state(lane_module, detail::native_model_owner(model).module());
    detail::native_model_owner(*lane_model).replicate_training_supervision_runtime_from(detail::native_model_owner(model));
    return lane_model;
}

std::vector<torch_api::Tensor> lane_grad_parameters(const NativeRfDetrModel& lane_model, const std::vector<std::string>& parameter_names) {
    const auto named_parameters = detail::native_model_owner(lane_model).module().named_parameters(true);
    std::vector<torch_api::Tensor> parameters;
    parameters.reserve(parameter_names.size());
    for (const auto& name : parameter_names) {
        auto* tensor = named_parameters.find(name);
        if (tensor == nullptr) { throw std::runtime_error("parallel RF-DETR lane is missing trainable parameter: " + name); }
        parameters.push_back(*tensor);
    }
    return parameters;
}

std::optional<std::string> first_running_stats_buffer_name(NativeRfDetrModel& model) {
    for (const auto& item : detail::native_model_owner(model).module().named_buffers(true)) {
        if (item.key().find("running_mean") != std::string::npos || item.key().find("running_var") != std::string::npos ||
            item.key().find("num_batches_tracked") != std::string::npos) {
            return item.key();
        }
    }
    return std::nullopt;
}

void ensure_train_lane_model_supported(NativeRfDetrModel& model, int train_lane_count) {
    if (train_lane_count <= 1) { return; }
    const auto running_stats = first_running_stats_buffer_name(model);
    if (running_stats.has_value()) {
        throw std::runtime_error("parallel RF-DETR train --lanes requires a model without running-stat buffers; found " + *running_stats);
    }
}

bool is_rank_zero(const DistributedContext& distributed) { return distributed.rank == 0; }

std::string train_progress_postfix(double average_class_loss, double average_box_loss, double average_loss, double step_class_loss,
                                   double step_box_loss, double step_loss, double images_per_second, int64_t optimizer_steps,
                                   int64_t steps_per_epoch) {
    return std::format("cl={:.4f}, bl={:.4f}, l={:.4f}, scl={:.4f}, sbl={:.4f}, sl={:.4f}, img/s={:.2f}, step={}/{}", average_class_loss,
                       average_box_loss, average_loss, step_class_loss, step_box_loss, step_loss, images_per_second, optimizer_steps,
                       steps_per_epoch);
}

torch_api::Tensor loss_value_or_zero(const TensorMap& loss_dict, const torch_api::Device& device, std::string_view key) {
    const auto found = loss_dict.find(std::string(key));
    if (found != loss_dict.end()) { return found->second; }
    return torch_api::zeros({}, torch_api::TensorOptions().dtype(torch_api::kFloat32).device(device));
}

scalar_packet::Tensors ordinary_scalar_tensors(const TensorMap& losses, const torch_api::Tensor& total,
                                                const torch_api::Tensor& auxiliary) {
    scalar_packet::Tensors values;
    scalar_packet::set<^^TrainingScalars::total>(values, total);
    scalar_packet::set<^^TrainingScalars::auxiliary_weighted>(values, auxiliary);
    const auto assign = [&]<std::meta::info Member>(std::string_view name) {
        const auto found = losses.find(std::string(name));
        if (found != losses.end()) scalar_packet::set<Member>(values, found->second);
    };
    assign.operator()<^^TrainingScalars::classification>("loss_ce");
    assign.operator()<^^TrainingScalars::l1>("loss_bbox");
    assign.operator()<^^TrainingScalars::giou>("loss_giou");
    assign.operator()<^^TrainingScalars::mask_ce>("loss_mask_ce");
    assign.operator()<^^TrainingScalars::mask_dice>("loss_mask_dice");
    assign.operator()<^^TrainingScalars::class_error>("class_error");
    assign.operator()<^^TrainingScalars::cardinality_error>("cardinality_error");
    return values;
}

struct RoutedTrainingLoss {
    torch_api::Tensor total;
    torch_api::Tensor classification;
    torch_api::Tensor box;
    TensorMap ordinary_terms;
    scalar_packet::Tensors scalars;
};

RoutedTrainingLoss compute_routed_training_loss(NativeRfDetrModel& model, const TrainingSupervisionRoute route, const ModelOutputs& outputs,
                                                const PreparedTargets& targets, const DeviceLossNormalizer& normalizer,
                                                const DetectionConfig& detection_config) {
    if (route_uses_match_free(route)) {
        const auto loss = detail::native_model_owner(model).supervision_loss(outputs, targets, normalizer, true);
        scalar_packet::Tensors scalars;
        scalar_packet::set<^^TrainingScalars::total>(scalars, loss.total);
        scalar_packet::set<^^TrainingScalars::classification>(scalars, loss.main.classification);
        scalar_packet::set<^^TrainingScalars::l1>(scalars, loss.main.box);
        scalar_packet::set<^^TrainingScalars::giou>(scalars, loss.main.giou);
        scalar_packet::set<^^TrainingScalars::correspondence_weighted>(scalars, loss.correspondence);
        scalar_packet::set<^^TrainingScalars::auxiliary_weighted>(scalars, loss.auxiliary);
        if (route_uses_denoising(route)) scalar_packet::set<^^TrainingScalars::denoising_weighted>(scalars, loss.denoising);
        return {loss.total, loss.classification, loss.box + loss.giou, {}, std::move(scalars)};
    }

    const double group_divisor = detection_config.sum_group_losses ? 1.0 : static_cast<double>(detection_config.group_detr);
    const double num_boxes = torch_api::clamp_min(normalizer.target_count * group_divisor, 1.0).item<double>();
    auto ordinary_terms = detection_loss_dict(outputs, targets, detection_config, true, num_boxes);
    torch_api::Tensor auxiliary;
    auto total = weighted_detection_loss(ordinary_terms, detection_config, outputs.main.pred_logits.device(), &auxiliary);
    auto scalars = ordinary_scalar_tensors(ordinary_terms, total, auxiliary);
    auto classification = loss_value_or_zero(ordinary_terms, outputs.main.pred_logits.device(), "loss_ce");
    auto box = loss_value_or_zero(ordinary_terms, outputs.main.pred_logits.device(), "loss_bbox");
    if (route_uses_denoising(route)) {
        const auto denoising = detail::native_model_owner(model).supervision_loss(outputs, targets, normalizer, true);
        total = total + denoising.total;
        scalar_packet::set<^^TrainingScalars::denoising_weighted>(scalars, denoising.total);
        classification = classification + denoising.classification;
        box = box + denoising.box + denoising.giou;
    }
    scalar_packet::set<^^TrainingScalars::total>(scalars, total);
    return {std::move(total), std::move(classification), std::move(box), std::move(ordinary_terms), std::move(scalars)};
}

std::string format_nonfinite_loss_report(const TensorMap& loss_dict, const std::vector<torch_api::Tensor>& parameters,
                                         const std::vector<std::string>& parameter_names) {
    std::ostringstream report;
    bool wrote_loss = false;
    for (const auto& [name, value] : loss_dict) {
        if (!torch_api::isfinite(value).item<bool>()) {
            if (!wrote_loss) {
                report << " nonfinite_losses=[";
                wrote_loss = true;
            } else {
                report << ", ";
            }
            report << name << "=" << value.item<double>();
        }
    }
    if (wrote_loss) { report << "]"; }

    for (size_t index = 0; index < parameters.size(); ++index) {
        const auto& param = parameters[index];
        if (param.defined() && !torch_api::isfinite(param).all().item<bool>()) {
            report << " nonfinite_param=" << parameter_names[index];
            break;
        }
        if (param.grad().defined() && !torch_api::isfinite(param.grad()).all().item<bool>()) {
            report << " nonfinite_grad=" << parameter_names[index];
            break;
        }
    }

    return report.str();
}

double checkpoint_metric(const EvalSummary& summary, const bool include_masks) {
    if (!include_masks) { return summary.bbox.ap; }
    if (!summary.mask.has_value()) { throw std::logic_error("segmentation validation did not produce a mask metric"); }
    return summary.mask->ap;
}

std::string formatted_mask_ap(const EvalSummary& summary) {
    return summary.mask.has_value() ? std::format("{:.4f}", summary.mask->ap) : "null";
}

DistributedContext make_distributed_context(const TrainRequest& options) {
    DistributedContext distributed;
    if (!options.distributed_worker || options.distributed_world_size <= 1) { return distributed; }
    if (options.distributed_store_path.empty()) { throw std::runtime_error("distributed RF-DETR worker requires --dist-store-file"); }
    if (options.distributed_rank < 0 || options.distributed_rank >= options.distributed_world_size) {
        throw std::runtime_error("distributed RF-DETR worker rank is out of range");
    }

#if !defined(USE_C10D_NCCL)
    throw std::runtime_error("distributed RF-DETR training requires a LibTorch build with NCCL/c10d enabled");
#else
    distributed.enabled = true;
    distributed.rank = options.distributed_rank;
    distributed.world_size = options.distributed_world_size;
    torch_cuda::TorchCudaDeviceGuard device_guard(cuda_device_index(options.device_id));
    distributed.store = c10::make_intrusive<c10d::FileStore>(options.distributed_store_path.string(), options.distributed_world_size);

    auto pg_options = c10::make_intrusive<c10d::ProcessGroupNCCL::Options>();
    pg_options->timeout = c10d::kProcessGroupNCCLDefaultTimeout;
    distributed.process_group =
        c10::make_intrusive<c10d::ProcessGroupNCCL>(distributed.store, distributed.rank, distributed.world_size, std::move(pg_options));
    return distributed;
#endif
}

void distributed_barrier(const DistributedContext& distributed) {
    if (!distributed.enabled) { return; }
#if defined(USE_C10D_NCCL)
    distributed.process_group->barrier()->wait();
#else
    (void)distributed;
    throw std::runtime_error("distributed RF-DETR training requires a LibTorch build with NCCL/c10d enabled");
#endif
}

void distributed_all_reduce_coalesced(const DistributedContext& distributed, std::vector<torch_api::Tensor>& tensors) {
    if (!distributed.enabled || tensors.empty()) { return; }
#if defined(USE_C10D_NCCL)
    distributed.process_group->allreduce_coalesced(tensors)->wait();
#else
    (void)distributed;
    (void)tensors;
    throw std::runtime_error("distributed RF-DETR training requires a LibTorch build with NCCL/c10d enabled");
#endif
}

bool is_encoder_param(std::string_view name) { return name.find("backbone.0.encoder") != std::string_view::npos; }

bool encoder_zero_weight_decay(std::string_view name) {
    return name.find("gamma") != std::string_view::npos || name.find("bias") != std::string_view::npos ||
           name.find("norm") != std::string_view::npos || name.find("embeddings") != std::string_view::npos ||
           name.find("position_embeddings") != std::string_view::npos || name.find("rel_pos") != std::string_view::npos;
}

int encoder_layer_id(std::string_view name) {
    constexpr int kNumVitLayers = 13;
    if (name.find("embeddings") != std::string_view::npos) { return 0; }
    const auto layer_pos = name.find(".layer.");
    if (layer_pos != std::string_view::npos && name.find(".residual.") == std::string_view::npos) {
        const auto digits_pos = layer_pos + std::string_view(".layer.").size();
        size_t digits_len = 0;
        while (digits_pos + digits_len < name.size() && std::isdigit(static_cast<unsigned char>(name[digits_pos + digits_len]))) {
            ++digits_len;
        }
        if (digits_len > 0) { return std::stoi(std::string(name.substr(digits_pos, digits_len))) + 1; }
    }
    return kNumVitLayers + 1;
}

double parameter_lr(std::string_view name, const TrainRequest& options) {
    if (is_encoder_param(name)) {
        constexpr int kNumVitLayers = 13;
        const int layer_id = encoder_layer_id(name);
        return options.lr_encoder * std::pow(options.encoder_layer_decay, static_cast<double>(kNumVitLayers + 1 - layer_id)) *
               options.lr_component_decay * options.lr_component_decay;
    }
    if (name.find("transformer.decoder") != std::string_view::npos) { return options.lr * options.lr_component_decay; }
    return options.lr;
}

double parameter_weight_decay(std::string_view name, const TrainRequest& options) {
    if (is_encoder_param(name)) { return encoder_zero_weight_decay(name) ? 0.0 : options.weight_decay; }
    return options.weight_decay;
}

}  // namespace

namespace {

// Builds the group/parameter inputs shared by the native optimizers: one NamedParameter per trainable
// parameter and one Group per bucket, recording each bucket's base lr and moving its parameter indices into
// the optimizer group. group_config maps a bucket to the optimizer-specific group configuration.
template <typename Optimizer, typename Groups, typename NamedParams, typename GroupConfigFn>
std::pair<std::vector<typename Optimizer::Group>, std::vector<typename Optimizer::NamedParameter>> build_optimizer_inputs(
    Groups& groups, const NamedParams& named_params, std::vector<double>& base_lrs, GroupConfigFn&& group_config) {
    std::vector<typename Optimizer::Group> optimizer_groups;
    std::vector<typename Optimizer::NamedParameter> optimizer_params;
    optimizer_groups.reserve(groups.size());
    optimizer_params.reserve(named_params.size());
    for (const auto& named_param : named_params) {
        optimizer_params.push_back(typename Optimizer::NamedParameter{named_param.first, named_param.second});
    }
    for (auto& group : groups) {
        base_lrs.push_back(group.lr);
        optimizer_groups.push_back(typename Optimizer::Group{group_config(group), std::move(group.param_indices)});
    }
    return {std::move(optimizer_groups), std::move(optimizer_params)};
}

OptimizerBuildResult build_optimizer(NativeRfDetrModel& model, const TrainRequest& options) {
    struct GroupSpec {
        double lr = 0.0;
        double weight_decay = 0.0;
        bool use_muon = false;
        std::vector<size_t> param_indices;
    };

    std::unordered_map<std::string, size_t> group_index;
    std::vector<GroupSpec> groups;
    std::vector<std::pair<std::string, torch_api::Tensor>> named_params;

    for (const auto& item : detail::native_model_owner(model).module().named_parameters(true)) {
        const auto& param = item.value();
        if (!param.requires_grad()) { continue; }

        const double lr = parameter_lr(item.key(), options);
        const double weight_decay = parameter_weight_decay(item.key(), options);
        const bool use_muon = options.optimizer == TrainOptimizerKind::Muon && muon_parameter_eligible(item.key(), param);
        const std::string key = std::format("{}{}:{}", use_muon ? "muon:" : "aux:", lr, weight_decay);
        auto found = group_index.find(key);
        if (found == group_index.end()) {
            found = group_index.emplace(key, groups.size()).first;
            groups.push_back(GroupSpec{lr, weight_decay, use_muon, {}});
        }
        named_params.emplace_back(item.key(), param);
        groups[found->second].param_indices.push_back(named_params.size() - 1);
    }

    if (groups.empty() || named_params.empty()) { throw std::runtime_error("RF-DETR runtime found no trainable parameters"); }

    std::vector<double> base_lrs;
    base_lrs.reserve(groups.size());
    if (options.optimizer == TrainOptimizerKind::Muon) {
        auto [optimizer_groups, optimizer_params] =
            build_optimizer_inputs<NativeMuonWithAuxAdam>(groups, named_params, base_lrs, [&options](const GroupSpec& group) {
                return NativeMuonGroupConfig{group.lr, group.weight_decay, options.momentum, group.use_muon, true};
            });
        return OptimizerBuildResult{
            NativeOptimizer(NativeMuonWithAuxAdam(std::move(optimizer_groups), std::move(optimizer_params))),
            std::move(base_lrs),
        };
    }

    auto [optimizer_groups, optimizer_params] = build_optimizer_inputs<NativeAdamW>(
        groups, named_params, base_lrs, [](const GroupSpec& group) { return NativeAdamWGroupConfig{group.lr, group.weight_decay, false}; });

    std::vector<torch_api::Tensor> trainable_params;
    trainable_params.reserve(optimizer_params.size());
    for (const auto& named_param : optimizer_params) {
        trainable_params.push_back(named_param.tensor);
    }
    const auto backend = (options.fused_optimizer && native_optimizer_supports_fused(trainable_params)) ? NativeOptimizerBackend::fused
                         : native_optimizer_supports_foreach(trainable_params)                          ? NativeOptimizerBackend::foreach
                                                                                                        : NativeOptimizerBackend::eager;

    return OptimizerBuildResult{
        NativeOptimizer(NativeAdamW(std::move(optimizer_groups), std::move(optimizer_params), backend)),
        std::move(base_lrs),
    };
}

std::vector<NormalizedModelStateEntry> collect_module_state(
    const NativeRfDetrModel& model, const std::unordered_map<std::string, torch_api::Tensor>* parameter_overrides = nullptr) {
    const auto& module = detail::native_model_owner(model).module();
    std::vector<NormalizedModelStateEntry> state;

    const auto parameters = module.named_parameters(true);
    state.reserve(parameters.size() + module.named_buffers(true).size());
    for (const auto& item : parameters) {
        NormalizedModelStateEntry entry;
        entry.name = item.key();
        if (parameter_overrides != nullptr) {
            const auto found = parameter_overrides->find(entry.name);
            if (found != parameter_overrides->end()) {
                entry.tensor = found->second.detach();
                state.push_back(std::move(entry));
                continue;
            }
        }
        entry.tensor = item.value().detach();
        state.push_back(std::move(entry));
    }

    for (const auto& item : module.named_buffers(true)) {
        NormalizedModelStateEntry entry;
        entry.name = item.key();
        entry.tensor = item.value().detach();
        state.push_back(std::move(entry));
    }
    return state;
}

ModelStateLoadSummary load_training_model_weights(NativeRfDetrModel& model, const DecodedNativeModelState& checkpoint,
                                                  const TrainingSupervisionRoute route) {
    const ResolvedClassLayout source_layout(checkpoint.metadata.class_layout);
    auto candidate = detail::native_model_owner(model).stage_normalized_state(detail::model_state_owner(checkpoint).entries,
        route_is_active(route) ? detail::NormalizedModelStateAdmission::FreshTransfer :
                                detail::NormalizedModelStateAdmission::PartialFreshTransfer, &source_layout);
    auto summary = candidate.summary;
    detail::native_model_owner(model).commit_normalized_state(std::move(candidate));
    return summary;
}

void save_collected_checkpoint(const std::filesystem::path& path, const NativeCheckpointMetadata& metadata, const NativeRfDetrModel& model,
                               const std::unordered_map<std::string, torch_api::Tensor>* parameter_overrides, const char* save_profile,
                               const char* collect_profile, const std::filesystem::path& explicit_descriptor) {
    mmltk::common::logging::ScopedProfile save{save_profile};
    DecodedNativeModelState checkpoint;
    checkpoint.metadata = metadata;
    {
        mmltk::common::logging::ScopedProfile collect{collect_profile};
        detail::model_state_owner(checkpoint).entries = collect_module_state(model, parameter_overrides);
    }
    save_native_checkpoint(path, checkpoint, explicit_descriptor);
}

std::vector<NormalizedModelStateEntry> read_state_archive(torch_api::InputArchive& archive, const char* key,
                                                          const std::span<const std::string> expected_names) {
    torch_api::InputArchive state_archive;
    archive.read(key, state_archive);
    const int64_t entry_count = require_int(state_archive, "entry_count");
    if (entry_count < 0 || static_cast<std::uint64_t>(entry_count) != expected_names.size()) {
        throw std::runtime_error("RF-DETR training checkpoint state count does not match the bounded active inventory");
    }

    std::vector<NormalizedModelStateEntry> state_dict;
    state_dict.reserve(static_cast<size_t>(entry_count));
    for (int64_t index = 0; index < entry_count; ++index) {
        torch_api::InputArchive entry_archive;
        state_archive.read(archive_entry_name(static_cast<size_t>(index)), entry_archive);
        torch_api::IValue name_value;
        entry_archive.read("name", name_value);
        if (!name_value.isString()) { throw std::runtime_error("RF-DETR training checkpoint state entry name is not a string"); }
        NormalizedModelStateEntry entry;
        entry.name = name_value.toStringRef();
        if (entry.name != expected_names[static_cast<std::size_t>(index)]) {
            throw std::runtime_error("RF-DETR training checkpoint state names do not match the active inventory");
        }
        entry_archive.read("tensor", entry.tensor);
        state_dict.push_back(std::move(entry));
    }
    return state_dict;
}

NativeCheckpointMetadata checkpoint_metadata(const ResolvedModelArtifacts& artifacts, int64_t num_classes) {
    return make_native_checkpoint_metadata(artifacts, num_classes);
}

std::unordered_map<std::string, torch_api::Tensor> ema_override_map(const std::vector<std::string>& param_names, const ModelEma& ema) {
    const auto& shadow_params = ema.shadow_params();
    if (shadow_params.size() != param_names.size()) { throw std::runtime_error("RF-DETR EMA parameter count changed unexpectedly"); }

    std::unordered_map<std::string, torch_api::Tensor> overrides;
    overrides.reserve(param_names.size());
    for (size_t index = 0; index < param_names.size(); ++index) {
        overrides.emplace(param_names[index], shadow_params[index]);
    }
    return overrides;
}

std::vector<NormalizedModelStateEntry> ema_state_entries(const std::vector<std::string>& param_names, const ModelEma& ema) {
    const auto overrides = ema_override_map(param_names, ema);
    std::vector<NormalizedModelStateEntry> state;
    state.reserve(overrides.size());
    for (const auto& item : param_names) {
        NormalizedModelStateEntry entry;
        entry.name = item;
        entry.tensor = overrides.at(item).detach();
        state.push_back(std::move(entry));
    }
    return state;
}

// One immutable epoch snapshot serves each synchronous archive in publication order.
// The archive-local CPU entries borrow completed slot views until save returns.
void save_snapshot_checkpoint(const std::filesystem::path& path, const NativeCheckpointMetadata& metadata,
                              const std::vector<NormalizedModelStateEntry>& ordinary,
                              const std::vector<NormalizedModelStateEntry>& ema,
                              torch_cuda::TensorReadbackBuffers& readback,
                              const std::filesystem::path& descriptor) {
    DecodedNativeModelState checkpoint;
    checkpoint.metadata = metadata;
    auto& entries = detail::model_state_owner(checkpoint).entries;
    entries.reserve(ordinary.size());
    std::unordered_map<std::string_view, std::size_t> shadows;
    for (std::size_t index = 0; index < ema.size(); ++index) shadows.emplace(ema[index].name, ordinary.size() + index);
    for (std::size_t index = 0; index < ordinary.size(); ++index) {
        auto entry = ordinary[index];
        const auto found = shadows.find(entry.name);
        entry.tensor = readback.Stage(found == shadows.end() ? index : found->second);
        entries.push_back(std::move(entry));
    }
    readback.Complete();
    save_native_checkpoint(path, checkpoint, descriptor);
}

void save_resume_checkpoint(const std::filesystem::path& checkpoint_path, const NativeCheckpointMetadata& metadata, const NativeOptimizer& optimizer, const GradScaler& grad_scaler,
                            const TrainRequest& options, int epoch, double best_regular, double best_ema,
                            const std::vector<NormalizedModelStateEntry>& model_state,
                            const std::vector<NormalizedModelStateEntry>& ema_state, std::string_view attempt_id,
                            const std::filesystem::path& original_descriptor,
                            torch_cuda::TensorReadbackBuffers& readback) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_train_save_resume_total{"rfdetr.train.save.resume.total"};
    std::filesystem::create_directories(checkpoint_path.parent_path());

    torch_api::OutputArchive archive;
    detail::write_native_checkpoint_metadata(archive, metadata);
    detail::write_training_configuration(archive, options);
    write_string(archive, "training_attempt_id", std::string(attempt_id));
    write_string(archive, "training_original_descriptor", original_descriptor.string());
    detail::write_training_supervision_config(archive, options.training_supervision);
    optimizer.reserve_checkpoint(readback, model_state.size() + ema_state.size());
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_train_save_resume_write_state{"rfdetr.train.save.resume.write_state"};
        detail::write_resume_state_archive(archive, "state", model_state, readback, 0);
    }

    torch_api::OutputArchive optimizer_archive;
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_train_save_resume_optimizer_state{"rfdetr.train.save.resume.optimizer_state"};
        optimizer.save(optimizer_archive, readback, model_state.size() + ema_state.size());
    }
    write_string(archive, "optimizer_kind", optimizer.kind_name());
    archive.write("optimizer", optimizer_archive);

    if (options.use_ema) {
        {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_save_resume_ema_write_state{
                "rfdetr.train.save.resume.ema_write_state"};
            detail::write_resume_state_archive(archive, "ema_state", ema_state, readback, model_state.size());
        }
    }

    write_int(archive, "epoch", epoch);
    write_string(archive, "lr_scheduler", cli_enum_spelling(options.lr_scheduler));
    write_int(archive, "lr_drop", options.lr_drop);
    write_double(archive, "warmup_epochs", options.warmup_epochs);
    write_double(archive, "warmup_momentum", options.warmup_momentum);
    write_double(archive, "lr_min_factor", options.lr_min_factor);
    write_bool(archive, "gpu_augment_enabled", options.gpu_augmentation.enabled);
    const auto write_augmentation_group = [&archive](const char* prefix, const AugmentationGroupConfig& group) {
        const std::string probability_key = std::string(prefix) + "_probability";
        const std::string min_strength_key = std::string(prefix) + "_min_strength";
        const std::string max_strength_key = std::string(prefix) + "_max_strength";
        write_double(archive, probability_key.c_str(), group.probability);
        write_double(archive, min_strength_key.c_str(), group.min_strength);
        write_double(archive, max_strength_key.c_str(), group.max_strength);
    };
    write_augmentation_group("gpu_augment_geometry", options.gpu_augmentation.geometry);
    write_augmentation_group("gpu_augment_resize", options.gpu_augmentation.resize);
    write_augmentation_group("gpu_augment_color", options.gpu_augmentation.color);
    write_augmentation_group("gpu_augment_noise", options.gpu_augmentation.noise);
    write_augmentation_group("gpu_augment_blur", options.gpu_augmentation.blur);
    write_augmentation_group("gpu_augment_occlusion", options.gpu_augmentation.occlusion);
    write_double(archive, "gpu_augment_copy_paste_probability", options.gpu_augmentation.copy_paste_probability);
    write_double(archive, "best_regular_metric", best_regular);
    write_double(archive, "best_ema_metric", best_ema);
    write_double(archive, "grad_scaler_scale", static_cast<double>(grad_scaler.current_scale()));
    write_int(archive, "grad_scaler_growth_tracker", grad_scaler.growth_tracker());
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_train_save_resume_archive_save_to{"rfdetr.train.save.resume.archive_save_to"};
        readback.Complete();
        detail::publish_native_checkpoint_archive(archive, checkpoint_path, options.class_layout_path);
    }
}

ResumeState load_resume_checkpoint_state(const std::filesystem::path& checkpoint_path, DecodedNativeModelState& admitted, NativeOptimizer& optimizer,
                                         const TrainRequest& options, const std::vector<std::string>& parameter_names,
                                         const std::vector<torch_api::Tensor>& parameters, const bool main_process) {
    auto& retained = detail::model_state_owner(admitted).native_archive;
    if (!retained || !admitted.class_artifact) throw std::invalid_argument("full resume requires an admitted current native archive");
    admitted.class_artifact->RequireUnchanged();
    auto& archive = *retained;
    const auto saved_configuration = detail::read_training_configuration(archive);
    if (saved_configuration.use_ema != options.use_ema)
        throw std::runtime_error("resume EMA selection differs from the saved training configuration");

    if (const auto lr_scheduler = read_optional_value<std::string>(archive, "lr_scheduler");
        lr_scheduler.has_value() && *lr_scheduler != cli_enum_spelling(options.lr_scheduler)) {
        throw std::runtime_error("native RF-DETR resume checkpoint lr_scheduler does not match current training options");
    }
    if (const auto lr_drop = read_optional_value<int64_t>(archive, "lr_drop"); lr_drop.has_value() && *lr_drop != options.lr_drop) {
        throw std::runtime_error("native RF-DETR resume checkpoint lr_drop does not match current training options");
    }
    if (const auto warmup_epochs = read_optional_value<double>(archive, "warmup_epochs");
        warmup_epochs.has_value() && *warmup_epochs != options.warmup_epochs) {
        throw std::runtime_error("native RF-DETR resume checkpoint warmup_epochs does not match current training options");
    }
    if (const auto warmup_momentum = read_optional_value<double>(archive, "warmup_momentum");
        warmup_momentum.has_value() && *warmup_momentum != options.warmup_momentum) {
        throw std::runtime_error("native RF-DETR resume checkpoint warmup_momentum does not match current training options");
    }
    if (const auto lr_min_factor = read_optional_value<double>(archive, "lr_min_factor");
        lr_min_factor.has_value() && *lr_min_factor != options.lr_min_factor) {
        throw std::runtime_error("native RF-DETR resume checkpoint lr_min_factor does not match current training options");
    }
    const auto optimizer_kind = read_optional_value<std::string>(archive, "optimizer_kind");
    if (!optimizer_kind.has_value()) {
        throw std::runtime_error("native RF-DETR resume checkpoint is missing optimizer_kind: " + checkpoint_path.string());
    }
    if (*optimizer_kind != optimizer.kind_name()) {
        throw std::runtime_error("native RF-DETR resume checkpoint optimizer_kind does not match current training options");
    }
    if (parameter_names.size() != parameters.size()) {
        throw std::runtime_error("native RF-DETR active optimizer parameter inventory is inconsistent");
    }

    ResumeState state;
    state.attempt_id = require_string(archive, "training_attempt_id");
    if (state.attempt_id.empty() || state.attempt_id.size() > 64) throw std::runtime_error("invalid resume attempt identity");
    const auto stored_epoch = read_optional_value<int64_t>(archive, "epoch").value_or(-1);
    if (stored_epoch < -1 || stored_epoch >= static_cast<int64_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("native RF-DETR resume checkpoint epoch is outside the supported range");
    }
    state.start_epoch = static_cast<int>(stored_epoch + 1);
    state.best_regular = read_optional_value<double>(archive, "best_regular_metric").value_or(-std::numeric_limits<double>::infinity());
    state.best_ema = read_optional_value<double>(archive, "best_ema_metric").value_or(-std::numeric_limits<double>::infinity());
    if (std::isnan(state.best_regular) || std::isnan(state.best_ema)) {
        throw std::runtime_error("native RF-DETR resume checkpoint metric state is invalid");
    }
    const auto stored_scale = read_optional_value<double>(archive, "grad_scaler_scale");
    const auto stored_growth = read_optional_value<int64_t>(archive, "grad_scaler_growth_tracker");

    torch_api::InputArchive ema_archive;
    const bool archive_has_ema = archive.try_read("ema_state", ema_archive);
    validate_resume_continuation_manifest(ResumeContinuationManifest{options.use_ema, archive_has_ema, stored_scale, stored_growth});
    std::optional<ModelEma> staged_ema;
    if (archive_has_ema) {
        auto ema_state = read_state_archive(archive, "ema_state", parameter_names);
        std::vector<torch_api::Tensor> cpu_shadow;
        cpu_shadow.reserve(ema_state.size());
        for (const auto& entry : ema_state) cpu_shadow.push_back(entry.tensor);
        if (main_process) {
            staged_ema = ModelEma::from_cpu_shadow(parameters, cpu_shadow, options.ema_decay, static_cast<double>(options.ema_tau));
        } else {
            for (std::size_t index = 0; index < cpu_shadow.size(); ++index) {
                const auto& source = cpu_shadow[index];
                const auto& destination = parameters[index];
                if (!source.defined() || !source.device().is_cpu() || !source.is_floating_point() ||
                    source.sizes() != destination.sizes() || source.scalar_type() != destination.scalar_type() ||
                    source.layout() != destination.layout() || !torch_api::isfinite(source).all().item<bool>())
                    throw std::runtime_error("native RF-DETR resume EMA tensor does not match the active parameter inventory");
            }
        }
    }

    torch_api::InputArchive optimizer_archive;
    if (!archive.try_read("optimizer", optimizer_archive)) {
        throw std::runtime_error("native RF-DETR resume checkpoint is missing optimizer state: " + checkpoint_path.string());
    }
    try {
        state.optimizer_candidate = optimizer.stage_load(optimizer_archive);
    } catch (const std::exception& error) {
        throw std::runtime_error("failed to load native RF-DETR optimizer state from " + checkpoint_path.string() + ": " + error.what());
    }

    if (stored_scale.has_value()) {
        state.scaler_scale = static_cast<float>(*stored_scale);
        state.scaler_growth_tracker = static_cast<int>(*stored_growth);
    }
    state.restored_ema = std::move(staged_ema);

    return state;
}

// How many predictions per image the criterion scores, and how many ranks it normalizes over.
void apply_detection_scale(DetectionConfig& detection_config, const NativeRfDetrConfig& config, const int64_t world_size) {
    detection_config.num_classes = config.num_classes;
    detection_config.group_detr = config.group_detr;
    detection_config.dec_layers = config.dec_layers;
    detection_config.num_select = config.num_select;
    detection_config.two_stage = config.two_stage;
    detection_config.world_size = std::max<int64_t>(1, world_size);
}

// Mask supervision: whether it runs at all, and how it is sampled and weighted.
void apply_detection_mask_supervision(DetectionConfig& detection_config, const NativeRfDetrConfig& config) {
    detection_config.include_masks = config.segmentation;
    detection_config.mask_point_sample_ratio = config.mask_point_sample_ratio;
    detection_config.mask_ce_loss_coef = config.mask_ce_loss_coef;
    detection_config.mask_dice_loss_coef = config.mask_dice_loss_coef;
}

// Which classification loss formulation the criterion evaluates, and how its ops are executed.
void apply_detection_loss_terms(DetectionConfig& detection_config, const NativeRfDetrConfig& config,
                                const CompilationMode compilation_mode) {
    detection_config.sum_group_losses = config.sum_group_losses;
    detection_config.use_varifocal_loss = config.use_varifocal_loss;
    detection_config.use_position_supervised_loss = config.use_position_supervised_loss;
    detection_config.ia_bce_loss = config.ia_bce_loss;
    detection_config.aux_loss = config.aux_loss;
    detection_config.focal_alpha = config.focal_alpha;
    detection_config.use_jit_traced_loss_ops = (compilation_mode == CompilationMode::kSelective);
}

// Box loss weights and the matching costs that pair predictions with targets.
void apply_detection_box_weights(DetectionConfig& detection_config, const NativeRfDetrConfig& config) {
    detection_config.cls_loss_coef = config.cls_loss_coef;
    detection_config.bbox_loss_coef = config.bbox_loss_coef;
    detection_config.giou_loss_coef = config.giou_loss_coef;
    detection_config.set_cost_class = config.set_cost_class;
    detection_config.set_cost_bbox = config.set_cost_bbox;
    detection_config.set_cost_giou = config.set_cost_giou;
}

DetectionConfig make_detection_config(const NativeRfDetrConfig& config, int64_t world_size, CompilationMode compilation_mode) {
    DetectionConfig detection_config;
    apply_detection_scale(detection_config, config, world_size);
    apply_detection_mask_supervision(detection_config, config);
    apply_detection_loss_terms(detection_config, config, compilation_mode);
    apply_detection_box_weights(detection_config, config);
    populate_default_detection_weight_dict(detection_config);
    return detection_config;
}

size_t full_batches_per_rank(size_t total_images, size_t batch_size, size_t world_size, int grad_accum_steps, int train_lane_count) {
    const size_t total_full_batches = total_images / batch_size;
    const size_t local_full_batches = total_full_batches / std::max<size_t>(1, world_size);
    const size_t batches_per_step = static_cast<size_t>(std::max(1, grad_accum_steps)) * static_cast<size_t>(std::max(1, train_lane_count));
    return (local_full_batches / batches_per_step) * batches_per_step;
}

void average_gradients(const DistributedContext& distributed, const std::vector<torch_api::Tensor>& parameters) {
    if (!distributed.enabled) { return; }

    std::vector<torch_api::Tensor> gradients;
    gradients.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        if (parameter.grad().defined()) { gradients.push_back(parameter.grad()); }
    }
    if (gradients.empty()) { return; }

    distributed_all_reduce_coalesced(distributed, gradients);
    const double scale = 1.0 / static_cast<double>(distributed.world_size);
    for (auto& gradient : gradients) {
        gradient.mul_(scale);
    }
}

std::future<TrainLaneResult> enqueue_train_lane(
    mmltk::common::concurrency::WorkerPool& lane_pool, RuntimeContext* runtime, mmltk::backend::data::DatasetLoader& loader,
    TrainLaneContext& lane, const mmltk::backend::data::Batch& batch, const mmltk::backend::ml::cuda::CudaEventPool::Lease* params_ready,
    mmltk::backend::ml::cuda::CudaEventPool& event_pool, double scaled_loss_factor, size_t parameter_version,
    const DetectionConfig& detection_config, const NativeRfDetrModel& model, int device_id, int image_height, int image_width,
    std::uint64_t seed, int epoch, int rank, std::uint64_t augmentation_sequence, bool amp_enabled, torch_api::ScalarType autocast_dtype,
    TrainingSupervisionRoute route, std::shared_ptr<WaveTargetNormalizer> wave_normalizer, std::size_t lane_index) {
    return lane_pool.enqueue([runtime, &loader, &lane, batch, params_ready, &event_pool, scaled_loss_factor,
                              parameter_version, &detection_config, &model, device_id, image_height, image_width, seed, epoch, rank,
                              augmentation_sequence, amp_enabled, autocast_dtype, route, wave_normalizer = std::move(wave_normalizer),
                              lane_index]() mutable {
        try {
            ScopedRuntimeContext worker_scope(runtime, lane_index + 1);
            torch_cuda::TorchCudaDeviceGuard device_guard(torch_cuda::checked_device_index(device_id));
            torch_cuda::TorchCudaStreamGuard stream_guard(lane.stream);
            LoaderBatchGuard batch_guard(loader, batch, device_id);

            torch_api::Tensor normalized;
            {
                mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_augment{"rfdetr.train.parallel.augment"};
                mmltk::common::logging::ScopedProfile profile_benchmark_rfdetr_train_augmentation{"benchmark.rfdetr.train.augmentation"};
                if (!lane.augmenter) { throw std::runtime_error("parallel RF-DETR train lane is missing its GPU augmenter"); }
                normalized = lane.augmenter->run(batch, seed, epoch, rank, augmentation_sequence);
            }

            PreparedTargets prepared;
            {
                mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_targets{"rfdetr.train.parallel.targets"};
                prepared =
                    build_targets(batch, image_height, image_width, detection_config.include_masks, detection_config.include_masks,
                                  device_id, lane.target_scratch, "train", model.config().num_queries, model.config().training_supervision,
                                  static_cast<int>(model.class_layout()->catalog()->size()), &lane.augmenter->batch_plan());
            }
            const auto target_count = prepared_target_count(prepared);
            if (wave_normalizer) { wave_normalizer->publish(lane_index, target_count); }
            batch_guard.set_consumer_stream(lane.augmenter->prepare_batch_consumer());
            static_cast<void>(lane.augmenter->finish_batch(batch));
            batch_guard.release();

            if (!lane.model) { throw std::runtime_error("parallel RF-DETR train lane is missing its model replica"); }
            if (lane.synced_parameter_version != parameter_version) {
                if (params_ready) {
                    params_ready->wait(reinterpret_cast<std::uintptr_t>(lane.stream.stream()),
                                       "wait for parallel train parameter readiness");
                }
                copy_module_state(detail::native_model_owner(*lane.model).module(), detail::native_model_owner(model).module());
                lane.synced_parameter_version = parameter_version;
            }
            detail::native_model_owner(*lane.model).module().train();

            torch_api::Tensor detached_loss;
            torch_api::Tensor detached_class_loss;
            torch_api::Tensor detached_box_loss;
            scalar_packet::Tensors scalar_values;
            std::vector<torch_api::Tensor> gradients;
            {
                TensorMap loss_dict;
                torch_api::Tensor loss;
                torch_api::Tensor class_loss;
                torch_api::Tensor box_loss;
                mmltk::backend::ml::cuda::TorchAutocastScope autocast_guard(amp_enabled, autocast_dtype);
                TargetConsumerLease target_consumer(lane.target_scratch, prepared, device_id);
                ModelOutputs outputs;
                auto& lane_owner = detail::native_model_owner(*lane.model);
                SupervisionTimingLease step_timing(lane_owner, route_is_active(route), SupervisionTimingLease::Kind::Step);
                if (route_uses_denoising(route)) {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_targets_handoff{
                        "rfdetr.train.parallel.targets_handoff"};
                    target_consumer.handoff();
                    outputs = detail::native_model_owner(*lane.model)
                                  .forward_with_denoising(NestedTensor{normalized, prepared.nested_mask}, prepared,
                                                          TrainingStepIdentity{seed, static_cast<std::uint64_t>(epoch),
                                                                               static_cast<std::uint32_t>(rank), augmentation_sequence});
                } else {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_forward{"rfdetr.train.parallel.forward"};
                    outputs =
                        route_uses_match_free(route)
                            ? detail::native_model_owner(*lane.model).forward_for_match_free(NestedTensor{normalized, prepared.nested_mask})
                            : detail::native_model_owner(*lane.model).forward(NestedTensor{normalized, prepared.nested_mask}, true);
                }
                if (!route_uses_denoising(route)) {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_targets_handoff{
                        "rfdetr.train.parallel.targets_handoff"};
                    target_consumer.handoff();
                }
                if (route_is_active(route) || wave_normalizer) {
                    if (!wave_normalizer) { throw std::runtime_error("active RF-DETR lane is missing its wave target normalizer"); }
                    auto normalizer = wave_normalizer->consume(lane_index, lane.stream.stream());
                    SupervisionTimingLease criterion_timing(lane_owner, route_is_active(route), SupervisionTimingLease::Kind::Criterion);
                    // CLEANUP-IGNORE: The parallel lane unpacks into detached gradient work owned by this lane.
                    auto routed = compute_routed_training_loss(*lane.model, route, outputs, prepared, normalizer, detection_config);
                    criterion_timing.finish();
                    loss = std::move(routed.total);
                    class_loss = std::move(routed.classification);
                    box_loss = std::move(routed.box);
                    loss_dict = std::move(routed.ordinary_terms);
                    scalar_values = std::move(routed.scalars);
                } else {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_loss_dict{"rfdetr.train.parallel.loss_dict"};
                    const double group_divisor = detection_config.sum_group_losses ? 1.0 : detection_config.group_detr;
                    const double num_boxes_value =
                        std::max(static_cast<double>(target_count) * group_divisor / std::max<int64_t>(1, detection_config.world_size), 1.0);
                    loss_dict = detection_loss_dict(outputs, prepared, detection_config, true, num_boxes_value);
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_loss_total{"rfdetr.train.parallel.loss_total"};
                    torch_api::Tensor auxiliary;
                    loss = weighted_detection_loss(loss_dict, detection_config, normalized.device(), &auxiliary);
                    scalar_values = ordinary_scalar_tensors(loss_dict, loss, auxiliary);
                    class_loss = loss_value_or_zero(loss_dict, normalized.device(), "loss_ce");
                    box_loss = loss_value_or_zero(loss_dict, normalized.device(), "loss_bbox");
                }
                {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_grad{"rfdetr.train.parallel.grad"};
                    gradients = torch_api::grad({loss * scaled_loss_factor}, lane.grad_params, {}, std::nullopt, false, true);
                    runtime->matcher_workspace().complete_assignments(lane.stream.stream());
                }
                target_consumer.retire();
                step_timing.finish();
                detached_loss = loss.detach();
                detached_class_loss = class_loss.detach();
                detached_box_loss = box_loss.detach();
            }

            return TrainLaneResult{
                std::move(detached_loss),
                std::move(detached_class_loss),
                std::move(detached_box_loss),
                std::move(scalar_values),
                std::move(gradients),
                event_pool.record(reinterpret_cast<std::uintptr_t>(lane.stream.stream()), "record parallel train lane completion event"),
            };
        } catch (...) {
            if (wave_normalizer) { wave_normalizer->fail(std::current_exception()); }
            throw;
        }
    });
}

EvalPassResult evaluate_model(const TrainRequest& options, TrainingValidationRuntime& validation, NativeRfDetrModel& model,
                              TrainingEventOwner& event_owner, const DetectionConfig& detection_config, bool calculate_loss,
                              std::optional<int> current_epoch, std::string progress_label, TrainingMetricHandoff* metrics = nullptr) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_total{"rfdetr.train.eval.total"};
    RuntimeContext& runtime = validation.runtime();
    mmltk::backend::data::DatasetLoader& loader = validation.loader();
    const std::vector<int>& image_ids = validation.image_ids();

    EvalPassResult result;
    torch_api::InferenceMode inference_mode;
    struct RestoreMode final {
        torch_api::Module& module;
        bool training;
        ~RestoreMode() { module.train(training); }
    } restore_mode{detail::native_model_owner(model).module(), detail::native_model_owner(model).module().is_training()};
    detail::native_model_owner(model).module().eval();
    validation.begin_pass();
    TrainingEvaluationRunOwner& evaluation_run = validation.evaluation_run();
    auto cancel_unsettled_run = std::unique_ptr<TrainingEvaluationRunOwner, void (*)(TrainingEvaluationRunOwner*)>{
        &validation.evaluation_run(), [](TrainingEvaluationRunOwner* owner) { owner->cancel(); }};

    const bool amp_enabled = validation.amp_enabled();
    const torch_api::ScalarType autocast_dtype = validation.inference_dtype();
    const bool match_free_loss = calculate_loss && model.config().training_supervision.assignment == TrainAssignmentKind::MatchFree;

    const auto started_at = std::chrono::steady_clock::now();
    const bool profiling = evaluation_run.profiling();
    if (profiling) {
        evaluation_run.configure_profile(EvaluationProfileSetup{
            options.output_dir / "validation_profile.jsonl",
            "training_validation",
            progress_label.starts_with("ema") ? "ema" : (progress_label.starts_with("test") ? "test" : "model"),
            evaluation_precision_name(autocast_dtype),
            current_epoch.has_value() ? std::optional<int>(*current_epoch + 1) : std::nullopt,
            evaluation_run.dataset().facts().metric_set,
            validation.batch_size(),
            loader.image_height(),
            loader.image_width(),
            static_cast<size_t>(model.config().num_queries),
            loader.max_instances_per_image(),
            validation.detection_limit().as_size,
            static_cast<size_t>(model.config().num_classes),
            validation.query_count_automatic(),
            validation.detection_limit().automatic,
            started_at,
        });
    }
    size_t image_count = 0;
    size_t batch_count = 0;
    size_t seen_images = 0;
    if (calculate_loss && !metrics) throw std::logic_error("validation loss requires the retained scalar handoff");
    if (calculate_loss) metrics->begin_validation();
    std::optional<CapturedEvalSample> captured_sample;
    const bool capture_eval_sample = current_epoch.has_value() && progress_label.starts_with("val ");
    std::unique_ptr<spdmon::ProgressBar> progress;
    if (options.progress_bar) { progress = std::make_unique<spdmon::ProgressBar>(std::move(progress_label), loader.num_images(), "img"); }
    std::mt19937_64 sample_rng(
        static_cast<uint64_t>(options.seed) ^
        (current_epoch.has_value() ? (0x9e3779b97f4a7c15ULL + static_cast<uint64_t>(*current_epoch + 1)) : 0xd1b54a32d192ed03ULL));

    mmltk::common::concurrency::WorkerPool& lane_pool = validation.lane_pool();
    mmltk::common::concurrency::WorkerPool& cpu_pool = runtime.cpu_pool();
    torch_cuda::TorchCudaDeviceGuard device_guard(torch_cuda::checked_device_index(options.device_id));

    const size_t max_cpu_futures = prediction_cpu_batch_limit(runtime.split(), validation.batch_size());
    auto drain_cpu = [&]() {
        const size_t completed_count = evaluation_run.drain_encoding();
        image_count += completed_count;
        if (progress) { progress->add(completed_count); }
    };

    mmltk::backend::data::Batch batch{};
    ClassPostprocessLane evaluation_classes(model.class_layout());
    evaluation_classes.Prepare(cuda_device(options.device_id));

    while (loader.next_batch(batch)) {
        mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_batch{"rfdetr.train.eval.batch"};
        {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_wait_batch{"rfdetr.train.eval.wait_batch"};
            if (profiling) {
                const auto phase_started = std::chrono::steady_clock::now();
                loader.wait_batch(batch);
                evaluation_run.record_loader_wait(std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_started).count());
            }
        }
        LoaderBatchGuard batch_guard(loader, batch, options.device_id);
        mmltk::common::logging::profile_add_value("rfdetr.train.eval.images", batch.num_images);
        EvaluationCudaTimingLease batch_timing;
        cudaStream_t evaluation_stream = nullptr;
        if (profiling) {
            batch_timing = evaluation_run.acquire_timing();
            evaluation_stream = torch_cuda::current_torch_cuda_stream_object(torch_cuda::checked_device_index(options.device_id)).stream();
            evaluation_run.record_timing_start(batch_timing, EvaluationCudaBatchTiming::Phase::Preprocessing, evaluation_stream);
        }

        torch_api::Tensor inference_input;
        {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_normalize{"rfdetr.train.eval.normalize"};
            inference_input = validation.preprocess(batch);
        }
        if (batch_timing) {
            evaluation_run.record_timing_stop(batch_timing, EvaluationCudaBatchTiming::Phase::Preprocessing, evaluation_stream);
        }

        std::optional<size_t> sampled_image_pos;
        torch_api::Tensor sampled_image;
        if (capture_eval_sample) {
            for (size_t image_pos = 0; image_pos < batch.num_images; ++image_pos) {
                ++seen_images;
                std::uniform_int_distribution<size_t> select_current(0, seen_images - 1);
                if (select_current(sample_rng) == 0) {
                    sampled_image_pos = image_pos;
                    sampled_image = make_device_batch_tensor(batch, options.device_id, loader.image_height(), loader.image_width())
                                        .select(0, static_cast<int64_t>(image_pos))
                                        .clone();
                }
            }
        }
        std::vector<PredictionBatchMetadata> prediction_metadata = make_prediction_batch_metadata(batch, image_ids);
        std::optional<PreparedTargets> prepared;
        if (calculate_loss) {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_targets{"rfdetr.train.eval.targets"};
            prepared = build_targets(batch, static_cast<int>(loader.image_height()), static_cast<int>(loader.image_width()),
                                     detection_config.include_masks, detection_config.include_masks, options.device_id,
                                     validation.target_scratch(), validation.split_name(), model.config().num_queries,
                                     model.config().training_supervision, static_cast<int>(model.class_layout()->catalog()->size()));
        }
        batch_guard.release();

        std::optional<TargetConsumerLease> target_consumer;
        if (prepared) { target_consumer.emplace(validation.target_scratch(), *prepared, options.device_id); }
        ModelOutputs outputs;
        {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_forward{"rfdetr.train.eval.forward"};
            if (batch_timing) {
                evaluation_run.record_timing_start(batch_timing, EvaluationCudaBatchTiming::Phase::ModelForward, evaluation_stream);
            }
            {
                mmltk::backend::ml::cuda::TorchAutocastScope autocast_guard(amp_enabled, autocast_dtype);
                outputs =
                    match_free_loss
                        ? detail::native_model_owner(model).forward_for_match_free(NestedTensor{inference_input, validation.nested_mask()})
                        : detail::native_model_owner(model).forward(NestedTensor{inference_input, validation.nested_mask()}, true);
            }
            assert_inference_output_dtype(outputs.main.pred_logits, outputs.main.pred_boxes, autocast_dtype, "RF-DETR training validation");
            validation.record_preprocess_consumer(
                torch_cuda::current_torch_cuda_stream_object(torch_cuda::checked_device_index(options.device_id)).stream());
            if (profiling) {
                evaluation_run.record_model_output(evaluation_precision_name(outputs.main.pred_logits.scalar_type()),
                                                   evaluation_precision_name(outputs.main.pred_boxes.scalar_type()),
                                                   static_cast<size_t>(outputs.main.pred_logits.size(1)),
                                                   static_cast<size_t>(outputs.main.pred_logits.size(2)));
            }
            if (batch_timing) {
                evaluation_run.record_timing_stop(batch_timing, EvaluationCudaBatchTiming::Phase::ModelForward, evaluation_stream);
            }
        }

        const auto real_count = static_cast<int64_t>(batch.num_images);
        if (outputs.main.pred_logits.size(0) < real_count) {
            throw std::runtime_error("RF-DETR validation output batch is smaller than the input batch");
        }
        std::optional<ModelOutputs> real_outputs;
        const ModelOutputs* evaluated_outputs = &outputs;
        if (outputs.main.pred_logits.size(0) != real_count) {
            real_outputs = narrow_model_outputs_batch(outputs, real_count);
            evaluated_outputs = &*real_outputs;
        }

        if (calculate_loss) {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_targets_handoff{"rfdetr.train.eval.targets_handoff"};
            target_consumer->handoff();

            torch_api::Tensor loss;
            auto& model_owner = detail::native_model_owner(model);
            SupervisionTimingLease criterion_timing(model_owner, training_supervision_enabled(model.config().training_supervision),
                                                    SupervisionTimingLease::Kind::Criterion);
            if (match_free_loss) {
                const auto target_count =
                    torch_api::tensor({static_cast<float>(prepared_target_count(*prepared))},
                                      torch_api::TensorOptions().dtype(torch_api::kFloat32).device(inference_input.device()));
                loss = detail::native_model_owner(model)
                           .supervision_loss(*evaluated_outputs, *prepared, DeviceLossNormalizer{target_count.select(0, 0)}, false)
                           .total;
            } else {
                TensorMap loss_dict;
                {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_loss_dict{"rfdetr.train.eval.loss_dict"};
                    loss_dict = detection_loss_dict(*evaluated_outputs, *prepared, detection_config, false, false);
                    runtime.matcher_workspace().complete_assignments(
                        torch_cuda::getCurrentCUDAStream(torch_cuda::checked_device_index(options.device_id)).stream());
                }
                mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_loss_total{"rfdetr.train.eval.loss_total"};
                loss = weighted_detection_loss(loss_dict, detection_config, inference_input.device());
            }
            criterion_timing.finish();
            target_consumer->retire();
            metrics->accumulate_validation(loss);
            static_cast<void>(detail::native_model_owner(model).harvest_supervision_timing());
            ++batch_count;
        }

        PostprocessedBatch processed = [&]() {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_postprocess{"rfdetr.train.eval.postprocess"};
            if (batch_timing) {
                evaluation_run.record_timing_start(batch_timing, EvaluationCudaBatchTiming::Phase::Postprocess, evaluation_stream);
            }
            PostprocessedBatch postprocessed = postprocess_output_batch_fixed_size(
                OutputTensors{evaluated_outputs->main.pred_logits, evaluated_outputs->main.pred_boxes, evaluated_outputs->main.pred_masks},
                static_cast<int64_t>(loader.image_height()), static_cast<int64_t>(loader.image_width()), model.config().num_select, &evaluation_classes);
            if (batch_timing) {
                evaluation_run.record_timing_stop(batch_timing, EvaluationCudaBatchTiming::Phase::Postprocess, evaluation_stream);
            }
            return postprocessed;
        }();
        auto processed_ready = record_current_stream_event(event_owner.pool(), options.device_id, "record train eval prediction readiness");
        if (!processed_ready.has_value()) { throw std::runtime_error("training event pool is unavailable for evaluation"); }

        const size_t processed_count = std::min(static_cast<size_t>(processed.size()), static_cast<size_t>(batch.num_images));
        evaluation_run.record_prediction_transfer(processed, processed_count);
        prediction_metadata.resize(processed_count);
        if (sampled_image_pos.has_value() && *sampled_image_pos < processed_count) {
            const auto image_index = static_cast<int64_t>(*sampled_image_pos);
            const auto scores = processed.scores[image_index];
            const auto labels = processed.labels[image_index];
            const auto boxes = processed.boxes[image_index];
            const torch_api::Tensor masks = processed.masks.has_value() ? (*processed.masks)[image_index] : torch_api::Tensor();
            const auto keep = scores.gt(0.35f);
            const auto keep_indices = keep.nonzero().squeeze(1);
            torch_api::Tensor filtered_masks;
            if (masks.defined()) { filtered_masks = masks.index_select(0, keep_indices).clone(); }
            captured_sample = CapturedEvalSample{
                std::move(sampled_image),
                boxes.index_select(0, keep_indices).clone(),
                labels.index_select(0, keep_indices).clone(),
                std::move(filtered_masks),
            };
        }

        evaluation_run.submit(
            lane_pool,
            [processed = std::move(processed), processed_ready = std::move(*processed_ready), metadata = std::move(prediction_metadata),
             category_count = loader.num_classes(), max_dets = validation.detection_limit().as_size,
             device_id = options.device_id](auto& lane) mutable {
                PredictionBufferLease lease = lane.slot_pool->acquire();
                processed_ready.wait(reinterpret_cast<std::uintptr_t>(lane.stream.stream()), "wait for train eval prediction readiness");
                processed_ready.retire();
                torch_api::InferenceMode lane_inference_mode;
                torch_cuda::TorchCudaDeviceGuard lane_device_guard(torch_cuda::checked_device_index(device_id));
                torch_cuda::TorchCudaStreamGuard stream_guard(lane.stream);
                return stage_prediction_batch(std::move(metadata), std::move(processed), category_count, max_dets, std::move(lease),
                                              device_id, reinterpret_cast<void*>(lane.stream.stream()));
            },
            batch_timing);
        while (static_cast<int>(evaluation_run.pending_lane_count()) >= runtime.split().lane_threads) {
            evaluation_run.drain_lane(cpu_pool);
        }
        while (evaluation_run.pending_encoding_count() >= max_cpu_futures) {
            drain_cpu();
        }
    }

    while (evaluation_run.pending_lane_count() != 0U) {
        evaluation_run.drain_lane(cpu_pool);
        while (evaluation_run.pending_encoding_count() >= max_cpu_futures) {
            drain_cpu();
        }
    }
    while (evaluation_run.pending_encoding_count() != 0U) {
        drain_cpu();
    }
    if (capture_eval_sample && captured_sample.has_value()) {
        RenderSampleOptions render_options;
        render_options.num_classes = static_cast<int>(model.class_layout()->catalog()->size());
        render_options.output_path = options.output_dir / "eval_samples" / std::format("epoch_{}.png", *current_epoch + 1);
        draw_eval_sample_async_gpu(captured_sample->image, captured_sample->boxes, captured_sample->labels, captured_sample->masks,
                                   render_options);
    }
    if (progress) { progress->close(); }
    if (calculate_loss) { result.loss = metrics->validation_average(batch_count); }
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_metric{"rfdetr.train.eval.metric"};
        result.summary = evaluation_run.evaluate(validation.detection_limit().as_size, cpu_pool);
        result.summary.model_detection_budget = checked_cast<std::uint32_t>(model.config().num_select, "training model detection budget exceeds uint32_t");
    }
    evaluation_run.settle(result.summary);
    static_cast<void>(cancel_unsettled_run.release());
    flush_eval_sample_writes();
    result.timing = elapsed_timing(started_at, image_count);
    return result;
}

class TrainingRuntimeOwner final {
   public:
    explicit TrainingRuntimeOwner(const TrainRequest& options);
    ~TrainingRuntimeOwner();

    TrainingRuntimeOwner(const TrainingRuntimeOwner&) = delete;
    TrainingRuntimeOwner& operator=(const TrainingRuntimeOwner&) = delete;

    [[nodiscard]] TrainRunResult run();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace

TrainRequest finalize_train_request(TrainRequest request) {
    if (request.preset_name.empty()) {
        const auto& source = request.resume_path.empty() ? request.weights_path : request.resume_path;
        request.preset_name = infer_train_recipe_preset_name_from_path(source);
    }
    apply_train_recipe(request, resolve_train_recipe(request.preset_name, request.optimizer), request.recipe_overrides);
    if (!request.device_ids.empty()) {
        const auto partitions = select_distributed_training_partitions(request);
        const auto rank = static_cast<std::size_t>(request.distributed_rank);
        if (request.distributed_worker && rank < partitions.size()) {
            apply_training_partition(request, partitions[rank]);
        } else if (!request.distributed_worker && partitions.size() == 1U) {
            apply_training_partition(request, partitions.front());
        }
    }
    validate_train_request(request);
    return request;
}

namespace {

struct TrainingRuntimeOwner::Impl final {
    explicit Impl(const TrainRequest& request) : options(request) {}

    [[nodiscard]] TrainRunResult run();

    const TrainRequest& options;
};

TrainRunResult TrainingRuntimeOwner::Impl::run() {
    mmltk::common::logging::ScopedProfile profile_rfdetr_train_total{"rfdetr.train.total"};
    // The public host vocabulary is available to this CUDA-linked target;
    // request-only admission remains in the runtime boundary.
    validate_train_request(options);
    if (!options.distributed_worker && options.device_ids.size() > 1) {
        throw std::runtime_error("multi-GPU RF-DETR training requires one materialized worker request per selected partition");
    }

    const int requested_train_lanes = effective_train_lanes(options);
    auto runtime_config = resolve_runtime_config(options.workers, requested_train_lanes, options.prefetch_factor, options.cpu_affinity,
                                                 options.device_id, options.numa_node);
    runtime_config.h2d_dataloader = options.h2d_dataloader;
    RuntimeContext train_runtime(runtime_config);
    const auto& placement = train_runtime.execution().placement;
    mmltk::common::system::ScopedExecutionPolicy boundary_policy({train_runtime.lane_cpus(), {}, 0, placement.numa_node, -10, false});
    DistributedContext distributed = make_distributed_context(options);
    const bool main_process = is_rank_zero(distributed);
    const int train_lane_count = train_runtime.split().lane_threads;
    TrainingEventOwner training_events(options.device_id, static_cast<std::size_t>(train_lane_count) + 1U);
    ScopedRuntimeContext worker_scope(&train_runtime);

    auto make_loader_config_for = [&](const std::filesystem::path& compiled_path, size_t loader_batch_size, bool shuffle,
                                      int prefetch_factor, bool shard_batches, bool drop_last) {
        auto config =
            make_loader_config(compiled_path.string(), loader_batch_size, shuffle, prefetch_factor, train_runtime.split().gather_threads,
                               train_runtime.loader_affinity_string(), options.device_id, static_cast<uint64_t>(options.seed));
        config.loading = options;
        config.execution = train_runtime.execution();
        config.execution->placement.cpus = train_runtime.loader_cpus();
        config.drop_last = drop_last;
        if (distributed.enabled && shard_batches) {
            config.batch_shard_rank = static_cast<uint32_t>(distributed.rank);
            config.batch_shard_count = static_cast<uint32_t>(distributed.world_size);
        }
        return config;
    };

    const size_t val_batch_size = options.val_batch_size > 0 ? options.val_batch_size : options.batch_size;
    mmltk::backend::data::DatasetLoader train_loader(
        make_loader_config_for(options.train_compiled_path, options.batch_size, true, options.prefetch_factor, true, true));
    std::unique_ptr<mmltk::backend::data::DatasetLoader> val_loader;
    if (main_process) {
        val_loader = std::make_unique<mmltk::backend::data::DatasetLoader>(
            make_loader_config_for(options.val_compiled_path, val_batch_size, false, options.prefetch_factor, false, false));
    }

    const std::uint32_t val_max_instances =
        val_loader ? val_loader->max_instances_per_image()
                   : mmltk::backend::data::inspect_compiled_dataset(options.val_compiled_path).max_instances_per_image;
    std::optional<mmltk::backend::data::CompiledDatasetInfo> test_info;
    if (!options.test_compiled_path.empty()) { test_info = mmltk::backend::data::inspect_compiled_dataset(options.test_compiled_path); }
    TrainingDatasetLimits dataset_limits;
    dataset_limits.train_max_instances = train_loader.max_instances_per_image();
    dataset_limits.val_max_instances = val_max_instances;
    if (test_info.has_value()) { dataset_limits.test_max_instances = test_info->max_instances_per_image; }
    dataset_limits.largest_max_instances = std::max(dataset_limits.train_max_instances, dataset_limits.val_max_instances);
    if (dataset_limits.test_max_instances.has_value()) {
        dataset_limits.largest_max_instances = std::max(dataset_limits.largest_max_instances, *dataset_limits.test_max_instances);
    }
    const auto source_checkpoint = !options.resume_path.empty() ? options.resume_path : options.weights_path;
    if (train_loader.image_width() != train_loader.image_height()) {
        throw std::runtime_error("train compiled RF-DETR input must be square");
    }
    ModelArtifactRequest artifact_request;
    artifact_request.weights_path = source_checkpoint;
    artifact_request.preset_name = options.preset_name;
    artifact_request.resolution = static_cast<int>(train_loader.image_width());
    auto admitted = resolve_model_state(artifact_request.weights_path, artifact_request.preset_name, artifact_request.resolution, options.class_layout_path);
    auto artifacts = admitted.artifacts;
    auto original_descriptor = options.class_layout_path;
    if (!options.resume_path.empty()) {
        if (!detail::model_state_owner(admitted.model_state).native_archive) {
            throw std::runtime_error("--resume requires a native RF-DETR .pt checkpoint: " + options.resume_path.string());
        }
        detail::require_resume_training_supervision_config(*detail::model_state_owner(admitted.model_state).native_archive, options.training_supervision);
        original_descriptor = read_optional_value<std::string>(*detail::model_state_owner(admitted.model_state).native_archive,
            "training_original_descriptor").value_or(options.class_layout_path.string());
    }
    artifacts.config.training_supervision = options.training_supervision;
    dataset_limits.automatic_num_queries_cap =
        checked_cast<std::size_t>(artifacts.automatic_num_queries_cap, "RF-DETR automatic query cap exceeds size_t");
    const ResolvedDatasetLimit required_query_limit =
        resolve_dataset_query_limit(dataset_limits.largest_max_instances, 0U, dataset_limits.automatic_num_queries_cap);
    const ResolvedDatasetLimit requested_query_limit =
        resolve_dataset_query_limit(dataset_limits.largest_max_instances, options.num_queries, dataset_limits.automatic_num_queries_cap);
    dataset_limits.required_num_queries = required_query_limit.as_size;
    dataset_limits.requested_override = options.num_queries != 0U;
    if (training_supervision_enabled(artifacts.config.training_supervision) &&
        (train_loader.num_classes() == 0 || train_loader.num_classes() >= static_cast<std::uint32_t>(std::numeric_limits<int>::max()))) {
        throw std::runtime_error("feature-active RF-DETR supervision requires a representable nonempty object-class catalog");
    }
    const int dataset_output_classes = rfdetr_output_class_count(train_loader.num_classes());
    if (!options.resume_path.empty()) {
        const auto& resume_checkpoint = admitted.model_state;
        if (resume_checkpoint.metadata.class_layout != native_training_class_layout(*train_loader.class_catalog()))
            throw std::runtime_error("resume class layout does not match ordered compiled catalog");
        if (resume_checkpoint.metadata.num_classes > 0 &&
            resume_checkpoint.metadata.num_classes != static_cast<int64_t>(dataset_output_classes)) {
            throw std::runtime_error("resume checkpoint class count does not match compiled dataset class count");
        }
        const std::size_t stored_queries =
            checked_cast<std::size_t>(resume_checkpoint.metadata.num_queries, "resume checkpoint query count exceeds size_t");
        if (dataset_limits.requested_override && requested_query_limit.as_size != stored_queries) {
            throw std::runtime_error("resume preserves checkpoint query count " + std::to_string(stored_queries) +
                                     "; requested query override resolves to " + std::to_string(requested_query_limit.as_size));
        }
        artifacts.config.num_queries = checked_cast<int>(stored_queries, "resume query count exceeds int");
        artifacts.config.num_select = checked_cast<int>(resume_checkpoint.metadata.num_select, "resume selection count exceeds int");
        dataset_limits.resolved_num_queries = stored_queries;
        dataset_limits.query_source = "resume";
        dataset_limits.automatic = false;
    } else {
        artifacts.config.num_queries = requested_query_limit.as_int;
        artifacts.config.num_select = requested_query_limit.as_int;
        dataset_limits.resolved_num_queries = requested_query_limit.as_size;
        dataset_limits.query_source = requested_query_limit.automatic ? "automatic" : "explicit";
        dataset_limits.automatic = requested_query_limit.automatic;
    }
    artifacts.config.num_classes = dataset_output_classes;
    artifacts.class_layout = native_training_class_layout(*train_loader.class_catalog());
    if (!training_supervision_model_config_valid(artifacts.config)) {
        throw std::runtime_error("resolved RF-DETR model is incompatible with the requested training supervision");
    }
    if (training_supervision_enabled(artifacts.config.training_supervision) &&
        !training_supervision_query_layout_valid(
            artifacts.config.training_supervision, static_cast<std::size_t>(artifacts.config.num_queries),
            static_cast<std::size_t>(artifacts.config.group_detr), static_cast<std::size_t>(dataset_limits.largest_max_instances))) {
        throw std::runtime_error("resolved RF-DETR training supervision query capacity is unsafe");
    }
    mmltk::common::logging::profile_set_value("rfdetr.train.dataset.train_max_instances", dataset_limits.train_max_instances);
    mmltk::common::logging::profile_set_value("rfdetr.train.dataset.val_max_instances", dataset_limits.val_max_instances);
    mmltk::common::logging::profile_set_value("rfdetr.train.dataset.test_max_instances", dataset_limits.test_max_instances.value_or(0U));
    mmltk::common::logging::profile_set_value("rfdetr.train.dataset.largest_max_instances", dataset_limits.largest_max_instances);
    mmltk::common::logging::profile_set_value("rfdetr.train.resolved_num_queries", dataset_limits.resolved_num_queries);
    mmltk::common::logging::profile_set_value("rfdetr.train.required_num_queries", dataset_limits.required_num_queries);
    mmltk::common::logging::profile_set_value("rfdetr.train.automatic_num_queries_cap", dataset_limits.automatic_num_queries_cap);
    mmltk::common::logging::profile_set_value("rfdetr.train.query_override", dataset_limits.requested_override ? 1U : 0U);

    const auto validate_loader = [&](const mmltk::backend::data::DatasetLoader& loader, const char* split) {
        if (loader.image_width() != static_cast<uint32_t>(artifacts.config.resolution) ||
            loader.image_height() != static_cast<uint32_t>(artifacts.config.resolution)) {
            throw std::runtime_error(std::string(split) + " compiled resolution does not match RF-DETR input size");
        }
        if (!loader.class_catalog()->ordered_equal(*train_loader.class_catalog())) {
            throw std::runtime_error("ordered compiled class catalog mismatch across train/val/test splits");
        }
    };
    if (!val_loader && !mmltk::backend::data::inspect_compiled_dataset(options.val_compiled_path).class_catalog->ordered_equal(*train_loader.class_catalog()))
        throw std::runtime_error("ordered validation class catalog mismatch");
    validate_loader(train_loader, "train");
    if (val_loader) { validate_loader(*val_loader, "val"); }
    if (test_info.has_value()) {
        if (test_info->width != static_cast<std::uint32_t>(artifacts.config.resolution) ||
            test_info->height != static_cast<std::uint32_t>(artifacts.config.resolution)) {
            throw std::runtime_error("test compiled resolution does not match RF-DETR input size");
        }
        if (!test_info->class_catalog->ordered_equal(*train_loader.class_catalog())) {
            throw std::runtime_error("ordered compiled class catalog mismatch across train/val/test splits");
        }
    }

    NativeRfDetrModel model(artifacts.config, artifacts.class_layout);
    detail::native_model_owner(model).initialize_training_supervision(static_cast<std::uint64_t>(options.seed));
    detail::native_model_owner(model).module().to(cuda_device(options.device_id));
    detail::native_model_owner(model).configure_supervision_timing(
        SupervisionTimingSetup{cuda_device(options.device_id), static_cast<std::size_t>(std::max(1, options.grad_accum_steps)),
                               mmltk::common::logging::profile_enabled()});
    const auto resolved_route = supervision_route(options.training_supervision);
    std::optional<detail::NormalizedModelStateCandidate> resume_model_candidate;
    ModelStateLoadSummary load_summary;
    if (!options.resume_path.empty()) {
        resume_model_candidate = detail::native_model_owner(model).stage_normalized_state(
            detail::model_state_owner(admitted.model_state).entries, detail::NormalizedModelStateAdmission::Exact);
        load_summary = resume_model_candidate->summary;
    } else {
        load_summary = load_training_model_weights(model, admitted.model_state, resolved_route);
    }
    model.optimize_for_inference(checked_inference_batch_size(options.batch_size), true, options.compilation_mode);
    if (!options.val_compiled_path.empty()) {
        model.optimize_for_inference(checked_inference_batch_size(val_batch_size), false, options.compilation_mode);
    }
    if (main_process) {
        mmltk::common::logging::info([&](auto& logger) {
            logger.info("rfdetr weights: loaded={} missing={} unexpected={} incompatible={} input={}", load_summary.loaded_names.size(),
                        load_summary.missing_names.size(), load_summary.unexpected_names.size(), load_summary.incompatible_names.size(),
                        source_checkpoint.string());
        });
        for (const auto& name : load_summary.missing_names) {
            mmltk::common::logging::warn([&](auto& logger) { logger.warn("  missing: {}", name); });
        }
        for (const auto& name : load_summary.unexpected_names) {
            mmltk::common::logging::warn([&](auto& logger) { logger.warn("  unexpected: {}", name); });
        }
    }
    ensure_train_lane_model_supported(model, train_lane_count);
    detail::native_model_owner(model).module().train();

    if (options.freeze_encoder) {
        for (auto& item : detail::native_model_owner(model).module().named_parameters(true)) {
            if (is_encoder_param(item.key())) { item.value().set_requires_grad(false); }
        }
    }

    auto optimizer_build = build_optimizer(model, options);
    auto& optimizer = optimizer_build.optimizer;
    auto& all_params = optimizer.parameters();
    const auto& all_param_names = optimizer.parameter_names();

    const bool amp_enabled = options.amp;
    const auto autocast_dtype = amp_enabled ? resolve_cuda_autocast_dtype() : torch_api::kFloat;
    const bool scaler_enabled = amp_enabled && autocast_dtype == torch_api::kFloat16;
    GradScaler grad_scaler(scaler_enabled);
    std::optional<ModelEma> ema;
    if (options.resume_path.empty() && options.use_ema && main_process) {
        ema.emplace(all_params, options.ema_decay, static_cast<double>(options.ema_tau));
    }

    int start_epoch = 0;
    std::string resume_attempt_id;
    double best_regular = -std::numeric_limits<double>::infinity();
    double best_ema = -std::numeric_limits<double>::infinity();
    if (!options.resume_path.empty()) {
        ResumeState resume_state =
            load_resume_checkpoint_state(options.resume_path, admitted.model_state, optimizer, options, all_param_names, all_params, main_process);
        if (resume_model_candidate.has_value()) {
            detail::native_model_owner(model).commit_normalized_state(std::move(*resume_model_candidate));
        }
        if (!resume_state.optimizer_candidate.has_value()) {
            throw std::logic_error("admitted RF-DETR resume is missing its optimizer candidate");
        }
        optimizer.commit(std::move(*resume_state.optimizer_candidate));
        if (resume_state.scaler_scale.has_value()) {
            grad_scaler.load_state(*resume_state.scaler_scale, *resume_state.scaler_growth_tracker);
        }
        if (resume_state.restored_ema.has_value()) { ema = std::move(resume_state.restored_ema); }
        start_epoch = resume_state.start_epoch;
        resume_attempt_id = std::move(resume_state.attempt_id);
        best_regular = resume_state.best_regular;
        best_ema = resume_state.best_ema;
    }

    detail::model_state_owner(admitted.model_state).entries.clear();
    detail::model_state_owner(admitted.model_state).native_archive.reset();

    LrScheduleConfig lr_config;
    lr_config.warmup_epochs = options.warmup_epochs;
    lr_config.warmup_momentum = options.warmup_momentum;
    lr_config.lr_scheduler = options.lr_scheduler;
    lr_config.lr_drop = options.lr_drop;
    lr_config.lr_min_factor = options.lr_min_factor;

    if (main_process) {
        mmltk::common::logging::info([&](auto& logger) {
            logger.info(
                "rfdetr train runtime: torch={} autocast={} optimizer={} optimizer_backend={} scaler={} train_lanes={} "
                "eval_lanes={} loader_threads={} gather_threads={} cpu_threads={} effective_batch_per_rank={} "
                "effective_batch_global={} train_max_instances={} val_max_instances={} test_max_instances={} "
                "query_source={} num_queries={} automatic_query_cap={} query_override={}",
                torch_api::kVersion, evaluation_precision_name(autocast_dtype), optimizer.kind_name(), optimizer.backend_name(),
                grad_scaler.enabled() ? "on" : "off", train_lane_count, train_runtime.split().lane_threads,
                train_runtime.split().loader_threads, train_runtime.split().gather_threads, train_runtime.split().cpu_threads,
                effective_batch_per_rank(options, train_lane_count), effective_batch_global(options, distributed, train_lane_count),
                dataset_limits.train_max_instances, dataset_limits.val_max_instances, dataset_limits.test_max_instances.value_or(0U),
                dataset_limits.query_source, dataset_limits.resolved_num_queries, dataset_limits.automatic_num_queries_cap,
                dataset_limits.requested_override ? "true" : "false");
        });
        if (options.optimizer == TrainOptimizerKind::Muon && options.fused_optimizer) {
            mmltk::common::logging::warn([](auto& logger) {
                logger.warn(
                    "rfdetr train runtime: optimizer=muon ignores --fused-optimizer and runs with the eager backend "
                    "only");
            });
        }
    }

    const size_t usable_full_batches =
        full_batches_per_rank(train_loader.num_images(), options.batch_size, static_cast<size_t>(distributed.world_size),
                              options.grad_accum_steps, train_lane_count);
    if (usable_full_batches == 0) {
        throw std::runtime_error(
            "compiled train split is too small for one effective batch per rank; reduce batch_size, "
            "grad_accum_steps, --lanes, or world size");
    }
    const size_t batches_per_step = micro_batches_per_optimizer_step(options, train_lane_count);
    const auto steps_per_epoch = static_cast<int64_t>(usable_full_batches / batches_per_step);
    const int64_t total_training_steps = std::max<int64_t>(1, steps_per_epoch * options.epochs);
    DetectionConfig detection_config = make_detection_config(artifacts.config, distributed.world_size, options.compilation_mode);
    const TrainingSupervisionRoute training_route = supervision_route(artifacts.config.training_supervision);

    if (main_process) { std::filesystem::create_directories(options.output_dir); }
    const auto checkpoint_path = options.output_dir / "checkpoint.pt";
    const auto best_regular_checkpoint_path = options.output_dir / "checkpoint_best_regular.pt";
    const auto best_ema_checkpoint_path = options.output_dir / "checkpoint_best_ema.pt";
    auto metadata = checkpoint_metadata(artifacts, dataset_output_classes);
    if (!options.resume_path.empty()) {
        metadata.source_path = admitted.model_state.metadata.source_path;
        metadata.source_kind = admitted.model_state.metadata.source_kind;
    }

    auto checkpoint_configuration = options;
    checkpoint_configuration.resolution = artifacts.config.resolution;
    checkpoint_configuration.preset_name = artifacts.config.preset_name;
    torch_cuda::TensorReadbackBuffers checkpoint_readback;
    TrainRunResult result;
    result.artifacts = artifacts;
    result.gpu_augmentation = options.gpu_augmentation;
    result.output_dir = options.output_dir;
    result.checkpoint_path = checkpoint_path;
    result.best_regular_checkpoint_path = best_regular_checkpoint_path;
    result.best_ema_checkpoint_path = best_ema_checkpoint_path;
    if (main_process && !options.use_ema && std::isfinite(best_regular) && std::filesystem::exists(best_regular_checkpoint_path)) {
        result.best_checkpoint_path = best_regular_checkpoint_path;
        result.best_is_ema = false;
    }
    if (main_process && options.use_ema && std::isfinite(best_ema) && std::filesystem::exists(best_ema_checkpoint_path)) {
        result.best_checkpoint_path = best_ema_checkpoint_path;
        result.best_is_ema = true;
    }
    std::unique_ptr<TrainingTelemetryWriter> progress_writer;
    TrainingMetricProgress last_training_progress;
    if (main_process) {
        TrainingRun run;
        run.configuration = checkpoint_configuration;
        run.original_weights = metadata.source_path;
        run.original_class_descriptor = original_descriptor;
        run.execution = {train_runtime.split().lane_threads, effective_batch_per_rank(options, train_lane_count),
                         effective_batch_global(options, distributed, train_lane_count), dataset_limits};
        run.class_layout = artifacts.class_layout;
        run.evaluated_weights = options.use_ema ? EvaluatedWeights::Ema : EvaluatedWeights::Ordinary;
        run.source_checkpoint_attempt_id = resume_attempt_id;
        run.resume_epoch = start_epoch - 1;
        run.resume_optimizer_step = static_cast<std::uint64_t>(start_epoch) * static_cast<std::uint64_t>(steps_per_epoch);
        progress_writer = std::make_unique<TrainingTelemetryWriter>(std::move(run));
        last_training_progress.epoch = start_epoch;
        last_training_progress.total_epochs = options.epochs;
        last_training_progress.total_batches = usable_full_batches;
        last_training_progress.steps_per_epoch = steps_per_epoch;
        last_training_progress.train_lanes = train_lane_count;
        progress_writer->Submit(last_training_progress, TrainingRecordRole::Boundary);
    }

    torch_cuda::TorchCudaDeviceGuard device_guard(cuda_device_index(options.device_id));
    std::unique_ptr<TrainingValidationRuntime> validation_runtime;
    if (main_process) {
        validation_runtime = std::make_unique<TrainingValidationRuntime>(
            options, train_runtime, std::move(val_loader), val_batch_size, options.validation_loss,
            detection_config.include_masks ? EvaluationMetricSet::BBoxAndMask : EvaluationMetricSet::BBox, artifacts.config.num_select,
            "val", dataset_limits.automatic);
    }
    TrainingMetricHandoff metric_handoff(options.device_id);
    std::unique_ptr<mmltk::common::concurrency::WorkerPool> train_lane_pool;
    std::deque<TrainLaneContext> train_lanes;
    std::unique_ptr<GpuBatchAugmenter> single_lane_augmenter;
    TargetScratch target_scratch(static_cast<std::size_t>(std::max(1, options.grad_accum_steps)));
    if (train_lane_count <= 1) {
        single_lane_augmenter = std::make_unique<GpuBatchAugmenter>(options.gpu_augmentation, static_cast<std::int64_t>(options.batch_size),
                                                                    static_cast<int>(train_loader.image_height()),
                                                                    static_cast<int>(train_loader.image_width()), options.device_id);
    }
    if (train_lane_count > 1) {
        train_lane_pool =
            std::make_unique<mmltk::common::concurrency::WorkerPool>(static_cast<size_t>(train_lane_count), train_runtime.lane_cpus(),
                                                                     "rfdtrtlane", 0U, &train_runtime.execution().placement, false);
        for (int lane_index = 0; lane_index < train_lane_count; ++lane_index) {
            train_lanes.emplace_back(
                torch_cuda::get_priority_cuda_stream(options.device_id, mmltk::frameworks::gpu::current_cuda_highest_stream_priority()),
                static_cast<std::size_t>(std::max(1, options.grad_accum_steps)));
        }
        for (auto& lane : train_lanes) {
            lane.augmenter = std::make_unique<GpuBatchAugmenter>(options.gpu_augmentation, static_cast<std::int64_t>(options.batch_size),
                                                                 static_cast<int>(train_loader.image_height()),
                                                                 static_cast<int>(train_loader.image_width()), options.device_id);
            lane.model = make_train_lane_model(model, options.device_id);
            detail::native_model_owner(*lane.model)
                .configure_supervision_timing(SupervisionTimingSetup{cuda_device(options.device_id),
                                                                     static_cast<std::size_t>(std::max(1, options.grad_accum_steps)),
                                                                     mmltk::common::logging::profile_enabled()});
            lane.model->optimize_for_inference(checked_inference_batch_size(options.batch_size), true, options.compilation_mode);
            lane.grad_params = lane_grad_parameters(*lane.model, all_param_names);
        }
    }
    size_t parameter_version = 0;
    distributed_barrier(distributed);
    for (int epoch = start_epoch; epoch < options.epochs; ++epoch) {
        mmltk::common::logging::ScopedProfile profile_rfdetr_train_epoch{"rfdetr.train.epoch"};
        result.last_epoch = epoch;
        train_loader.begin_epoch();
        detail::native_model_owner(model).module().train();
        optimizer.zero_grad(true);
        metric_handoff.reset_epoch();
        last_training_progress.scalars = {};
        const auto epoch_started = std::chrono::steady_clock::now();
        int64_t local_micro_batches = 0;
        int64_t reported_micro_batches = 0;
        int64_t local_waves = 0;
        int64_t optimizer_steps = 0;
        size_t local_full_batches = 0;
        double host_loss_sum = 0.0;
        double host_class_loss_sum = 0.0;
        double host_box_loss_sum = 0.0;
        std::optional<double> epoch_global_loss;
        double current_step_loss = 0.0;
        double current_step_class_loss = 0.0;
        double current_step_box_loss = 0.0;
        auto last_progress_submit = epoch_started - std::chrono::seconds(1);

        std::unique_ptr<spdmon::ProgressBar> progress;
        if (main_process && options.progress_bar) {
            progress = std::make_unique<spdmon::ProgressBar>(
                phase_progress_label("train", epoch, options.epochs),
                static_cast<size_t>(usable_full_batches) * static_cast<size_t>(options.batch_size), "img");
            progress->set_postfix("cl=warming, bl=warming, l=warming");
        }

        auto write_progress_snapshot = [&](TrainingPhase phase, std::optional<double> val_loss, const std::optional<EvalSummary>& val_summary,
                                           const std::filesystem::path& checkpoint_override, bool force) {
            if (!main_process) { return; }
            const double elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch_started).count();
            const double average_class_loss =
                reported_micro_batches > 0 ? host_class_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double average_box_loss =
                reported_micro_batches > 0 ? host_box_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double average_loss = reported_micro_batches > 0 ? host_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double batches_per_second = elapsed_seconds > 0.0 ? static_cast<double>(local_micro_batches) / elapsed_seconds : 0.0;
            const double images_per_second =
                elapsed_seconds > 0.0 ? static_cast<double>(local_micro_batches) * static_cast<double>(options.batch_size) / elapsed_seconds
                                      : 0.0;
            TrainingMetricProgress snapshot;
            snapshot.phase = phase;
            snapshot.epoch = epoch;
            snapshot.total_epochs = options.epochs;
            snapshot.completed_batches = local_micro_batches;
            snapshot.total_batches = usable_full_batches;
            snapshot.completed_waves = local_waves;
            snapshot.optimizer_steps = optimizer_steps;
            snapshot.global_optimizer_step = static_cast<std::int64_t>(epoch) * steps_per_epoch + optimizer_steps;
            snapshot.epoch_global_loss = epoch_global_loss;
            snapshot.steps_per_epoch = steps_per_epoch;
            snapshot.train_lanes = train_lane_count;
            snapshot.train_loss = average_loss;
            snapshot.class_loss = average_class_loss;
            snapshot.box_loss = average_box_loss;
            snapshot.step_loss = current_step_loss;
            snapshot.step_class_loss = current_step_class_loss;
            snapshot.step_box_loss = current_step_box_loss;
            snapshot.batches_per_second = batches_per_second;
            snapshot.images_per_second = images_per_second;
            snapshot.elapsed_seconds = elapsed_seconds;
            snapshot.checkpoint_path = checkpoint_override;
            snapshot.full_checkpoint_path = phase == TrainingPhase::EpochComplete ? checkpoint_path : last_training_progress.full_checkpoint_path;
            snapshot.val_loss = val_loss;
            snapshot.val = val_summary;
            snapshot.scalars = last_training_progress.scalars;
            snapshot.scalars.total = reported_micro_batches > 0 ? std::optional<double>{average_loss} : std::nullopt;
            snapshot.scalars.images_per_second = images_per_second;
            last_training_progress = snapshot;
            progress_writer->Submit(std::move(snapshot), phase == TrainingPhase::EpochComplete ? TrainingRecordRole::Epoch :
                                    (force ? TrainingRecordRole::Boundary : TrainingRecordRole::Live));
        };

        auto flush_progress = [&](bool force) {
            if (local_micro_batches == 0) { return; }
            const double average_class_loss =
                reported_micro_batches > 0 ? host_class_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double average_box_loss =
                reported_micro_batches > 0 ? host_box_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double average_loss = reported_micro_batches > 0 ? host_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch_started).count();
            const double images_per_second =
                elapsed_seconds > 0.0 ? static_cast<double>(local_micro_batches) * static_cast<double>(options.batch_size) / elapsed_seconds
                                      : 0.0;
            const bool should_update_bar =
                static_cast<bool>(progress) && (force || local_micro_batches % std::max(1, options.print_freq) == 0);
            if (should_update_bar) {
                progress->set_postfix(train_progress_postfix(average_class_loss, average_box_loss, average_loss, current_step_class_loss,
                                                             current_step_box_loss, current_step_loss, images_per_second, optimizer_steps,
                                                             steps_per_epoch));
            }
            const auto now = std::chrono::steady_clock::now();
            if (force || now - last_progress_submit >= std::chrono::seconds(1)) {
                write_progress_snapshot(TrainingPhase::Train, std::nullopt, {}, std::filesystem::path{}, force);
                last_progress_submit = now;
            }
        };

        write_progress_snapshot(TrainingPhase::Train, std::nullopt, {}, std::filesystem::path{}, true);
        last_progress_submit = std::chrono::steady_clock::now();

        const auto apply_optimizer_schedule = [&](int64_t current_step) {
            const double lr_scale = compute_lr_scale(lr_config, current_step, steps_per_epoch, total_training_steps);
            set_optimizer_lrs(optimizer, optimizer_build.base_lrs, lr_scale);
            if (!optimizer_build.base_lrs.empty()) {
                const auto [minimum, maximum] = std::minmax_element(optimizer_build.base_lrs.begin(), optimizer_build.base_lrs.end());
                last_training_progress.scalars.learning_rate = optimizer_build.base_lrs.front() * lr_scale;
                last_training_progress.scalars.learning_rate_min = *minimum * lr_scale;
                last_training_progress.scalars.learning_rate_max = *maximum * lr_scale;
            }
            if (options.optimizer == TrainOptimizerKind::Muon) {
                optimizer.set_muon_momentum(compute_warmup_momentum(lr_config, current_step, steps_per_epoch, options.momentum));
            }
        };
        const auto run_optimizer_step = [&](const auto& post_step, const TensorMap* loss_report, int64_t wave_micro_batches) {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_optimizer{"rfdetr.train.optimizer"};
            const int64_t current_step = static_cast<int64_t>(epoch) * steps_per_epoch + optimizer_steps;
            apply_optimizer_schedule(current_step);
            average_gradients(distributed, all_params);
            const auto found_inf = grad_scaler.check_and_unscale_(optimizer);
            if (options.clip_max_norm > 0.0) { torch_api::clip_grad_norm_(all_params, options.clip_max_norm); }

            const TrainingMetricSnapshot metrics = metric_handoff.complete_step(found_inf, wave_micro_batches, local_micro_batches);
            auto scalars = metrics.scalars;
            scalars.learning_rate = last_training_progress.scalars.learning_rate;
            scalars.learning_rate_min = last_training_progress.scalars.learning_rate_min;
            scalars.learning_rate_max = last_training_progress.scalars.learning_rate_max;
            last_training_progress.scalars = std::move(scalars);
            host_loss_sum = metrics.loss_sum;
            host_class_loss_sum = metrics.class_loss_sum;
            host_box_loss_sum = metrics.box_loss_sum;
            current_step_loss = metrics.step_loss;
            current_step_class_loss = metrics.step_class_loss;
            current_step_box_loss = metrics.step_box_loss;
            reported_micro_batches = local_micro_batches;

            const bool gradient_overflow = !metrics.gradients_finite;
            if (!metrics.loss_finite || (gradient_overflow && !grad_scaler.enabled())) {
                const TensorMap empty_loss_report;
                const auto& report = loss_report != nullptr ? *loss_report : empty_loss_report;
                const std::string details = format_nonfinite_loss_report(report, all_params, all_param_names);
                grad_scaler.update(gradient_overflow);
                optimizer.zero_grad(true);
                throw std::runtime_error("non-finite RF-DETR loss or gradients encountered during native training" + details);
            }

            std::string overflow_details;
            if (gradient_overflow) {
                const TensorMap empty_loss_report;
                const auto& report = loss_report != nullptr ? *loss_report : empty_loss_report;
                overflow_details = format_nonfinite_loss_report(report, all_params, all_param_names);
            }
            grad_scaler.step(optimizer, gradient_overflow);
            grad_scaler.update(gradient_overflow);
            optimizer.zero_grad(true);
            if (ema.has_value() && !gradient_overflow) { ema->update(optimizer_steps); }
            if (gradient_overflow) {
                mmltk::common::logging::warn(
                    [&](auto& logger) { logger.warn("skipped RF-DETR optimizer update after gradient overflow{}", overflow_details); });
            }
            ++optimizer_steps;
            post_step();
        };

        auto next_train_full_batch = [&]() -> std::optional<mmltk::backend::data::Batch> {
            mmltk::backend::data::Batch batch{};
            while (train_loader.next_batch(batch)) {
                mmltk::common::logging::ScopedProfile profile_rfdetr_train_batch{"rfdetr.train.batch"};
                if (batch.num_images != options.batch_size) {
                    train_loader.release_batch(batch);
                    continue;
                }
                if (local_full_batches >= usable_full_batches) {
                    train_loader.release_batch(batch);
                    return std::nullopt;
                }
                ++local_full_batches;
                mmltk::common::logging::profile_add_value("rfdetr.train.images", batch.num_images);
                mmltk::common::logging::profile_add_value("rfdetr.train.full_batches", 1);
                return batch;
            }
            return std::nullopt;
        };

        std::optional<mmltk::backend::ml::cuda::CudaEventPool::Lease> params_ready;
        if (train_lane_count > 1) {
            params_ready =
                record_current_stream_event(training_events.pool(), options.device_id, "record parallel train parameter readiness");
            if (!params_ready) throw std::runtime_error("training CUDA event capacity is exhausted");
        }

        if (train_lane_count <= 1) {
            while (true) {
                auto batch = next_train_full_batch();
                if (!batch.has_value()) { break; }
                metric_handoff.begin_wave();

                LoaderBatchGuard batch_guard(train_loader, *batch, options.device_id);
                torch_api::Tensor normalized;
                {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_augment{"rfdetr.train.augment"};
                    mmltk::common::logging::ScopedProfile profile_benchmark_rfdetr_train_augmentation{
                        "benchmark.rfdetr.train.augmentation"};
                    normalized = single_lane_augmenter->run(*batch, static_cast<std::uint64_t>(options.seed), epoch, distributed.rank,
                                                            local_full_batches - 1);
                }

                torch_api::Tensor loss;
                torch_api::Tensor class_loss;
                torch_api::Tensor box_loss;
                scalar_packet::Tensors scalar_values;
                TensorMap loss_dict;
                auto& model_owner = detail::native_model_owner(model);
                PreparedTargets prepared;
                {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_targets{"rfdetr.train.targets"};
                    prepared =
                        build_targets(*batch, static_cast<int>(train_loader.image_height()), static_cast<int>(train_loader.image_width()),
                                      detection_config.include_masks, detection_config.include_masks, options.device_id, target_scratch,
                                      "train", artifacts.config.num_queries, artifacts.config.training_supervision,
                                      static_cast<int>(model.class_layout()->catalog()->size()), &single_lane_augmenter->batch_plan());
                }
                batch_guard.set_consumer_stream(single_lane_augmenter->prepare_batch_consumer());
                static_cast<void>(single_lane_augmenter->finish_batch(*batch));
                batch_guard.release();
                std::optional<DeviceLossNormalizer> active_normalizer;
                if (route_is_active(training_route)) {
                    auto target_count =
                        torch_api::tensor({static_cast<float>(prepared_target_count(prepared))},
                                          torch_api::TensorOptions().dtype(torch_api::kFloat32).device(normalized.device()));
                    distributed_all_reduce_tensor(distributed, target_count);
                    target_count.div_(static_cast<double>(std::max(1, distributed.world_size)));
                    active_normalizer = DeviceLossNormalizer{target_count.select(0, 0)};
                }
                SupervisionTimingLease step_timing(model_owner, route_is_active(training_route), SupervisionTimingLease::Kind::Step);

                mmltk::backend::ml::cuda::TorchAutocastScope autocast_guard(amp_enabled, autocast_dtype);
                TargetConsumerLease target_consumer(target_scratch, prepared, options.device_id);
                ModelOutputs outputs;
                if (route_is_active(training_route)) {
                    if (route_uses_denoising(training_route)) {
                        mmltk::common::logging::ScopedProfile profile_rfdetr_train_targets_handoff{"rfdetr.train.targets_handoff"};
                        target_consumer.handoff();
                        outputs = detail::native_model_owner(model).forward_with_denoising(
                            NestedTensor{normalized, prepared.nested_mask}, prepared,
                            TrainingStepIdentity{static_cast<std::uint64_t>(options.seed), static_cast<std::uint64_t>(epoch),
                                                 static_cast<std::uint32_t>(distributed.rank), local_full_batches - 1});
                    } else {
                        mmltk::common::logging::ScopedProfile profile_rfdetr_train_forward{"rfdetr.train.forward"};
                        outputs = detail::native_model_owner(model).forward_for_match_free(NestedTensor{normalized, prepared.nested_mask});
                        target_consumer.handoff();
                    }
                    SupervisionTimingLease criterion_timing(model_owner, true, SupervisionTimingLease::Kind::Criterion);
                    auto routed =
                        compute_routed_training_loss(model, training_route, outputs, prepared, *active_normalizer, detection_config);
                    criterion_timing.finish();
                    loss = std::move(routed.total);
                    class_loss = std::move(routed.classification);
                    box_loss = std::move(routed.box);
                    loss_dict = std::move(routed.ordinary_terms);
                    scalar_values = std::move(routed.scalars);
                } else {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_forward{"rfdetr.train.forward"};
                    outputs = detail::native_model_owner(model).forward(NestedTensor{normalized, prepared.nested_mask}, true);
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_targets_handoff{"rfdetr.train.targets_handoff"};
                    target_consumer.handoff();
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_loss_dict{"rfdetr.train.loss_dict"};
                    loss_dict = detection_loss_dict(outputs, prepared, detection_config, true, distributed.enabled,
                                                    distributed.enabled ? AllReduceTensorFn([&distributed](torch_api::Tensor& value) {
                                                        distributed_all_reduce_tensor(distributed, value);
                                                    })
                                                                        : AllReduceTensorFn{});
                }
                if (!route_is_active(training_route)) {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_loss_total{"rfdetr.train.loss_total"};
                    torch_api::Tensor auxiliary;
                    loss = weighted_detection_loss(loss_dict, detection_config, normalized.device(), &auxiliary);
                    scalar_values = ordinary_scalar_tensors(loss_dict, loss, auxiliary);
                    class_loss = loss_value_or_zero(loss_dict, normalized.device(), "loss_ce");
                    box_loss = loss_value_or_zero(loss_dict, normalized.device(), "loss_bbox");
                }
                metric_handoff.accumulate(loss, class_loss, box_loss, scalar_values);
                ++local_micro_batches;
                ++local_waves;
                const auto scaled_loss = grad_scaler.scale(loss.div(static_cast<double>(options.grad_accum_steps)));
                {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_backward{"rfdetr.train.backward"};
                    scaled_loss.backward();
                    train_runtime.matcher_workspace().complete_assignments(
                        torch_cuda::getCurrentCUDAStream(torch_cuda::checked_device_index(options.device_id)).stream());
                }
                target_consumer.retire();
                step_timing.finish();

                if (local_waves % options.grad_accum_steps == 0) {
                    run_optimizer_step([] {}, &loss_dict, 1);
                    static_cast<void>(detail::native_model_owner(model).harvest_supervision_timing());
                }

                if (progress) { progress->add(static_cast<size_t>(options.batch_size)); }
                flush_progress(false);
            }
        } else {
            while (local_full_batches < usable_full_batches) {
                metric_handoff.begin_wave();
                const double current_scale = grad_scaler.enabled() ? static_cast<double>(grad_scaler.current_scale()) : 1.0;
                const double scaled_loss_factor =
                    current_scale / static_cast<double>(micro_batches_per_optimizer_step(options, train_lane_count));
                ParallelTrainingWave<TrainLaneResult> wave(static_cast<std::size_t>(train_lane_count),
                                                           route_is_active(training_route) || distributed.enabled,
                                                           options.device_id, distributed);

                for (int lane_index = 0; lane_index < train_lane_count; ++lane_index) {
                    auto batch = next_train_full_batch();
                    if (!batch.has_value()) {
                        throw std::runtime_error("native RF-DETR training ended an epoch with an incomplete parallel train wave");
                    }

                    auto& lane = train_lanes[static_cast<size_t>(lane_index)];

                    wave.add(enqueue_train_lane(
                        *train_lane_pool, &train_runtime, train_loader, lane, *batch, params_ready ? &*params_ready : nullptr,
                        training_events.pool(), scaled_loss_factor, parameter_version, detection_config, model,
                        options.device_id, static_cast<int>(train_loader.image_height()), static_cast<int>(train_loader.image_width()),
                        static_cast<std::uint64_t>(options.seed), epoch, distributed.rank, local_full_batches - 1, amp_enabled,
                        autocast_dtype, training_route, wave.normalizer(), static_cast<std::size_t>(lane_index)));
                }

                wave.settle(cuda_device(options.device_id), [&](TrainLaneResult& lane_result) {
                    merge_lane_gradients(lane_result, all_params, options.device_id);
                    metric_handoff.accumulate(lane_result.loss, lane_result.class_loss, lane_result.box_loss, lane_result.scalars);
                    ++local_micro_batches;
                    if (progress) { progress->add(static_cast<size_t>(options.batch_size)); }
                });

                ++local_waves;
                if (local_waves % options.grad_accum_steps == 0) {
                    run_optimizer_step(
                        [&] {
                            ++parameter_version;
                            if (params_ready) params_ready->retire();
                            params_ready = record_current_stream_event(training_events.pool(), options.device_id,
                                                                       "record parallel train parameter readiness");
                            if (!params_ready) throw std::runtime_error("training CUDA event capacity is exhausted");
                        },
                        nullptr, static_cast<int64_t>(train_lane_count));
                    for (auto& lane : train_lanes) {
                        static_cast<void>(detail::native_model_owner(*lane.model).harvest_supervision_timing());
                    }
                }
                flush_progress(false);
            }
        }

        if (params_ready) params_ready->retire();

        {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_drain_loader{"rfdetr.train.drain_loader"};
            mmltk::backend::data::Batch drain_batch{};
            while (train_loader.next_batch(drain_batch)) {
                train_loader.release_batch(drain_batch);
            }
        }

        if (local_micro_batches == 0 || local_waves % options.grad_accum_steps != 0) {
            throw std::runtime_error("native RF-DETR training ended an epoch with incomplete gradient accumulation");
        }
        if (train_lane_count <= 1) {
            {
                mmltk::common::logging::ScopedProfile profile_rfdetr_train_wait_pending_copy{"rfdetr.train.wait_pending_copy"};
                target_scratch.wait_for_pending_copy();
            }
        } else {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_wait_pending_copy{
                "rfdetr.train.parallel.wait_pending_copy"};
            for (auto& lane : train_lanes) {
                lane.target_scratch.wait_for_pending_copy();
            }
        }
        flush_progress(true);
        if (progress) { progress->close(); }

        auto reduced_loss_sum = metric_handoff.loss_sum();
        auto reduced_micro_batches = metric_handoff.epoch_count(local_micro_batches);
        if (distributed.enabled) {
            distributed_all_reduce_tensor(distributed, reduced_loss_sum);
            distributed_all_reduce_tensor(distributed, reduced_micro_batches);
        }
        const double train_loss = metric_handoff.epoch_average();
        epoch_global_loss = train_loss;
        distributed_barrier(distributed);

        if (main_process) {
            write_progress_snapshot(TrainingPhase::Validate, std::nullopt, {}, std::filesystem::path{}, true);
            EvalPassResult val_result;
            {
                std::optional<ModelEma::Selection> selected;
                if (ema) selected.emplace(*ema, detail::native_model_owner(model).module());
                val_result = evaluate_model(options, *validation_runtime, model, training_events, detection_config,
                                             options.validation_loss, epoch,
                                             phase_progress_label(ema ? "ema" : "val", epoch, options.epochs), &metric_handoff);
                if (selected) selected->restore();
                detail::native_model_owner(model).module().train();
            }

            if (val_result.loss.has_value()) {
                mmltk::common::logging::info([&](auto& logger) {
                    logger.info("epoch {} stats: optimizer={} train_loss={:.6f} val_loss={:.6f} bbox_ap={:.4f} mask_ap={}", epoch + 1,
                                optimizer.kind_name(), train_loss, *val_result.loss, val_result.summary.bbox.ap,
                                formatted_mask_ap(val_result.summary));
                });
            } else {
                mmltk::common::logging::info([&](auto& logger) {
                    logger.info("epoch {} stats: optimizer={} train_loss={:.6f} bbox_ap={:.4f} mask_ap={}", epoch + 1,
                                optimizer.kind_name(), train_loss, val_result.summary.bbox.ap, formatted_mask_ap(val_result.summary));
                });
            }

            checkpoint_readback.Begin();
            const auto ordinary_snapshot = collect_module_state(model);
            detail::reserve_state_archive(ordinary_snapshot, checkpoint_readback, 0);
            save_snapshot_checkpoint(options.output_dir / std::format("checkpoint_epoch_{}.pt", epoch + 1),
                                     metadata, ordinary_snapshot, {}, checkpoint_readback, options.class_layout_path);
            // Preparing later artifacts must not undo a successfully published epoch.
            const auto ema_snapshot = ema ? ema_state_entries(all_param_names, *ema) :
                                            std::vector<NormalizedModelStateEntry>{};
            detail::reserve_state_archive(ema_snapshot, checkpoint_readback, ordinary_snapshot.size());

            TrainEpochSummary epoch_summary;
            epoch_summary.epoch = epoch;
            epoch_summary.train_loss = train_loss;
            epoch_summary.val_loss = val_result.loss;
            epoch_summary.val_summary = val_result.summary;

            epoch_summary.evaluated_ema = options.use_ema;
            const double selected_metric = checkpoint_metric(val_result.summary, detection_config.include_masks);
            auto& best_metric = options.use_ema ? best_ema : best_regular;
            const auto& selected_path = options.use_ema ? best_ema_checkpoint_path : best_regular_checkpoint_path;
            if (selected_metric > best_metric) {
                save_snapshot_checkpoint(selected_path, metadata, ordinary_snapshot, ema_snapshot,
                                         checkpoint_readback, options.class_layout_path);
                best_metric = selected_metric;
                result.best_is_ema = options.use_ema;
                result.best_checkpoint_path = selected_path;
            }

            {
                mmltk::common::logging::ScopedProfile profile_rfdetr_train_save_resume{"rfdetr.train.save.resume"};
                save_resume_checkpoint(checkpoint_path, metadata, optimizer, grad_scaler, checkpoint_configuration, epoch, best_regular, best_ema,
                                       ordinary_snapshot, ema_snapshot, progress_writer->attempt_id(), original_descriptor, checkpoint_readback);
                checkpoint_readback.Release();
            }

            if (result.history.size() == result.history.capacity()) result.history.erase(result.history.begin());
            result.history.push_back(epoch_summary);
            ++result.completed_epochs;
            write_progress_snapshot(TrainingPhase::EpochComplete, val_result.loss, val_result.summary,
                                    result.best_checkpoint_path.has_value() ? *result.best_checkpoint_path : checkpoint_path, true);
        }
        distributed_barrier(distributed);
    }

    if (main_process && !result.best_checkpoint_path.has_value()) {
        const auto fallback_path = options.output_dir / (options.use_ema ? "checkpoint_fallback_ema.pt" : "checkpoint_fallback_regular.pt");
        const auto overrides = ema ? ema_override_map(all_param_names, *ema) : std::unordered_map<std::string, torch_api::Tensor>{};
        save_collected_checkpoint(fallback_path, metadata, model, ema ? &overrides : nullptr,
                                  "rfdetr.train.save.selected_fallback", "rfdetr.train.save.selected_fallback.collect_state",
                                  options.class_layout_path);
        result.best_checkpoint_path = fallback_path;
        result.best_is_ema = options.use_ema;
        result.best_is_fallback = true;
    }

    if (main_process && !options.test_compiled_path.empty()) {
        auto test_config = make_loader_config_for(options.test_compiled_path, val_batch_size, false, options.prefetch_factor, false, false);
        auto test_loader = std::make_unique<mmltk::backend::data::DatasetLoader>(test_config);
        validate_loader(*test_loader, "test");
        TrainingValidationRuntime test_runtime(
            options, train_runtime, std::move(test_loader), val_batch_size, false,
            detection_config.include_masks ? EvaluationMetricSet::BBoxAndMask : EvaluationMetricSet::BBox, artifacts.config.num_select,
            "test", dataset_limits.automatic);
        NativeRfDetrModel best_model(artifacts.config, artifacts.class_layout);
        detail::native_model_owner(best_model).module().to(cuda_device(options.device_id));
        load_model_weights(best_model, *result.best_checkpoint_path, false);
        best_model.optimize_for_inference(checked_inference_batch_size(val_batch_size), false, options.compilation_mode);
        result.test_summary =
            evaluate_model(options, test_runtime, best_model, training_events, detection_config, false, std::nullopt, "test").summary;
    }

    if (main_process) {
        last_training_progress.phase = TrainingPhase::Completed;
        last_training_progress.checkpoint_path = *result.best_checkpoint_path;
        TrainingFinalFacts final;
        if (std::isfinite(best_regular)) final.best_regular = best_regular;
        if (std::isfinite(best_ema)) final.best_ema = best_ema;
        final.fallback = result.best_is_fallback;
        final.history_size = result.completed_epochs;
        last_training_progress.test = result.test_summary;
        progress_writer->Finish(last_training_progress, std::move(final));
    }
    distributed_barrier(distributed);
    if (distributed.enabled) {
#if defined(USE_C10D_NCCL)
        distributed.process_group.reset();
        distributed.store.reset();
#endif
    }
    if (main_process) { progress_writer->Close(); }

    return result;
}

TrainingRuntimeOwner::TrainingRuntimeOwner(const TrainRequest& options) : impl_(std::make_unique<Impl>(options)) {}
TrainingRuntimeOwner::~TrainingRuntimeOwner() = default;

TrainRunResult TrainingRuntimeOwner::run() { return impl_->run(); }

}  // namespace

TrainRunResult run_training(const TrainRequest& options) { return TrainingRuntimeOwner(options).run(); }

void print_training_summary(const TrainRequest& options, const TrainRunResult& result) {
    if (options.distributed_worker && options.distributed_rank != 0) { return; }
    const char* source_label = options.resume_path.empty() ? "weights" : "resume";
    const auto best_path = result.best_checkpoint_path.has_value() ? result.best_checkpoint_path->string() : "";
    const auto checkpoint_path = result.checkpoint_path.string();
    const double train_loss = result.history.empty() ? 0.0 : result.history.back().train_loss;
    const auto val_loss = result.history.empty() ? std::optional<double>{} : result.history.back().val_loss;
    const double val_bbox_ap = result.history.empty() ? 0.0 : result.history.back().val_summary.bbox.ap;
    const std::string val_mask_ap = result.history.empty() ? "null" : formatted_mask_ap(result.history.back().val_summary);
    std::string summary;
    if (val_loss.has_value()) {
        summary = std::format(
            "rfdetr train[{}]: preset={} optimizer={} epochs={} train_loss={:.6f} val_loss={:.6f} "
            "val_bbox_ap={:.4f} val_mask_ap={} best={} checkpoint={}",
            source_label, result.artifacts.config.preset_name, cli_enum_spelling(options.optimizer), result.last_epoch + 1, train_loss,
            *val_loss, val_bbox_ap, val_mask_ap, best_path, checkpoint_path);
    } else {
        summary = std::format(
            "rfdetr train[{}]: preset={} optimizer={} epochs={} train_loss={:.6f} val_bbox_ap={:.4f} "
            "val_mask_ap={} best={} checkpoint={}",
            source_label, result.artifacts.config.preset_name, cli_enum_spelling(options.optimizer), result.last_epoch + 1, train_loss,
            val_bbox_ap, val_mask_ap, best_path, checkpoint_path);
    }
    if (mmltk::common::logging::enabled(spdlog::level::info)) {
        mmltk::common::logging::info([&](auto& logger) { logger.info("{}", summary); });
    } else {
        std::println("{}", summary);
    }
}

}  // namespace mmltk::backend::models::rfdetr
