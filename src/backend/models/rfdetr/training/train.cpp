#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "detail/training_snapshot.h"
#include "detail/training_lanes.h"
#include "detail/training_metrics.h"
#include <ATen/cuda/CUDAContext.h>
#include <torch/version.h>
#include <torch/csrc/autograd/autograd.h>
#include <torch/nn/utils/clip_grad.h>
#include <c10/core/InferenceMode.h>
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
#include "src/backend/ml/torch/archive.h"
#include "src/backend/models/rfdetr/core/detection_ops.h"
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/models/rfdetr/core/sample_output.h"
#include "src/backend/models/rfdetr/core/execution_precision.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/core/postprocess.h"
#include "src/backend/ml/torch/scalar_type.h"
#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/backend/models/rfdetr/core/model_info.h"
#include "src/backend/models/rfdetr/training/training_partition.h"
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
#include <torch/types.h>
#include <torch/serialize.h>
#include "detail/checkpoint_private.h"
#include "detail/training_continuation.h"
#include "detail/model_ema.h"
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
#include "src/backend/models/rfdetr/core/runtime.h"
#include "src/backend/ml/cuda/shared_cuda_event.h"
#include "detail/gpu_augment_private.h"
#include "detail/native_optimizer_private.h"
#include "detail/target_builder_private.h"
#include "detail/training_ops_private.h"
#include "detail/evaluation_runtime.h"
#include "checkpoint.h"
import mmltk.backend.models.rfdetr.core.dataset_limit_resolution; // CLEANUP-IGNORE: Training directly imports its dataset policy owner.
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
import mmltk.backend.ml.cuda.gpu_quiescence;
namespace mmltk::backend::models::rfdetr {
namespace torch_cuda = mmltk::backend::ml::cuda;
using mmltk::common::math::checked_cast;
using mmltk::frameworks::gpu::ensure_cuda_ok;
namespace {
int rfdetr_output_class_count(uint32_t dataset_class_count) { return static_cast<int>(dataset_class_count) + 1; }
int checked_inference_batch_size(size_t batch_size) {
    const size_t effective_batch_size = std::max<size_t>(1, batch_size);
    if (effective_batch_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("batch_size exceeds supported inference compilation range");
    }
    return static_cast<int>(effective_batch_size);
}
std::string phase_progress_label(const char* phase, int epoch, int total_epochs) { return std::format("{} {}/{}", phase, epoch + 1, total_epochs); }
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
bool is_rank_zero(const DistributedContext& distributed) { return distributed.rank == 0; }
std::string train_progress_postfix(double average_class_loss, double average_box_loss, double average_loss, double step_class_loss, double step_box_loss,
                                   double step_loss, double images_per_second, int64_t optimizer_steps, int64_t steps_per_epoch) {
    return std::format("cl={:.4f}, bl={:.4f}, l={:.4f}, scl={:.4f}, sbl={:.4f}, sl={:.4f}, img/s={:.2f}, step={}/{}", average_class_loss, average_box_loss,
                       average_loss, step_class_loss, step_box_loss, step_loss, images_per_second, optimizer_steps, steps_per_epoch);
}
std::string format_nonfinite_loss_report(const TensorMap& loss_dict, const std::vector<torch::Tensor>& parameters,
                                         const std::vector<std::string>& parameter_names) {
    std::ostringstream report;
    bool wrote_loss = false;
    for (const auto& [name, value] : loss_dict) {
        if (!torch::isfinite(value).item<bool>()) {
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
        if (param.defined() && !torch::isfinite(param).all().item<bool>()) {
            report << " nonfinite_param=" << parameter_names[index];
            break;
        }
        if (param.grad().defined() && !torch::isfinite(param.grad()).all().item<bool>()) {
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
std::string formatted_mask_ap(const EvalSummary& summary) { return summary.mask.has_value() ? std::format("{:.4f}", summary.mask->ap) : "null"; }
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
    torch_cuda::TorchCudaDeviceGuard device_guard(mmltk::backend::ml::cuda::checked_device_index(options.device_id));
    distributed.store = c10::make_intrusive<c10d::FileStore>(options.distributed_store_path.string(), options.distributed_world_size);
    auto pg_options = c10::make_intrusive<c10d::ProcessGroupNCCL::Options>();
    pg_options->timeout = c10d::kProcessGroupNCCLDefaultTimeout;
    distributed.process_group = c10::make_intrusive<c10d::ProcessGroupNCCL>(distributed.store, distributed.rank, distributed.world_size, std::move(pg_options));
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
void distributed_all_reduce_coalesced(const DistributedContext& distributed, std::vector<torch::Tensor>& tensors) {
    if (!distributed.enabled || tensors.empty()) { return; }
#if defined(USE_C10D_NCCL)
    distributed.process_group->allreduce_coalesced(tensors)->wait();
#else
    (void)distributed;
    (void)tensors;
    throw std::runtime_error("distributed RF-DETR training requires a LibTorch build with NCCL/c10d enabled");
#endif
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
void apply_detection_loss_terms(DetectionConfig& detection_config, const NativeRfDetrConfig& config, const CompilationMode compilation_mode) {
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
void average_gradients(const DistributedContext& distributed, const std::vector<torch::Tensor>& parameters) {
    if (!distributed.enabled) { return; }
    std::vector<torch::Tensor> gradients;
    gradients.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        if (parameter.grad().defined()) { gradients.push_back(parameter.grad()); }
    }
    if (gradients.empty()) { return; }
    distributed_all_reduce_coalesced(distributed, gradients);
    const double scale = 1.0 / static_cast<double>(distributed.world_size);
    for (auto& gradient : gradients) { gradient.mul_(scale); }
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
    apply_train_recipe(request, train_recipe(request.optimizer), request.recipe_overrides);
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
    auto runtime_config =
        resolve_runtime_config(options.workers, requested_train_lanes, options.prefetch_factor, options.cpu_affinity, options.device_id, options.numa_node);
    runtime_config.h2d_dataloader = options.h2d_dataloader;
    RuntimeContext train_runtime(runtime_config);
    const auto& placement = train_runtime.execution().placement;
    mmltk::common::system::ScopedExecutionPolicy boundary_policy({train_runtime.lane_cpus(), {}, 0, placement.numa_node, -10, false});
    DistributedContext distributed = make_distributed_context(options);
    const bool main_process = is_rank_zero(distributed);
    const int train_lane_count = train_runtime.split().lane_threads;
    TrainingEventOwner training_events(options.device_id, static_cast<std::size_t>(train_lane_count) + 1U);
    ScopedRuntimeContext worker_scope(&train_runtime);
    auto make_loader_config_for = [&](const std::filesystem::path& compiled_path, size_t loader_batch_size, bool shuffle, int prefetch_factor,
                                      bool shard_batches, bool drop_last) {
        auto config = make_loader_config(compiled_path.string(), loader_batch_size, shuffle, prefetch_factor, train_runtime.split().gather_threads,
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
        val_loader ? val_loader->max_instances_per_image() : mmltk::backend::data::inspect_compiled_dataset(options.val_compiled_path).max_instances_per_image;
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
    if (train_loader.image_width() != train_loader.image_height()) { throw std::runtime_error("train compiled RF-DETR input must be square"); }
    ModelArtifactRequest artifact_request;
    artifact_request.weights_path = source_checkpoint;
    artifact_request.preset_name = options.preset_name;
    artifact_request.resolution = static_cast<int>(train_loader.image_width());
    auto admitted = resolve_model_state(artifact_request.weights_path, artifact_request.preset_name, artifact_request.resolution, options.class_layout_path);
    auto artifacts = admitted.artifacts;
    auto original_descriptor = options.class_layout_path;
    std::optional<detail::TrainingContinuation> continuation;
    if (!options.resume_path.empty()) {
        if (!admitted.model_state.admitted_archive()) {
            throw std::runtime_error("--resume requires a native RF-DETR .pt checkpoint: " + options.resume_path.string());
        }
        continuation = detail::read_training_continuation(*admitted.model_state.admitted_archive());
        if (!continuation) throw std::runtime_error("--resume requires a full training checkpoint");
        detail::require_active_training_continuation(*continuation, options);
        original_descriptor = continuation->values.training_original_descriptor;
    }
    artifacts.config.training_supervision = options.training_supervision;
    dataset_limits.automatic_num_queries_cap = checked_cast<std::size_t>(artifacts.automatic_num_queries_cap, "RF-DETR automatic query cap exceeds size_t");
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
        if (resume_checkpoint.metadata.num_classes > 0 && resume_checkpoint.metadata.num_classes != static_cast<int64_t>(dataset_output_classes)) {
            throw std::runtime_error("resume checkpoint class count does not match compiled dataset class count");
        }
        const std::size_t stored_queries = checked_cast<std::size_t>(resume_checkpoint.metadata.num_queries, "resume checkpoint query count exceeds size_t");
        if (dataset_limits.requested_override && requested_query_limit.as_size != stored_queries) {
            throw std::runtime_error("resume preserves checkpoint query count " + std::to_string(stored_queries) + "; requested query override resolves to " +
                                     std::to_string(requested_query_limit.as_size));
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
        !training_supervision_query_layout_valid(artifacts.config.training_supervision, static_cast<std::size_t>(artifacts.config.num_queries),
                                                 static_cast<std::size_t>(artifacts.config.group_detr),
                                                 static_cast<std::size_t>(dataset_limits.largest_max_instances))) {
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
    model.initialize_training_supervision(static_cast<std::uint64_t>(options.seed));
    model.to(mmltk::backend::ml::cuda::cuda_device(options.device_id));
    model.configure_supervision_timing(SupervisionTimingSetup{mmltk::backend::ml::cuda::cuda_device(options.device_id),
                                                              static_cast<std::size_t>(std::max(1, options.grad_accum_steps)),
                                                              mmltk::common::logging::profile_enabled()});
    const auto resolved_route = supervision_route(options.training_supervision);
    std::optional<detail::NormalizedModelStateCandidate> resume_model_candidate;
    ModelStateLoadSummary load_summary;
    if (!options.resume_path.empty()) {
        resume_model_candidate = model.stage_normalized_state(admitted.model_state.entries(), detail::NormalizedModelStateAdmission::Exact);
        load_summary = resume_model_candidate->summary;
    } else {
        load_summary = load_training_model_weights(model, admitted.model_state, resolved_route);
    }
    model.optimize_for_inference(checked_inference_batch_size(options.batch_size), true, options.compilation_mode);
    if (!options.val_compiled_path.empty()) { model.optimize_for_inference(checked_inference_batch_size(val_batch_size), false, options.compilation_mode); }
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
    model.train();
    if (options.freeze_encoder) {
        for (auto& item : model.named_parameters(true)) {
            if (is_encoder_param(item.key())) { item.value().set_requires_grad(false); }
        }
    }
    auto optimizer_build = build_optimizer(model, options);
    auto& optimizer = optimizer_build.optimizer;
    auto& all_params = optimizer.parameters();
    const auto& all_param_names = optimizer.parameter_names();
    const bool amp_enabled = options.amp;
    const auto autocast_dtype = amp_enabled ? resolve_cuda_autocast_dtype() : at::kFloat;
    const bool scaler_enabled = amp_enabled && autocast_dtype == torch::kFloat16;
    GradScaler grad_scaler(scaler_enabled);
    std::optional<ModelEma> ema;
    if (options.resume_path.empty() && options.use_ema && main_process) { ema.emplace(all_params, options.ema_decay, static_cast<double>(options.ema_tau)); }
    int start_epoch = 0;
    std::string resume_attempt_id;
    double best_regular = -std::numeric_limits<double>::infinity();
    double best_ema = -std::numeric_limits<double>::infinity();
    if (!options.resume_path.empty()) {
        ResumeState resume_state = load_resume_checkpoint_state(options.resume_path, admitted.model_state, *continuation, optimizer, options, all_param_names,
                                                                all_params, main_process);
        if (resume_model_candidate.has_value()) { model.commit_normalized_state(std::move(*resume_model_candidate)); }
        if (!resume_state.optimizer_candidate.has_value()) { throw std::logic_error("admitted RF-DETR resume is missing its optimizer candidate"); }
        optimizer.commit(std::move(*resume_state.optimizer_candidate));
        if (resume_state.scaler_scale.has_value()) { grad_scaler.load_state(*resume_state.scaler_scale, *resume_state.scaler_growth_tracker); }
        if (resume_state.restored_ema.has_value()) { ema = std::move(resume_state.restored_ema); }
        start_epoch = resume_state.start_epoch;
        resume_attempt_id = std::move(resume_state.attempt_id);
        best_regular = resume_state.best_regular;
        best_ema = resume_state.best_ema;
    }
    continuation.reset();
    admitted.model_state.release_admission();
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
                TORCH_VERSION, evaluation_precision_name(autocast_dtype), optimizer.kind_name(), optimizer.backend_name(), grad_scaler.enabled() ? "on" : "off",
                train_lane_count, train_runtime.split().lane_threads, train_runtime.split().loader_threads, train_runtime.split().gather_threads,
                train_runtime.split().cpu_threads, effective_batch_per_rank(options, train_lane_count),
                effective_batch_global(options, distributed, train_lane_count), dataset_limits.train_max_instances, dataset_limits.val_max_instances,
                dataset_limits.test_max_instances.value_or(0U), dataset_limits.query_source, dataset_limits.resolved_num_queries,
                dataset_limits.automatic_num_queries_cap, dataset_limits.requested_override ? "true" : "false");
        });
        if (options.optimizer == TrainOptimizerKind::Muon && options.fused_optimizer) {
            mmltk::common::logging::warn([](auto& logger) {
                logger.warn(
                    "rfdetr train runtime: optimizer=muon ignores --fused-optimizer and runs with the eager backend "
                    "only");
            });
        }
    }
    const size_t usable_full_batches = full_batches_per_rank(train_loader.num_images(), options.batch_size, static_cast<size_t>(distributed.world_size),
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
    auto metadata = make_native_checkpoint_metadata(artifacts, dataset_output_classes);
    if (!options.resume_path.empty()) {
        metadata.source_path = admitted.model_state.metadata.source_path;
        metadata.source_kind = admitted.model_state.metadata.source_kind;
    }
    auto checkpoint_configuration = options;
    checkpoint_configuration.resolution = artifacts.config.resolution;
    checkpoint_configuration.preset_name = artifacts.config.preset_name;
    TrainingSnapshot checkpoint_snapshot;
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
    torch_cuda::TorchCudaDeviceGuard device_guard(mmltk::backend::ml::cuda::checked_device_index(options.device_id));
    std::unique_ptr<TrainingValidationRuntime> validation_runtime;
    if (main_process) {
        validation_runtime =
            std::make_unique<TrainingValidationRuntime>(options, train_runtime, std::move(val_loader), val_batch_size, options.validation_loss,
                                                        detection_config.include_masks ? EvaluationMetricSet::BBoxAndMask : EvaluationMetricSet::BBox,
                                                        artifacts.config.num_select, "val", dataset_limits.automatic);
    }
    TrainingMetricHandoff metric_handoff(options.device_id);
    const mmltk::frameworks::gpu::DeviceContext augmentation_context(options.device_id, mmltk::frameworks::gpu::cuda_image_copy_backend(),
                                                                     mmltk::frameworks::gpu::DeviceContextMode::PrimaryInterop);
    std::unique_ptr<GpuBatchAugmenter> single_lane_augmenter;
    TargetScratch target_scratch(static_cast<std::size_t>(std::max(1, options.grad_accum_steps)));
    if (train_lane_count <= 1) {
        single_lane_augmenter = std::make_unique<GpuBatchAugmenter>(options.gpu_augmentation, static_cast<std::int64_t>(options.batch_size),
                                                                    static_cast<int>(train_loader.image_height()), static_cast<int>(train_loader.image_width()),
                                                                    augmentation_context);
    }
    TrainingLanes train_lanes(options, train_runtime, train_loader, model, all_param_names, train_lane_count, augmentation_context);
    size_t parameter_version = 0;
    distributed_barrier(distributed);
    for (int epoch = start_epoch; epoch < options.epochs; ++epoch) {
        mmltk::common::logging::ScopedProfile profile_rfdetr_train_epoch{"rfdetr.train.epoch"};
        result.last_epoch = epoch;
        train_loader.begin_epoch();
        model.train();
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
            progress = std::make_unique<spdmon::ProgressBar>(phase_progress_label("train", epoch, options.epochs),
                                                             static_cast<size_t>(usable_full_batches) * static_cast<size_t>(options.batch_size), "img");
            progress->set_postfix("cl=warming, bl=warming, l=warming");
        }
        auto write_progress_snapshot = [&](TrainingPhase phase, std::optional<double> val_loss, const std::optional<EvalSummary>& val_summary,
                                           const std::filesystem::path& checkpoint_override, bool force) {
            if (!main_process) { return; }
            const double elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch_started).count();
            const double average_class_loss = reported_micro_batches > 0 ? host_class_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double average_box_loss = reported_micro_batches > 0 ? host_box_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double average_loss = reported_micro_batches > 0 ? host_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double batches_per_second = elapsed_seconds > 0.0 ? static_cast<double>(local_micro_batches) / elapsed_seconds : 0.0;
            const double images_per_second =
                elapsed_seconds > 0.0 ? static_cast<double>(local_micro_batches) * static_cast<double>(options.batch_size) / elapsed_seconds : 0.0;
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
            progress_writer->Submit(std::move(snapshot), phase == TrainingPhase::EpochComplete
                                                             ? TrainingRecordRole::Epoch
                                                             : (force ? TrainingRecordRole::Boundary : TrainingRecordRole::Live));
        };
        auto flush_progress = [&](bool force) {
            if (local_micro_batches == 0) { return; }
            const double average_class_loss = reported_micro_batches > 0 ? host_class_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double average_box_loss = reported_micro_batches > 0 ? host_box_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double average_loss = reported_micro_batches > 0 ? host_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch_started).count();
            const double images_per_second =
                elapsed_seconds > 0.0 ? static_cast<double>(local_micro_batches) * static_cast<double>(options.batch_size) / elapsed_seconds : 0.0;
            const bool should_update_bar = static_cast<bool>(progress) && (force || local_micro_batches % std::max(1, options.print_freq) == 0);
            if (should_update_bar) {
                progress->set_postfix(train_progress_postfix(average_class_loss, average_box_loss, average_loss, current_step_class_loss, current_step_box_loss,
                                                             current_step_loss, images_per_second, optimizer_steps, steps_per_epoch));
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
            if (options.clip_max_norm > 0.0) { torch::nn::utils::clip_grad_norm_(all_params, options.clip_max_norm); }
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
            if (ema.has_value() && !gradient_overflow) { ema->update(); }
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
            params_ready = record_current_stream_event(training_events.pool(), options.device_id, "record parallel train parameter readiness");
            if (!params_ready) throw std::runtime_error("training CUDA event capacity is exhausted");
        }
        if (train_lane_count <= 1) {
            while (true) {
                auto batch = next_train_full_batch();
                if (!batch.has_value()) { break; }
                metric_handoff.begin_wave();
                LoaderBatchGuard batch_guard(train_loader, *batch, options.device_id);
                torch::Tensor normalized;
                {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_augment{"rfdetr.train.augment"};
                    mmltk::common::logging::ScopedProfile profile_benchmark_rfdetr_train_augmentation{"benchmark.rfdetr.train.augmentation"};
                    normalized = single_lane_augmenter->run(*batch, static_cast<std::uint64_t>(options.seed), epoch, distributed.rank, local_full_batches - 1);
                }
                torch::Tensor loss;
                torch::Tensor class_loss;
                torch::Tensor box_loss;
                scalar_packet::Tensors scalar_values;
                TensorMap loss_dict;
                auto& model_owner = (model);
                PreparedTargets prepared;
                {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_targets{"rfdetr.train.targets"};
                    prepared = build_targets(*batch, static_cast<int>(train_loader.image_height()), static_cast<int>(train_loader.image_width()),
                                             detection_config.include_masks, detection_config.include_masks, options.device_id, target_scratch, "train",
                                             artifacts.config.num_queries, artifacts.config.training_supervision,
                                             static_cast<int>(model.class_layout()->catalog()->size()), &single_lane_augmenter->batch_plan());
                }
                batch_guard.set_consumer_stream(single_lane_augmenter->prepare_batch_consumer());
                static_cast<void>(single_lane_augmenter->finish_batch(*batch));
                batch_guard.release();
                std::optional<DeviceLossNormalizer> active_normalizer;
                if (route_is_active(training_route)) {
                    auto target_count = torch::tensor({static_cast<float>(prepared_target_count(prepared))},
                                                      torch::TensorOptions().dtype(torch::kFloat32).device(normalized.device()));
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
                        outputs = model.forward_with_denoising(NestedTensor{normalized, prepared.nested_mask}, prepared,
                                                               TrainingStepIdentity{static_cast<std::uint64_t>(options.seed), static_cast<std::uint64_t>(epoch),
                                                                                    static_cast<std::uint32_t>(distributed.rank), local_full_batches - 1});
                    } else {
                        mmltk::common::logging::ScopedProfile profile_rfdetr_train_forward{"rfdetr.train.forward"};
                        outputs = model.forward_for_match_free(NestedTensor{normalized, prepared.nested_mask});
                        target_consumer.handoff();
                    }
                    SupervisionTimingLease criterion_timing(model_owner, true, SupervisionTimingLease::Kind::Criterion);
                    auto routed = compute_routed_training_loss(model, training_route, outputs, prepared, *active_normalizer, detection_config);
                    criterion_timing.finish();
                    loss = std::move(routed.total);
                    class_loss = std::move(routed.classification);
                    box_loss = std::move(routed.box);
                    loss_dict = std::move(routed.ordinary_terms);
                    scalar_values = std::move(routed.scalars);
                } else {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_forward{"rfdetr.train.forward"};
                    outputs = model.forward(NestedTensor{normalized, prepared.nested_mask}, true);
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_targets_handoff{"rfdetr.train.targets_handoff"};
                    target_consumer.handoff();
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_loss_dict{"rfdetr.train.loss_dict"};
                    loss_dict = detection_loss_dict(
                        outputs, prepared, detection_config, true, distributed.enabled,
                        distributed.enabled ? AllReduceTensorFn([&distributed](torch::Tensor& value) { distributed_all_reduce_tensor(distributed, value); })
                                            : AllReduceTensorFn{});
                }
                if (!route_is_active(training_route)) {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_loss_total{"rfdetr.train.loss_total"};
                    torch::Tensor auxiliary;
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
                    static_cast<void>(model.harvest_supervision_timing());
                }
                if (progress) { progress->add(static_cast<size_t>(options.batch_size)); }
                flush_progress(false);
            }
        } else {
            while (local_full_batches < usable_full_batches) {
                metric_handoff.begin_wave();
                const double current_scale = grad_scaler.enabled() ? static_cast<double>(grad_scaler.current_scale()) : 1.0;
                const double scaled_loss_factor = current_scale / static_cast<double>(micro_batches_per_optimizer_step(options, train_lane_count));
                ParallelTrainingWave<TrainLaneResult> wave(static_cast<std::size_t>(train_lane_count), route_is_active(training_route) || distributed.enabled,
                                                           options.device_id, distributed);
                for (int lane_index = 0; lane_index < train_lane_count; ++lane_index) {
                    auto batch = next_train_full_batch();
                    if (!batch.has_value()) { throw std::runtime_error("native RF-DETR training ended an epoch with an incomplete parallel train wave"); }
                    wave.add(train_lanes.enqueue(&train_runtime, train_loader, *batch, params_ready ? &*params_ready : nullptr, training_events.pool(),
                                                 scaled_loss_factor, parameter_version, detection_config, model, options.device_id,
                                                 static_cast<int>(train_loader.image_height()), static_cast<int>(train_loader.image_width()),
                                                 static_cast<std::uint64_t>(options.seed), epoch, distributed.rank, local_full_batches - 1, amp_enabled,
                                                 autocast_dtype, training_route, wave.normalizer(), static_cast<std::size_t>(lane_index)));
                }
                wave.settle(mmltk::backend::ml::cuda::cuda_device(options.device_id), [&](TrainLaneResult& lane_result) {
                    train_lanes.merge(lane_result, all_params, options.device_id);
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
                            params_ready = record_current_stream_event(training_events.pool(), options.device_id, "record parallel train parameter readiness");
                            if (!params_ready) throw std::runtime_error("training CUDA event capacity is exhausted");
                        },
                        nullptr, static_cast<int64_t>(train_lane_count));
                    train_lanes.harvest_timing();
                }
                flush_progress(false);
            }
        }
        if (params_ready) params_ready->retire();
        {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_drain_loader{"rfdetr.train.drain_loader"};
            mmltk::backend::data::Batch drain_batch{};
            while (train_loader.next_batch(drain_batch)) { train_loader.release_batch(drain_batch); }
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
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_wait_pending_copy{"rfdetr.train.parallel.wait_pending_copy"};
            train_lanes.settle_targets();
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
                if (ema) selected.emplace(*ema, (model));
                val_result =
                    evaluate_model(options, *validation_runtime, model, training_events, detection_config, options.validation_loss,
                                   EvaluationPurpose::ScheduledValidation, ema ? EvaluatedWeights::Ema : EvaluatedWeights::Ordinary, epoch, &metric_handoff);
                if (selected) selected->restore();
                model.train();
            }
            if (val_result.loss.has_value()) {
                mmltk::common::logging::info([&](auto& logger) {
                    logger.info("epoch {} stats: optimizer={} train_loss={:.6f} val_loss={:.6f} bbox_ap={:.4f} mask_ap={}", epoch + 1, optimizer.kind_name(),
                                train_loss, *val_result.loss, val_result.summary.bbox.ap, formatted_mask_ap(val_result.summary));
                });
            } else {
                mmltk::common::logging::info([&](auto& logger) {
                    logger.info("epoch {} stats: optimizer={} train_loss={:.6f} bbox_ap={:.4f} mask_ap={}", epoch + 1, optimizer.kind_name(), train_loss,
                                val_result.summary.bbox.ap, formatted_mask_ap(val_result.summary));
                });
            }
            checkpoint_snapshot.begin(model);
            checkpoint_snapshot.save_weights(options.output_dir / std::format("checkpoint_epoch_{}.pt", epoch + 1), metadata, false, options.class_layout_path);
            // Preparing later artifacts must not undo a successfully published epoch.
            checkpoint_snapshot.prepare_ema(all_param_names, ema ? &*ema : nullptr);
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
                checkpoint_snapshot.save_weights(selected_path, metadata, true, options.class_layout_path);
                best_metric = selected_metric;
                result.best_is_ema = options.use_ema;
                result.best_checkpoint_path = selected_path;
            }
            {
                mmltk::common::logging::ScopedProfile profile_rfdetr_train_save_resume{"rfdetr.train.save.resume"};
                checkpoint_snapshot.save_resume(checkpoint_path, metadata, optimizer, grad_scaler, checkpoint_configuration, epoch, best_regular, best_ema,
                                                ema ? ema->completed_updates() : 0, progress_writer->attempt_id(), original_descriptor);
                checkpoint_snapshot.release();
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
        const auto overrides = ema ? ema_override_map(all_param_names, *ema) : std::unordered_map<std::string, torch::Tensor>{};
        save_collected_checkpoint(fallback_path, metadata, model, ema ? &overrides : nullptr, "rfdetr.train.save.selected_fallback",
                                  "rfdetr.train.save.selected_fallback.collect_state", options.class_layout_path);
        result.best_checkpoint_path = fallback_path;
        result.best_is_ema = options.use_ema;
        result.best_is_fallback = true;
    }
    if (main_process && !options.test_compiled_path.empty()) {
        auto test_config = make_loader_config_for(options.test_compiled_path, val_batch_size, false, options.prefetch_factor, false, false);
        auto test_loader = std::make_unique<mmltk::backend::data::DatasetLoader>(test_config);
        validate_loader(*test_loader, "test");
        TrainingValidationRuntime test_runtime(options, train_runtime, std::move(test_loader), val_batch_size, false,
                                               detection_config.include_masks ? EvaluationMetricSet::BBoxAndMask : EvaluationMetricSet::BBox,
                                               artifacts.config.num_select, "test", dataset_limits.automatic);
        NativeRfDetrModel best_model(artifacts.config, artifacts.class_layout);
        best_model.to(mmltk::backend::ml::cuda::cuda_device(options.device_id));
        load_model_weights(best_model, *result.best_checkpoint_path, false);
        best_model.optimize_for_inference(checked_inference_batch_size(val_batch_size), false, options.compilation_mode);
        result.test_summary = evaluate_model(options, test_runtime, best_model, training_events, detection_config, false, EvaluationPurpose::FinalTest,
                                             result.best_is_ema ? EvaluatedWeights::Ema : EvaluatedWeights::Ordinary, std::nullopt)
                                  .summary;
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
            source_label, result.artifacts.config.preset_name, cli_enum_spelling(options.optimizer), result.last_epoch + 1, train_loss, *val_loss, val_bbox_ap,
            val_mask_ap, best_path, checkpoint_path);
    } else {
        summary = std::format(
            "rfdetr train[{}]: preset={} optimizer={} epochs={} train_loss={:.6f} val_bbox_ap={:.4f} "
            "val_mask_ap={} best={} checkpoint={}",
            source_label, result.artifacts.config.preset_name, cli_enum_spelling(options.optimizer), result.last_epoch + 1, train_loss, val_bbox_ap,
            val_mask_ap, best_path, checkpoint_path);
    }
    if (mmltk::common::logging::enabled(spdlog::level::info)) {
        mmltk::common::logging::info([&](auto& logger) { logger.info("{}", summary); });
    } else {
        std::println("{}", summary);
    }
}
}  // namespace mmltk::backend::models::rfdetr
