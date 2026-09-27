#include "src/backend/models/rfdetr/training/detail/training_artifact.h"
#include "train.h"
#include "checkpoint.h"
#include "telemetry_writer.h"
#include "training_partition.h"
#include "detail/training_model.h"
#include "detail/model_merging.h"
#include "detail/training_session_checkpoint.h"
#include "detail/training_snapshot.h"
#include "detail/training_lanes.h"
#include "detail/training_data_plan.h"
#include "detail/training_distributed.h"
#include "detail/evaluation_runtime.h"
#include "src/backend/models/rfdetr/contract/cli.h"
#include "src/backend/models/rfdetr/core/runtime.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/core/detection_ops.h"
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/data/compiled/compiled_file_utils.h"
#include "src/backend/data/loading/dataset_loader.h"
#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/system/execution_policy.h"
#include "src/common/io/file_digest.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/frameworks/serialization/reflected_json.h"
#include <torch/types.h>
#include <torch/utils.h>
#include <torch/version.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <iomanip>
#include <limits>
#include <memory>
#include <meta>
#include <mutex>
#include <optional>
#include <print>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <spdlog/spdlog.h>
#include "spdmon/spdmon.hpp"
import mmltk.backend.models.rfdetr.core.dataset_limit_resolution; // CLEANUP-IGNORE: Training directly imports its dataset policy owner.
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
namespace mmltk::backend::models::rfdetr {
namespace torch_cuda = mmltk::backend::ml::cuda;
using mmltk::common::math::checked_cast;
namespace {
int rfdetr_output_class_count(uint32_t dataset_class_count) { return static_cast<int>(dataset_class_count) + 1; }
int checked_inference_batch_size(size_t batch_size) {
 const size_t effective_batch_size = std::max<size_t>(1, batch_size);
 if (effective_batch_size > static_cast<size_t>(std::numeric_limits<int>::max())) { throw std::runtime_error("batch_size exceeds supported inference compilation range"); }
 return static_cast<int>(effective_batch_size);
}
std::string phase_progress_label(const char* phase, int epoch, int total_epochs) { return std::format("{} {}/{}", phase, epoch + 1, total_epochs); }
int effective_train_lanes(const TrainRequest& options) {
 if (options.lanes < 1) throw std::runtime_error("RF-DETR train --lanes must be positive");
 return options.lanes;
}
bool is_rank_zero(const DistributedContext& distributed) { return distributed.rank == 0; }
std::string train_progress_postfix(double average_class_loss, double average_box_loss, double average_loss, double step_class_loss, double step_box_loss, double step_loss, double images_per_second,
 int64_t optimizer_steps, int64_t steps_per_epoch) {
 return std::format("cl={:.4f}, bl={:.4f}, l={:.4f}, scl={:.4f}, sbl={:.4f}, sl={:.4f}, img/s={:.2f}, step={}/{}", average_class_loss, average_box_loss, average_loss, step_class_loss, step_box_loss,
  step_loss, images_per_second, optimizer_steps, steps_per_epoch);
}
std::string formatted_mask_ap(const EvalSummary& summary) { return summary.mask.has_value() ? std::format("{:.4f}", summary.mask->ap) : "null"; }
// Prediction layout and selected distributed world; criterion normalization is global.
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
 detection_config.set_cost_class = config.set_cost_class;
 detection_config.set_cost_bbox = config.set_cost_bbox;
 detection_config.set_cost_giou = config.set_cost_giou;
}
DetectionConfig make_detection_config(const NativeRfDetrConfig& config, int64_t world_size, CompilationMode compilation_mode) {
 DetectionConfig detection_config;
 project_loss_coefficients(detection_config, config);
 apply_detection_scale(detection_config, config, world_size);
 apply_detection_mask_supervision(detection_config, config);
 apply_detection_loss_terms(detection_config, config, compilation_mode);
 apply_detection_box_weights(detection_config, config);
 populate_default_detection_weight_dict(detection_config);
 return detection_config;
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
 if (request.preset_name.empty() && request.resume_path.empty()) {
  if (const auto* preset = find_model_preset_by_weight_filename(request.weights_path.filename().string())) request.preset_name = preset->preset_name;
 }
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
 std::filesystem::path last_checkpoint_path;
};
TrainRunResult TrainingRuntimeOwner::Impl::run() {
 mmltk::common::logging::ScopedProfile profile_rfdetr_train_total{"rfdetr.train.total"};
 // The public host vocabulary is available to this CUDA-linked target;
 // request-only admission remains in the runtime boundary.
 validate_train_request(options);
 TrainingPreparationWriter preparation(options);
 if (!options.distributed_worker && options.device_ids.size() > 1) { throw std::runtime_error("multi-GPU RF-DETR training requires one materialized worker request per selected partition"); }
 const int requested_train_lanes = effective_train_lanes(options);
 auto runtime_config = resolve_runtime_config(options.workers, requested_train_lanes, options.prefetch_factor, options.cpu_affinity, options.device_id, options.numa_node);
 runtime_config.h2d_dataloader = options.h2d_dataloader;
 RuntimeContext train_runtime(runtime_config);
 const auto& placement = train_runtime.execution().placement;
 mmltk::common::system::ScopedExecutionPolicy boundary_policy({train_runtime.lane_cpus(), {}, 0, placement.numa_node, -10, false});
 preparation.Stage(TrainingPreparationStage::Distributed);
 DistributedContext distributed = make_distributed_context(options);
 const bool main_process = is_rank_zero(distributed);
 TrainingSessionManifest session;
 std::mutex failure_mutex;
 std::optional<TrainingFailure> first_cause;
 const auto report_failure = [&](std::uint64_t model_id, std::exception_ptr failure) {
  std::lock_guard lock(failure_mutex);
  if (first_cause) return;
  std::string detail;
  try {
   std::rethrow_exception(failure);
  } catch (const std::exception& error) { detail = error.what(); } catch (...) {
   detail = "unknown training model failure";
  }
  if (detail.size() > 65536) detail.resize(65536);
  TrainingFailure cause{session.session_id, model_id, 1, std::move(detail)};
  try {
   first_cause = claim_training_failure(distributed, cause);
  } catch (...) { first_cause = std::move(cause); }
 };
 std::unique_ptr<TrainingTelemetryWriter> writer;
 TrainingMetricProgress latest;
 const auto submit = [&](TrainingMetricProgress progress, TrainingRecordRole role) {
  progress.full_checkpoint_path = last_checkpoint_path;
  writer->Submit(std::move(progress), role);
 };
 TrainRunResult result;
 std::uint64_t active_model = 0;
 try {
  agree_training_topology(distributed, options.device_id);
  agree_training_request(distributed, options);
  // The launcher selects local GPUs and shares these exact files. Agree the
  // complete inode/version identity, then require it unchanged after admission;
  // do not reread every compiled pixel on every rank to calculate a digest.
  struct AdmittedFile final {
   std::filesystem::path path;
   mmltk::common::io::FileSnapshot snapshot;
   std::shared_ptr<const mmltk::backend::data::CompiledDataset> dataset;
  };
  std::vector<AdmittedFile> admitted_files;
  {
   admitted_files.reserve(5);
   for (const auto& [role, path] : std::array{
         std::pair{"train", options.train_compiled_path}, std::pair{"validation", options.val_compiled_path}, std::pair{"test", options.test_compiled_path},
         std::pair{"class-layout", options.class_layout_path}, std::pair{"initial-state", options.resume_path.empty() ? options.weights_path : options.resume_path}
        }) {
    if (!path.empty()) {
     const auto snapshot = mmltk::common::io::FileSnapshot::Read(path);
     std::ostringstream signature;
     [&]<class Snapshot>(const Snapshot& value) {
      template for (constexpr auto field : std::define_static_array(std::meta::nonstatic_data_members_of(^^Snapshot, std::meta::access_context::current()))) signature << value.[:field:] << ':';
     }(snapshot);
     agree_training_text(distributed, role, signature.str());
     // Loader configuration resolves relative paths before opening its source.
     admitted_files.push_back({std::filesystem::absolute(path), snapshot, {}});
    }
   }
  }
  // Every trajectory turn launches collectives on this guarded session stream.
  torch_cuda::TorchCudaDeviceGuard device_guard(torch_cuda::checked_device_index(options.device_id));
  const int train_lane_count = train_runtime.split().lane_threads;
  // RF-DETR returns loss/K and Lightning's accumulation closure divides by K
  // again. Keep this common loop scale independent of criterion normalization.
  auto execution_facts = derive_execution_facts(options, 0);
  execution_facts.admitted_capacity = train_lane_count;
  const auto admitted_microbatches = static_cast<std::size_t>(execution_facts.microbatches_per_attempt);
  const auto rank_slice = training_rank_slice(options.batch_size, distributed.rank, distributed.world_size);
  const std::size_t local_batch_size = rank_slice.count;
  ScopedRuntimeContext worker_scope(&train_runtime);
  std::shared_ptr<mmltk::common::concurrency::WorkerPool> reader_pool;
  const auto training_file = std::ranges::find(admitted_files, std::filesystem::absolute(options.train_compiled_path), &AdmittedFile::path);
  if (training_file == admitted_files.end()) throw std::logic_error("training source lacks admitted file metadata");
  auto make_loader_config_for = [&](const std::filesystem::path& compiled_path, size_t loader_batch_size, int prefetch_factor, bool shard_batches, bool drop_last) {
   auto config = make_loader_config(compiled_path.string(), loader_batch_size, false, prefetch_factor, train_runtime.split().gather_threads, train_runtime.loader_affinity_string(), options.device_id,
    static_cast<uint64_t>(options.seed));
   config.loading = options;
   config.execution = train_runtime.execution();
   config.execution->placement.cpus = train_runtime.loader_cpus();
   config.reader_pool = reader_pool;
   const auto admitted = std::ranges::find(admitted_files, std::filesystem::path(config.compiled_path), &AdmittedFile::path);
   if (admitted == admitted_files.end()) throw std::logic_error("training source lacks admitted file metadata");
   if (!admitted->dataset) {
    // Different path spellings and hard links may select the same immutable
    // file. Every trajectory and evaluation loader borrows that one mapping.
    const auto shared = std::ranges::find_if(admitted_files, [&](const auto& file) { return file.dataset && file.snapshot == admitted->snapshot; });
    using Dataset = mmltk::backend::data::CompiledDataset;
    // Training uses explicit randomly drawn schedules, irrespective of the
    // loader's legacy shuffle switch. Preserve that policy for evaluation aliases
    // of the same admitted file; distinct evaluation sources stay sequential.
    const auto access = admitted->snapshot == training_file->snapshot ? Dataset::AccessPattern::Random : Dataset::AccessPattern::Sequential;
    admitted->dataset = shared != admitted_files.end() ? shared->dataset : std::make_shared<const Dataset>(Dataset::open(admitted->path, access));
   }
   config.source = admitted->dataset;
   config.drop_last = drop_last;
   if (distributed.enabled && shard_batches) {
    config.batch_shard_rank = static_cast<uint32_t>(distributed.rank);
    config.batch_shard_count = static_cast<uint32_t>(distributed.world_size);
   }
   return config;
  };
  const size_t val_batch_size = options.val_batch_size > 0 ? options.val_batch_size : options.batch_size;
  preparation.Stage(TrainingPreparationStage::Dataset);
  reader_pool = std::make_shared<mmltk::common::concurrency::WorkerPool>(std::max(1, train_runtime.split().gather_threads), train_runtime.loader_cpus(), "rfdetr-read",
   static_cast<std::size_t>(options.prefetch_factor) * (options.lane_configuration.models.size() + 3U), &placement, true);
  auto admission_loader =
   std::make_unique<mmltk::backend::data::DatasetLoader>(make_loader_config_for(options.train_compiled_path, std::max<std::size_t>(1, local_batch_size), options.prefetch_factor, false, true));
  auto& train_loader = *admission_loader;
  TrainingDataPlan data_plan(train_loader, options);
  // Admit every logical model before training publishes or updates any state.
  auto epoch_draws = data_plan.epoch(0, 0);
  for (std::size_t model_index = 1; model_index < data_plan.shards().size(); ++model_index) (void)data_plan.epoch(model_index, 0);
  std::unique_ptr<mmltk::backend::data::DatasetLoader> val_loader;
  if (main_process) { val_loader = std::make_unique<mmltk::backend::data::DatasetLoader>(make_loader_config_for(options.val_compiled_path, val_batch_size, options.prefetch_factor, false, false)); }
  const std::uint32_t val_max_instances = val_loader ? val_loader->max_instances_per_image() : mmltk::backend::data::inspect_compiled_dataset(options.val_compiled_path).max_instances_per_image;
  std::optional<mmltk::backend::data::CompiledDatasetInfo> test_info;
  if (!options.test_compiled_path.empty()) { test_info = mmltk::backend::data::inspect_compiled_dataset(options.test_compiled_path); }
  TrainingDatasetLimits dataset_limits;
  dataset_limits.train_max_instances = train_loader.max_instances_per_image();
  dataset_limits.val_max_instances = val_max_instances;
  if (test_info.has_value()) { dataset_limits.test_max_instances = test_info->max_instances_per_image; }
  dataset_limits.largest_max_instances = std::max(dataset_limits.train_max_instances, dataset_limits.val_max_instances);
  if (dataset_limits.test_max_instances.has_value()) { dataset_limits.largest_max_instances = std::max(dataset_limits.largest_max_instances, *dataset_limits.test_max_instances); }
  std::optional<TrainingSessionAdmission> resumed;
  if (!options.resume_path.empty()) {
   // Rank zero admits every referenced archive before peers receive the admission
   // turn. All ranks retain immutable generation leases while staging live state.
   if (main_process) resumed.emplace(options.resume_path);
   distributed_barrier(distributed);
   if (!main_process) resumed.emplace(options.resume_path);
   if (resumed->plan().plan_hash != data_plan.hash() || resumed->plan().shards != data_plan.shards()) throw std::runtime_error("resume data plan differs from compiled membership");
  }
  preparation.Stage(TrainingPreparationStage::Checkpoint);
  const auto source_checkpoint = resumed ? std::filesystem::canonical(options.resume_path).parent_path() / resumed->manifest().models.front().path : options.weights_path;
  if (train_loader.image_width() != train_loader.image_height()) throw std::runtime_error("train compiled RF-DETR input must be square");
  std::optional<ResolvedModelState> transferred;
  if (!resumed) transferred = resolve_model_state(source_checkpoint, options.preset_name, static_cast<int>(train_loader.image_width()), options.class_layout_path);
  auto artifacts = resumed ? resolve_admitted_model_artifacts(resumed->model(0), source_checkpoint, options.preset_name, static_cast<int>(train_loader.image_width()), options.class_layout_path)
                           : transferred->artifacts;
  auto original_descriptor = resumed ? std::filesystem::path(resumed->continuation(0).values.training_original_descriptor) : options.class_layout_path;
  if (!resumed) apply_stock_training_coefficients(artifacts.config);
  artifacts.config.training_supervision = options.training_supervision;
  dataset_limits.automatic_num_queries_cap = checked_cast<std::size_t>(artifacts.automatic_num_queries_cap, "RF-DETR automatic query cap exceeds size_t");
  const ResolvedDatasetLimit required_query_limit = resolve_dataset_query_limit(dataset_limits.largest_max_instances, 0U, dataset_limits.automatic_num_queries_cap);
  const ResolvedDatasetLimit requested_query_limit = resolve_dataset_query_limit(dataset_limits.largest_max_instances, options.num_queries, dataset_limits.automatic_num_queries_cap);
  dataset_limits.required_num_queries = required_query_limit.as_size;
  dataset_limits.requested_override = options.num_queries != 0U;
  if (training_supervision_enabled(artifacts.config.training_supervision) &&
      (train_loader.num_classes() == 0 || train_loader.num_classes() >= static_cast<std::uint32_t>(std::numeric_limits<int>::max()))) {
   throw std::runtime_error("feature-active RF-DETR supervision requires a representable nonempty object-class catalog");
  }
  const int dataset_output_classes = rfdetr_output_class_count(train_loader.num_classes());
  if (!options.resume_path.empty()) {
   const auto& resume_checkpoint = resumed->model(0);
   if (artifacts.class_layout != native_training_class_layout(*train_loader.class_catalog())) throw std::runtime_error("resume class layout does not match ordered compiled catalog");
   if (resume_checkpoint.metadata.num_classes > 0 && resume_checkpoint.metadata.num_classes != static_cast<int64_t>(dataset_output_classes)) {
    throw std::runtime_error("resume checkpoint class count does not match compiled dataset class count");
   }
   const std::size_t stored_queries = checked_cast<std::size_t>(resume_checkpoint.metadata.num_queries, "resume checkpoint query count exceeds size_t");
   if (dataset_limits.requested_override && requested_query_limit.as_size != stored_queries) {
    throw std::runtime_error("resume preserves checkpoint query count " + std::to_string(stored_queries) + "; requested query override resolves to " + std::to_string(requested_query_limit.as_size));
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
  if (!training_supervision_model_config_valid(artifacts.config)) { throw std::runtime_error("resolved RF-DETR model is incompatible with the requested training supervision"); }
  if (training_supervision_enabled(artifacts.config.training_supervision) &&
      !training_supervision_query_layout_valid(artifacts.config.training_supervision, static_cast<std::size_t>(artifacts.config.num_queries), static_cast<std::size_t>(artifacts.config.group_detr),
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
   if (loader.image_width() != static_cast<uint32_t>(artifacts.config.resolution) || loader.image_height() != static_cast<uint32_t>(artifacts.config.resolution)) {
    throw std::runtime_error(std::string(split) + " compiled resolution does not match RF-DETR input size");
   }
   if (!loader.class_catalog()->ordered_equal(*train_loader.class_catalog())) { throw std::runtime_error("ordered compiled class catalog mismatch across train/val/test splits"); }
  };
  if (!val_loader && !mmltk::backend::data::inspect_compiled_dataset(options.val_compiled_path).class_catalog->ordered_equal(*train_loader.class_catalog()))
   throw std::runtime_error("ordered validation class catalog mismatch");
  validate_loader(train_loader, "train");
  if (val_loader) { validate_loader(*val_loader, "val"); }
  if (test_info.has_value()) {
   if (test_info->width != static_cast<std::uint32_t>(artifacts.config.resolution) || test_info->height != static_cast<std::uint32_t>(artifacts.config.resolution)) {
    throw std::runtime_error("test compiled resolution does not match RF-DETR input size");
   }
   if (!test_info->class_catalog->ordered_equal(*train_loader.class_catalog())) { throw std::runtime_error("ordered compiled class catalog mismatch across train/val/test splits"); }
  }
  torch::manual_seed(static_cast<std::uint64_t>(options.seed));
  std::ostringstream resolved_signature;
  resolved_signature << std::hexfloat;
  [&]<class Config>(const Config& config) {
   template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^Config, std::meta::access_context::current()))) {
    const auto& value = config.[:member:];
    if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, TrainingSupervisionConfig>) {
     std::array<std::byte, 4096> scratch{};
     resolved_signature << mmltk::frameworks::serialization::reflected_json(value, scratch, {.max_bytes = scratch.size(), .max_items = 128, .max_depth = 16}).dump();
    } else
     resolved_signature << std::define_static_string(std::meta::identifier_of(member)) << '=' << value << ';';
   }
  }(artifacts.config);
  resolved_signature << encode_class_layout(artifacts.class_layout);
  agree_training_text(distributed, "resolved-model", resolved_signature.str());
  agree_training_text(distributed, "data-plan", std::to_string(data_plan.hash()) + ":" + std::to_string(epoch_draws.microbatches));
  preparation.Stage(TrainingPreparationStage::Model);
  auto common = std::make_shared<NativeRfDetrModel>(artifacts.config, artifacts.class_layout);
  common->initialize_training_supervision(static_cast<std::uint64_t>(options.seed));
  agree_model_inventory(distributed, *common, "initialized-cpu-inventory");
  preparation.Stage(TrainingPreparationStage::Weights);
  common->to(mmltk::backend::ml::cuda::cuda_device(options.device_id));
  if (!resumed) {
   const auto loaded = load_training_model_weights(*common, transferred->model_state, supervision_route(options.training_supervision));
   if (main_process) {
    mmltk::common::logging::info([&](auto& logger) {
     logger.info("rfdetr weights: loaded={} missing={} unexpected={} incompatible={} input={}", loaded.loaded_names.size(), loaded.missing_names.size(), loaded.unexpected_names.size(),
      loaded.incompatible_names.size(), source_checkpoint.string());
    });
    mmltk::common::logging::warn([&](auto& logger) {
     for (const auto& name : loaded.missing_names) logger.warn("  missing: {}", name);
     for (const auto& name : loaded.unexpected_names) logger.warn("  unexpected: {}", name);
    });
   }
  }
  preparation.Stage(TrainingPreparationStage::Synchronization);
  broadcast_training_model(distributed, *common);
  const auto precision = agree_training_precision(distributed, options.device_id, options.amp, options.fused_optimizer);
  if (main_process)
   mmltk::common::logging::info([&](auto& logger) {
    logger.info(
     "rfdetr train session runtime: torch={} autocast={} models={} logical_train_lanes={} admitted_train_workers={} requested_validation_lanes={} "
     "loader_threads={} gather_threads={} cpu_threads={} global_microbatch_images={} microbatches_per_model_attempt={} effective_batch_per_model={} aggregate_round_images={} "
     "train_max_instances={} val_max_instances={} test_max_instances={} query_source={} num_queries={} automatic_query_cap={} query_override={}",
     TORCH_VERSION, evaluation_precision_name(precision.autocast_dtype), data_plan.shards().size(), options.lanes, train_lane_count, options.validation_lanes, train_runtime.split().loader_threads,
     train_runtime.split().gather_threads, train_runtime.split().cpu_threads, options.batch_size, execution_facts.microbatches_per_attempt, execution_facts.effective_batch_per_model,
     execution_facts.aggregate_round_images, dataset_limits.train_max_instances, dataset_limits.val_max_instances, dataset_limits.test_max_instances.value_or(0U), dataset_limits.query_source,
     dataset_limits.resolved_num_queries, dataset_limits.automatic_num_queries_cap, dataset_limits.requested_override ? "true" : "false");
   });
  const auto precision_kind = precision.autocast_dtype == torch::kFloat16    ? TrainingPrecisionKind::Float16
                              : precision.autocast_dtype == torch::kBFloat16 ? TrainingPrecisionKind::BFloat16
                                                                             : TrainingPrecisionKind::Float32;
  if (resumed && resumed->manifest().precision != precision_kind) throw std::runtime_error("Resume precision is incompatible with saved optimizer/scaler state");
  const auto share_identity = [&](std::string value, std::size_t bytes) {
   if (!main_process) value.assign(bytes, '0');
   if (value.size() != bytes) throw std::logic_error("training identity has an invalid extent");
   auto buffer = torch::from_blob(value.data(), {static_cast<std::int64_t>(bytes)}, torch::kUInt8).clone().to(mmltk::backend::ml::cuda::cuda_device(options.device_id));
   broadcast_training_tensors(distributed, {buffer});
   const auto host = buffer.cpu();
   return std::string(static_cast<const char*>(host.const_data_ptr()), bytes);
  };
  const auto initialization = resumed ? resumed->manifest().initialization : share_identity(main_process ? native_state_fingerprint(collect_module_state(*common)) : std::string{}, 64);
  const auto source_fingerprint = [&](const mmltk::backend::data::DatasetLoader* loader) {
   if (!main_process) return std::string{};
   const auto& source = *loader->compiled_source();
   const auto admitted = std::ranges::find(admitted_files, source.path(), &AdmittedFile::path);
   if (admitted == admitted_files.end()) throw std::logic_error("training source lacks admitted file metadata");
   return training_dataset_identity(source.header(), admitted->snapshot, resolved_signature.str());
  };
  // The compiled header and admitted file generation already identify each
  // source. Starting training must not scan image or mask payloads for a hash.
  const auto configuration_fingerprint = share_identity(source_fingerprint(&train_loader), 64);
  const auto validation_fingerprint = options.val_compiled_path == options.train_compiled_path ? configuration_fingerprint : share_identity(source_fingerprint(val_loader.get()), 64);
  session = resumed ? resumed->manifest() : TrainingSessionManifest{};
  if (!resumed) {
   session.session_id = share_identity(main_process ? training_artifact_identity() : std::string{}, 32);
   session.initialization = initialization;
   session.configuration = configuration_fingerprint;
   session.validation = validation_fingerprint;
   session.precision = precision_kind;
  }
  session.request = options;
  if (session.configuration != configuration_fingerprint) throw std::runtime_error("Resume training data/model configuration differs");
  if (session.validation != validation_fingerprint) throw std::runtime_error("Resume validation split/configuration differs");
  latest.session_id = session.session_id;
  latest.scope = TrainingRecordScope::Session;
  result.artifacts = artifacts;
  result.gpu_augmentation = options.gpu_augmentation;
  result.output_dir = options.output_dir;
  result.checkpoint_path = options.output_dir / "session.json";
  preparation.Stage(TrainingPreparationStage::Optimizers);
  std::vector<std::unique_ptr<TrainingModel>> models;
  // Keep trajectories alive until their first failure is published and their
  // communicator is aborted. Their GPU retirement may otherwise wait behind
  // a peer collective that this failing rank will never enter.
  try {
   models.reserve(data_plan.shards().size());
   const auto detection = make_detection_config(artifacts.config, distributed.world_size, options.compilation_mode);
   for (std::size_t index = 0; index < data_plan.shards().size(); ++index) {
    active_model = data_plan.shards()[index].model_id;
    // All trajectories start from the exact complete common state; stochastic
    // seeds are used only by each trajectory's immutable logical data identities.
    auto native = index == 0 ? common : make_train_lane_model(*common, options.device_id);
    native->configure_supervision_timing(SupervisionTimingSetup{mmltk::backend::ml::cuda::cuda_device(options.device_id), admitted_microbatches, mmltk::common::logging::profile_enabled()});
    auto loader_config = make_loader_config_for(options.train_compiled_path, std::max<std::size_t>(1, local_batch_size), options.prefetch_factor, false, true);
    auto loader = index == 0 ? std::move(admission_loader) : std::make_unique<mmltk::backend::data::DatasetLoader>(loader_config);
    models.push_back(std::make_unique<TrainingModel>(options, index, train_runtime, std::move(loader), std::move(native), data_plan, distributed, precision, detection, report_failure));
    if (resumed) {
     models.back()->stage_resume(resumed->model(index), resumed->continuation(index));
     if (const auto& best = resumed->manifest().models[index].best) models.back()->remember_candidate(TrainingArtifactCandidate{*best, resumed->best_admission(index)});
    }
   }
   for (const auto& file : admitted_files) file.snapshot.RequireUnchanged(file.path);
   if (resumed) resumed->require_unchanged();
   // No live model/optimizer commit occurs until every trajectory passed staging.
   for (auto& trajectory : models) {
    active_model = trajectory->id();
    trajectory->commit_resume();
   }
   if (resumed) last_checkpoint_path = std::filesystem::absolute(options.resume_path);
   auto physical_workers = std::make_shared<mmltk::common::concurrency::WorkerPool>(
    static_cast<std::size_t>(train_lane_count), train_runtime.lane_cpus(), "rfdtrtlane", static_cast<std::size_t>(train_lane_count), &train_runtime.execution().placement, false);
   for (auto& trajectory : models) {
    active_model = trajectory->id();
    trajectory->start(physical_workers);
   }
   if (transferred) transferred->model_state.release_admission();
   const int start_epoch = resumed ? static_cast<int>(session.epoch) : 0;
   const auto resume_attempt = resumed ? session.attempt_id : std::string{};
   result.last_epoch = start_epoch - 1;
   auto metadata = make_native_checkpoint_metadata(artifacts, dataset_output_classes);
   if (resumed) {
    metadata.source_path = resumed->model(0).metadata.source_path;
    metadata.source_kind = resumed->model(0).metadata.source_kind;
   }
   resumed.reset();
   if (main_process) std::filesystem::create_directories(options.output_dir);
   std::unique_ptr<TrainingSessionCheckpoint> checkpoint;
   std::unique_ptr<TrainingValidationRuntime> validation;
   if (main_process) {
    checkpoint = std::make_unique<TrainingSessionCheckpoint>(options.output_dir);
    validation = std::make_unique<TrainingValidationRuntime>(options, train_runtime, std::move(val_loader), val_batch_size, options.validation_loss,
     detection.include_masks ? EvaluationMetricSet::BBoxAndMask : EvaluationMetricSet::BBox, artifacts.config.num_select, "val", dataset_limits.automatic);
    TrainingRun run;
    run.run_id = session.session_id;
    run.configuration = options;
    run.original_weights = metadata.source_path;
    run.original_class_descriptor = original_descriptor;
    run.execution.training = execution_facts;
    run.execution.validation = validation->execution_facts();
    run.execution.dataset_limits = dataset_limits;
    run.class_layout = artifacts.class_layout;
    run.evaluated_weights = options.use_ema ? EvaluatedWeights::Ema : EvaluatedWeights::Ordinary;
    run.source_checkpoint_attempt_id = resume_attempt;
    run.resume_epoch = start_epoch - 1;
    run.resume_optimizer_step = models.front()->schedule().consumed_attempts;
    writer = std::make_unique<TrainingTelemetryWriter>(std::move(run));
    session.attempt_id = writer->attempt_id();
    latest.epoch = start_epoch;
    latest.total_epochs = options.epochs;
    latest.total_batches = epoch_draws.microbatches;
    latest.total_images = checked_training_product(epoch_draws.microbatches, options.batch_size);
    submit(latest, TrainingRecordRole::Boundary);
   }
   const bool periodic = options.lane_configuration.mode == TrainLaneMode::PeriodicAveraging;
   TrainingMergeSchedule merge_schedule(options.lane_configuration, {session.round, session.merge, session.interval_rounds});
   std::vector<std::uint64_t> successful_images(models.size());
   if (!session.models.empty())
    for (std::size_t i = 0; i < models.size(); ++i) successful_images[i] = session.models[i].successful_images;
   NativeModelAverage average;
   std::vector<const std::vector<NormalizedModelStateEntry>*> merge_values;
   merge_values.reserve(models.size());
   for (const auto& model : models) merge_values.push_back(&model->ordinary());
   std::vector<double> merge_weights(models.size());
   const auto merge = [&](TrainingMergeBoundary reason) {
    if (!periodic || !merge_schedule.pending()) return;
    for (std::size_t i = 0; i < models.size(); ++i) merge_weights[i] = static_cast<double>(successful_images[i]);
    const bool any = std::ranges::any_of(successful_images, [](auto count) { return count != 0; });
    if (main_process && any) {
     average.prepare(merge_values, merge_weights);
     average.install(merge_values);
    }
    for (auto& model : models) {
     if (any) {
      broadcast_training_model(distributed, model->model());
      model->ordinary_changed();
     }
    }
    merge_schedule.retire_interval();
    std::ranges::fill(successful_images, 0);
    if (main_process) {
     TrainingMetricProgress event;
     event.phase = TrainingPhase::Merge;
     event.scope = TrainingRecordScope::SynchronizedSession;
     event.session_id = session.session_id;
     event.epoch = mmltk::common::math::checked_cast<int>(session.epoch, "training session epoch exceeds progress range");
     event.total_epochs = options.epochs;
     event.round = merge_schedule.state().round;
     event.merge = merge_schedule.state().merge;
     event.merge_boundary = reason;
     submit(std::move(event), TrainingRecordRole::Boundary);
    }
   };
   const TrainingPlanState immutable_plan{data_plan.hash(), data_plan.shards()};
   std::uint64_t history_size = 0;
   const auto retain_epoch = [&](const TrainingMetricProgress& progress) {
    if (result.history.size() == result.history.capacity()) result.history.erase(result.history.begin());
    result.history.push_back(progress);
    mmltk::common::logging::info([&](auto& logger) {
     logger.info("epoch {} model {} weights {} stats: train_loss={:.6f} val_loss={} bbox_ap={:.4f} mask_ap={}", progress.epoch + 1, progress.model_id,
      progress.artifact->weights == EvaluatedWeights::Ema ? "ema" : "ordinary", progress.train_loss, progress.val_loss ? std::format("{:.6f}", *progress.val_loss) : "unavailable",
      progress.val->bbox.ap, formatted_mask_ap(*progress.val));
    });
    ++history_size;
   };
   for (int epoch = start_epoch; epoch < options.epochs; ++epoch) {
    mmltk::common::logging::ScopedProfile profile_epoch{"rfdetr.train.epoch"};
    result.last_epoch = epoch;
    session.epoch = epoch;
    // Admit every model's complete window before entering any model's epoch.
    std::vector<TrainingEpochDraws> schedules;
    for (std::size_t i = 0; i < models.size(); ++i) schedules.push_back(data_plan.epoch(i, epoch));
    for (std::size_t i = 0; i < models.size(); ++i) {
     active_model = models[i]->id();
     agree_training_text(distributed, "epoch-model", std::to_string(active_model) + ":" + std::to_string(schedules[i].microbatches));
     models[i]->begin_epoch(epoch, std::move(schedules[i]));
    }
    std::unique_ptr<spdmon::ProgressBar> progress;
    if (main_process && options.progress_bar) {
     std::uint64_t total = 0, completed = 0;
     for (const auto& model : models) {
      const auto state = model->progress(TrainingPhase::Train);
      total = mmltk::common::math::checked_add(total, state.total_images, "session progress image overflow");
      completed = mmltk::common::math::checked_add(completed, state.completed_images, "session progress image overflow");
     }
     progress = std::make_unique<spdmon::ProgressBar>(phase_progress_label("train", epoch, options.epochs), total, "img");
     progress->add(completed);
     progress->set_postfix("cl=warming, bl=warming, l=warming");
    }
    std::vector last_live(models.size(), std::chrono::steady_clock::now() - std::chrono::seconds(1));
    for (;;) {
     bool attempted = false;
     // Current-turn capacity is exclusive: its P slots are reserved. There is
     // no speculative spare admission, so a peer can never starve its counts.
     for (std::size_t i = 0; i < models.size(); ++i) {
      active_model = models[i]->id();
      if (models[i]->exhausted()) continue;  // deterministic idle slot on every rank
      attempted = true;
      const auto images = models[i]->attempt();
      successful_images[i] = mmltk::common::math::checked_add(successful_images[i], images, "merge successful-image counter overflow");
      if (progress) {
       progress->add(execution_facts.effective_batch_per_model);
       const auto state = models[i]->progress(TrainingPhase::Train);
       if (state.completed_batches % std::max(1, options.print_freq) == 0 || models[i]->exhausted())
        progress->set_postfix("model=" + std::to_string(models[i]->id()) + " " +
                              train_progress_postfix(state.class_loss, state.box_loss, state.train_loss, state.step_class_loss, state.step_box_loss, state.step_loss, state.images_per_second,
                               state.optimizer_steps, state.steps_per_epoch));
      }
      if (main_process && std::chrono::steady_clock::now() - last_live[i] >= std::chrono::seconds(1)) {
       latest = models[i]->progress(TrainingPhase::Train);
       latest.session_id = session.session_id;
       latest.round = merge_schedule.state().round;
       submit(latest, TrainingRecordRole::Live);
       last_live[i] = std::chrono::steady_clock::now();
      }
     }
     if (!attempted) break;
     if (merge_schedule.finish_round(true)) merge(TrainingMergeBoundary::Rounds);
    }
    if (progress) progress->close();
    for (auto& model : models) model->end_epoch();
    merge(TrainingMergeBoundary::Epoch);
    distributed_barrier(distributed);
    if (main_process) {
     if (periodic && !options.use_ema) {
      // Each trajectory's settled optimizer facts need durable custody even
      // when Live updates coalesce. Only the synchronized record is evaluated.
      for (const auto& model : models) {
       latest = model->progress(TrainingPhase::EpochComplete);
       latest.session_id = session.session_id;
       submit(latest, TrainingRecordRole::Epoch);
      }
     }
     // These borrowed scopes finish after every archive callback. If validation
     // or publication throws, they release before any trajectory is destroyed.
     std::vector<TrainingSnapshotPublication> publications;
     publications.reserve(models.size());
     std::optional<TrainingArtifactCandidate> synchronized;
     for (auto& model : models) {
      active_model = model->id();
      publications.push_back(model->begin_publication());
      if (!periodic && options.use_ema) model->save_ordinary_epoch(metadata, options.output_dir);
      if (!periodic || !synchronized) {
       const auto kind = !periodic && options.use_ema ? EvaluatedWeights::Ema : EvaluatedWeights::Ordinary;
       latest = model->progress(TrainingPhase::Validate);
       latest.session_id = session.session_id;
       submit(latest, TrainingRecordRole::Boundary);
       const auto evaluated = model->evaluate(*validation, kind);
       auto artifact =
        model->save_candidate(metadata, options.output_dir, session.session_id, initialization, session.configuration, validation_fingerprint, merge_schedule.state().merge, kind, evaluated.summary);
       latest = model->progress(TrainingPhase::EpochComplete);
       latest.session_id = session.session_id;
       latest.artifact = artifact.artifact;
       latest.val = evaluated.summary;
       latest.val_loss = evaluated.loss;
       if (periodic) {
        latest.scope = TrainingRecordScope::SynchronizedSession;
        latest.model_id = 0;
        synchronized = artifact;
       }
       retain_epoch(latest);
       submit(latest, TrainingRecordRole::Epoch);
       if (!options.use_ema)
        model->remember_candidate(std::move(artifact));
       else if (!periodic)
        model->remember_candidate(std::move(artifact));
      } else if (!options.use_ema) {
       auto same = *synchronized;
       same.artifact.model_id = model->id();
       same.artifact.attempt = model->schedule().consumed_attempts;
       model->remember_candidate(std::move(same));
      }
      if (periodic && options.use_ema) {
       const auto evaluated = model->evaluate(*validation, EvaluatedWeights::Ema);
       auto artifact = model->save_candidate(
        metadata, options.output_dir, session.session_id, initialization, session.configuration, validation_fingerprint, merge_schedule.state().merge, EvaluatedWeights::Ema, evaluated.summary);
       latest = model->progress(TrainingPhase::EpochComplete);
       latest.session_id = session.session_id;
       latest.artifact = artifact.artifact;
       latest.val = evaluated.summary;
       latest.val_loss = evaluated.loss;
       retain_epoch(latest);
       submit(latest, TrainingRecordRole::Epoch);
       model->remember_candidate(std::move(artifact));
      }
     }
     session.epoch = epoch + 1;
     session.round = merge_schedule.state().round;
     session.merge = merge_schedule.state().merge;
     session.interval_rounds = merge_schedule.state().interval_rounds;
     session.models.clear();
     std::vector<std::shared_ptr<const TrainingArtifactAdmission>> candidate_admissions;
     candidate_admissions.reserve(models.size());
     for (std::size_t i = 0; i < models.size(); ++i) {
      const auto& best = models[i]->best();
      session.models.push_back({models[i]->id(), {}, {}, successful_images[i], best ? std::optional{best->artifact} : std::nullopt});
      if (best) candidate_admissions.push_back(best->admission);
     }
     checkpoint->publish(session, immutable_plan,
      [&](const std::filesystem::path& destination, std::size_t index) { models[index]->save_resume(destination, metadata, session.attempt_id, original_descriptor); }, candidate_admissions);
     for (auto& publication : publications) publication.finish();
     last_checkpoint_path = checkpoint->path();
     latest = {};
     latest.phase = TrainingPhase::EpochComplete;
     latest.epoch = epoch;
     latest.total_epochs = options.epochs;
     latest.session_id = session.session_id;
     latest.full_checkpoint_path = last_checkpoint_path;
     latest.scope = TrainingRecordScope::Session;
     latest.model_id = 0;
     submit(latest, TrainingRecordRole::Epoch);
     ++result.completed_epochs;
    }
    distributed_barrier(distributed);
   }
   merge(TrainingMergeBoundary::Terminal);
   if (main_process) {
    // Selected artifacts contain deployment weights. Their evaluation owners
    // must omit the training-only supervision modules pruned at publication.
    auto deployment_config = artifacts.config;
    deployment_config.training_supervision = {};
    std::vector<TrainingSelectionCandidate> candidates;
    for (const auto& model : models) {
     if (!model->best()) throw std::runtime_error("final selection requires a completed scheduled-validation candidate for every model");
     double coefficient = 1;
     if (options.lane_configuration.mode != TrainLaneMode::SharedGradients) coefficient = std::ranges::find(options.lane_configuration.models, model->id(), &TrainModelSettings::model_id)->coefficient;
     candidates.push_back({model->best()->artifact, coefficient, model->best()->admission});
    }
    const auto evaluate_native = [&](const std::filesystem::path& path) {
     NativeRfDetrModel candidate(deployment_config, artifacts.class_layout);
     candidate.to(mmltk::backend::ml::cuda::cuda_device(options.device_id));
     load_model_weights(candidate, path, false);
     candidate.optimize_for_inference(checked_inference_batch_size(val_batch_size), false, options.compilation_mode);
     return evaluate_model(options, *validation, candidate, detection, false, EvaluationPurpose::SelectionValidation, EvaluatedWeights::Soup, std::nullopt).summary;
    };
    result.selected = select_training_artifact(candidates, effective_final_policy(options.lane_configuration), detection.include_masks, options.output_dir, evaluate_native);
    if (!options.test_compiled_path.empty()) {
     auto test_loader = std::make_unique<mmltk::backend::data::DatasetLoader>(make_loader_config_for(options.test_compiled_path, val_batch_size, options.prefetch_factor, false, false));
     validate_loader(*test_loader, "test");
     TrainingValidationRuntime test(options, train_runtime, std::move(test_loader), val_batch_size, false, detection.include_masks ? EvaluationMetricSet::BBoxAndMask : EvaluationMetricSet::BBox,
      artifacts.config.num_select, "test", dataset_limits.automatic);
     NativeRfDetrModel selected(deployment_config, artifacts.class_layout);
     selected.to(mmltk::backend::ml::cuda::cuda_device(options.device_id));
     load_model_weights(selected, result.selected->artifact.path, false);
     selected.optimize_for_inference(checked_inference_batch_size(val_batch_size), false, options.compilation_mode);
     result.test_summary = evaluate_model(options, test, selected, detection, false, EvaluationPurpose::FinalTest, result.selected->artifact.weights, std::nullopt).summary;
    }
    latest = models.front()->progress(TrainingPhase::Completed);
    latest.session_id = session.session_id;
    latest.phase = TrainingPhase::Completed;
    latest.scope = TrainingRecordScope::SelectedOutput;
    latest.model_id = result.selected->artifact.model_id;
    latest.artifact = result.selected->artifact;
    latest.val = result.selected->validation;
    latest.checkpoint_path = result.selected->artifact.path;
    latest.full_checkpoint_path = last_checkpoint_path;
    latest.test = result.test_summary;
    TrainingFinalFacts final;
    final.history_size = history_size;
    final.selected = result.selected;
    writer->Finish(latest, std::move(final));
   }
   distributed_barrier(distributed);
   distributed_shutdown(distributed);
   if (writer) writer->Close();
  } catch (...) {
   report_failure(active_model, std::current_exception());
   distributed_abort(distributed);
   throw;
  }
 } catch (...) {
  report_failure(active_model, std::current_exception());
  distributed_abort(distributed);
  if (writer) {
   latest = {};
   latest.phase = TrainingPhase::Error;
   latest.scope = TrainingRecordScope::Model;
   latest.model_id = first_cause->model_id;
   latest.session_id = first_cause->session_id;
   latest.failure = first_cause;
   latest.full_checkpoint_path = last_checkpoint_path;
   writer->Fail(latest);
   writer->Close();
  }
  if (!main_process) throw TrainingPeerCancelled{};
  throw std::runtime_error(first_cause->detail);
 }
 if (main_process) result.checkpoint_path = last_checkpoint_path;
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
 const auto best_path = result.selected ? result.selected->artifact.path.string() : "";
 const auto checkpoint_path = result.checkpoint_path.string();
 std::string summary = std::format("rfdetr train[{}]: preset={} optimizer={} epochs={} selected={} checkpoint={}", source_label, result.artifacts.config.preset_name,
  cli_enum_spelling(options.recipe.optimizer), result.last_epoch + 1, best_path, checkpoint_path);
 if (result.selected) {
  const auto& selected = *result.selected;
  summary += std::format(" selected_val_bbox_ap={:.4f} selected_val_mask_ap={} selected_metric={:.4f} best_individual_metric={:.4f}", selected.validation.bbox.ap,
   formatted_mask_ap(selected.validation), *selected.artifact.selection_metric, selected.best_individual_metric);
 }
 if (!result.history.empty()) {
  const auto& observation = result.history.back();
  const auto model = observation.scope == TrainingRecordScope::SynchronizedSession && observation.artifact ? observation.artifact->model_id : observation.model_id;
  summary += std::format(" last_observed_model={} train_loss={:.6f}", model, observation.train_loss);
  if (observation.val_loss) summary += std::format(" model_val_loss={:.6f}", *observation.val_loss);
 }
 if (mmltk::common::logging::enabled(spdlog::level::info)) {
  mmltk::common::logging::info([&](auto& logger) { logger.info("{}", summary); });
 } else {
  std::println("{}", summary);
 }
}
}  // namespace mmltk::backend::models::rfdetr
