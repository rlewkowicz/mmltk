#include "detail/training_model.h"
#include "detail/training_snapshot.h"
#include "detail/training_data_plan.h"
#include "detail/training_distributed.h"
#include "detail/evaluation_runtime.h"
#include "detail/native_optimizer_private.h"
#include "src/backend/data/dataset_loader.h"
#include "detail/training_lanes.h"
#include "detail/training_gradient_reducer.h"
#include "detail/training_metrics.h"
#include "detail/training_artifact.h"
#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/common/math/checked_arithmetic.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
namespace mmltk::backend::models::rfdetr {
namespace {
TrainRequest model_request(TrainRequest value, const TrainingShard& shard) {
 if (value.lane_configuration.mode != TrainLaneMode::SharedGradients) {
  const auto found = std::ranges::find(value.lane_configuration.models, shard.model_id, &TrainModelSettings::model_id);
  if (found == value.lane_configuration.models.end()) throw std::logic_error("training shard has no model recipe");
  value.recipe = found->recipe;
 }
 return value;
}
}
struct TrainingModel::Impl final {
 Impl(TrainRequest request, std::size_t index, RuntimeContext& context, std::unique_ptr<mmltk::backend::data::DatasetLoader> data,
  std::shared_ptr<NativeRfDetrModel> native, const TrainingDataPlan& plan, DistributedContext group, TrainingPrecision precision, DetectionConfig criterion, std::function<void(std::uint64_t, std::exception_ptr)> failure)
  : options(model_request(std::move(request), plan.shards().at(index))), shard(plan.shards().at(index)), shard_index(index), runtime(context), loader(std::move(data)), owner(std::move(native)),
    data_plan(plan), distributed(std::move(group)), precision(precision), detection(std::move(criterion)), failure(std::move(failure)),
    contributions(derive_execution_facts(options, 0).microbatches_per_attempt), rank_slice(plan.rank_slice(distributed.rank, distributed.world_size)),
    events(options.device_id, runtime.split().lane_threads + 1),
    scaler(precision.autocast_dtype == torch::kFloat16), donors(options.lane_configuration.mode == TrainLaneMode::SharedGradients ? options.lanes : 1, options.batch_size),
    epoch_policy(options.unfreeze_encoder_last_epochs, options.disable_augmentation_last_epochs), metrics(options.device_id),
    augmentation_context(options.device_id, mmltk::frameworks::gpu::cuda_image_copy_backend(), mmltk::frameworks::gpu::DeviceContextMode::PrimaryInterop) {
  if (distributed.rank == 0) mmltk::common::logging::warn([&](auto& logger) {
   if (options.recipe.optimizer == TrainOptimizerKind::Muon && options.fused_optimizer)
    logger.warn("rfdetr train runtime: model={} optimizer=muon ignores --fused-optimizer and runs with the eager backend only", shard.model_id);
  });
  options.fused_optimizer = precision.fused_optimizer;
  owner->train();
  for (auto& item : owner->named_parameters(true)) if (options.freeze_encoder && is_encoder_param(item.key())) item.value().set_requires_grad(false);
  optimizer_build = build_optimizer(owner->named_parameters(true), options);
  clock = std::make_unique<TrainingSchedule>(options.recipe, optimizer_build.base_lrs, optimizer_build.roles, options.epochs, plan.epoch(index, 0).microbatches, contributions);
  continuation.data.model_id = shard.model_id;
  continuation.data.plan_hash = plan.hash();
  continuation.execution = derive_execution_facts(options, 0);
  continuation.execution.admitted_capacity = runtime.split().lane_threads;
  owner->optimize_for_inference(static_cast<int>(std::max<std::uint64_t>(1, rank_slice.count)), true, options.compilation_mode);
  ordinary = collect_module_state(*owner);
 }
 TrainRequest options;
 const TrainingShard& shard;
 std::size_t shard_index;
 RuntimeContext& runtime;
 std::unique_ptr<mmltk::backend::data::DatasetLoader> loader;
 std::shared_ptr<NativeRfDetrModel> owner;
 const TrainingDataPlan& data_plan;
 DistributedContext distributed;
 TrainingPrecision precision;
 DetectionConfig detection;
 std::function<void(std::uint64_t, std::exception_ptr)> failure;
 std::size_t contributions;
 TrainingRankSlice rank_slice;
 TrainingEventOwner events;
 OptimizerBuildResult optimizer_build;
 GradScaler scaler;
 std::optional<ModelEma> ema;
 TrainingDonorHistory donors;
 TrainingEpochPolicy epoch_policy;
 std::unique_ptr<TrainingSchedule> clock;
 TrainingMetricHandoff metrics;
 mmltk::frameworks::gpu::DeviceContext augmentation_context;
 std::unique_ptr<TrainingLanes> lanes;
 std::unique_ptr<TrainingGradientReducer> reducer;
 std::shared_ptr<TrainingTargetCounts> counts;
 TrainingSnapshot snapshot;
 std::vector<NormalizedModelStateEntry> ordinary;
 detail::TrainingContinuationValues continuation;
 std::optional<detail::NormalizedModelStateCandidate> staged_model;
 std::optional<ResumeState> staged_resume;
 std::optional<TrainingArtifactCandidate> best;
 TrainingEpochDraws draws;
 TrainingMetricSnapshot last_metrics;
 TrainingLossReport losses;
 std::vector<TrainingDonorDescriptor> current_donors;
 std::uint64_t epoch = 0, cursor = 0, resume_cursor = 0, waves = 0, parameter_version = 0;
 std::chrono::steady_clock::time_point epoch_started;
 bool epoch_closed = false;
 bool policy_installed = false;
};
TrainingModel::TrainingModel(TrainRequest request, std::size_t index, RuntimeContext& runtime, std::unique_ptr<mmltk::backend::data::DatasetLoader> loader,
 std::shared_ptr<NativeRfDetrModel> model, const TrainingDataPlan& plan, DistributedContext group, TrainingPrecision precision, DetectionConfig criterion, std::function<void(std::uint64_t, std::exception_ptr)> failure)
 : impl_(std::make_unique<Impl>(std::move(request), index, runtime, std::move(loader), std::move(model), plan, std::move(group), precision, std::move(criterion), std::move(failure))) {}
TrainingModel::~TrainingModel() = default;
void TrainingModel::stage_resume(const DecodedNativeModelState& state, const detail::TrainingContinuation& saved) {
 auto& p = *impl_;
 p.snapshot.require_inactive();
 detail::require_active_training_continuation(saved, p.options);
 if (saved.values.data.model_id != p.shard.model_id || saved.values.data.plan_hash != p.data_plan.hash()) throw std::invalid_argument("resume model/data identity differs");
 p.donors.restore(*p.loader, saved.values.data.donors);
 p.clock->restore(saved.values.schedule);
 p.epoch_policy = TrainingEpochPolicy(p.options.unfreeze_encoder_last_epochs, p.options.disable_augmentation_last_epochs, saved.values.epoch_policy);
 const auto& optimizer = p.optimizer_build.optimizer;
 std::vector<std::uint8_t> active;
 active.reserve(optimizer.eligible_parameter_names().size());
 for (std::size_t i = 0; i < optimizer.eligible_parameter_names().size(); ++i)
  active.push_back(optimizer.eligible_parameters()[i].requires_grad() || (saved.values.epoch_policy.encoder_unfrozen && is_encoder_param(optimizer.eligible_parameter_names()[i])));
 p.staged_model = p.owner->stage_normalized_state(state.entries(), detail::NormalizedModelStateAdmission::Exact);
 p.staged_resume = load_resume_checkpoint_state({}, state, saved, p.optimizer_build.optimizer, optimizer.eligible_parameter_names(), optimizer.eligible_parameters(), active);
 if (p.distributed.rank == 0) mmltk::common::logging::info([&](auto& logger) {
  const auto& loaded = p.staged_model->summary;
  logger.info("rfdetr resume weights: model={} loaded={} missing={} unexpected={} incompatible={} input={}", p.shard.model_id, loaded.loaded_names.size(),
   loaded.missing_names.size(), loaded.unexpected_names.size(), loaded.incompatible_names.size(), state.class_artifact->artifact_path().string());
 });
 p.staged_resume->optimizer_candidate->admit_continuation(saved.values.schedule);
 if (p.options.use_ema && static_cast<std::uint64_t>(saved.values.ema_completed_updates) != saved.values.schedule.consumed_attempts)
  throw std::invalid_argument("saved EMA age differs from consumed model attempts");
 agree_training_continuation(p.distributed, saved.values);
 if (p.precision.autocast_dtype != torch::kFloat16 && (saved.values.grad_scaler_scale != p.scaler.current_scale() || saved.values.grad_scaler_growth_tracker != 0))
  throw std::invalid_argument("saved gradient scaler requires the admitted FP16 precision");
 p.continuation = saved.values;
 p.epoch = saved.values.epoch;
 p.draws = p.data_plan.epoch(p.shard_index, p.epoch);
 p.epoch_closed = saved.values.data.epoch > p.epoch;
 p.cursor = p.epoch_closed ? p.draws.microbatches : saved.values.data.next_microbatch;
 p.resume_cursor = p.cursor;
 p.metrics.restore_epoch(saved.values.epoch_metrics);
 p.epoch_started = std::chrono::steady_clock::now();
}
void TrainingModel::commit_resume() {
 auto& p = *impl_;
 p.snapshot.require_inactive();
 if (!p.staged_resume || !p.staged_model) return;
 p.owner->commit_normalized_state(std::move(*p.staged_model));
 broadcast_training_model(p.distributed, *p.owner);
 p.optimizer_build.optimizer.commit(std::move(*p.staged_resume->optimizer_candidate));
 if (p.continuation.epoch_policy.encoder_unfrozen) {
  for (auto& item : p.owner->named_parameters(true)) if (is_encoder_param(item.key())) item.value().set_requires_grad(true);
  p.optimizer_build.optimizer.activate();
 }
 p.optimizer_build.optimizer.broadcast_state(p.distributed);
 p.scaler.load_state(*p.staged_resume->scaler_scale, *p.staged_resume->scaler_growth_tracker);
 if (p.staged_resume->ema_cpu_shadow) p.ema = ModelEma::from_cpu_shadow(p.optimizer_build.optimizer.eligible_parameters(), *p.staged_resume->ema_cpu_shadow,
  p.options.ema_decay, p.options.ema_tau, p.continuation.ema_completed_updates);
 p.staged_model.reset(); p.staged_resume.reset();
}
void TrainingModel::start(std::shared_ptr<mmltk::common::concurrency::WorkerPool> workers) {
 auto& p = *impl_;
 p.snapshot.require_inactive();
 if (p.staged_resume) throw std::logic_error("trajectory cannot start before complete session admission");
 if (!p.ema && p.options.use_ema) p.ema.emplace(p.optimizer_build.optimizer.eligible_parameters(), p.options.ema_decay, p.options.ema_tau);
 if (p.ema) broadcast_training_tensors(p.distributed, p.ema->shadow_params());
 agree_training_text(p.distributed, "trajectory-precision", std::string(p.optimizer_build.optimizer.backend_name()) + ":" + std::to_string(static_cast<int>(p.precision.autocast_dtype)) + ":" + std::to_string(p.scaler.current_scale()));
 if (p.distributed.rank == 0) mmltk::common::logging::info([&](auto& logger) {
  logger.info("rfdetr train model runtime: model={} autocast={} optimizer={} optimizer_backend={} scaler={} scaler_scale={} scaler_growth_tracker={} effective_batch_per_model={}",
   p.shard.model_id, evaluation_precision_name(p.precision.autocast_dtype), p.optimizer_build.optimizer.kind_name(), p.optimizer_build.optimizer.backend_name(),
   p.scaler.enabled() ? "on" : "off", p.scaler.current_scale(), p.scaler.growth_tracker(), p.continuation.execution.effective_batch_per_model);
 });
 ensure_train_lane_model_supported(*p.owner, p.runtime.split().lane_threads);
 p.lanes = std::make_unique<TrainingLanes>(p.options, p.runtime, *p.loader, p.owner, p.optimizer_build.optimizer.parameter_names(), p.runtime.split().lane_threads, p.rank_slice.count, p.augmentation_context,
  [&p](std::exception_ptr failure) { p.failure(p.shard.model_id, std::move(failure)); }, std::move(workers));
 p.reducer = std::make_unique<TrainingGradientReducer>(p.distributed, p.options.device_id, mmltk::backend::ml::cuda::getCurrentCUDAStream(p.options.device_id),
  p.optimizer_build.optimizer.parameter_names(), p.optimizer_build.optimizer.parameters(), p.lanes->gradient_leaves());
 p.counts = std::make_shared<TrainingTargetCounts>(p.runtime.split().lane_threads, p.options.device_id, p.distributed);
}
void TrainingModel::begin_epoch(std::uint64_t epoch, TrainingEpochDraws draws) {
 auto& p = *impl_;
 p.snapshot.require_inactive();
 p.epoch = epoch; p.draws = std::move(draws); p.epoch_closed = false;
 const auto previous = p.epoch_policy.state();
 const auto policy = p.epoch_policy.enter(epoch, p.options.epochs);
 auto& optimizer = p.optimizer_build.optimizer;
 if (policy.encoder_unfrozen && !previous.encoder_unfrozen) {
  for (auto& item : p.owner->named_parameters(true)) if (is_encoder_param(item.key())) item.value().set_requires_grad(true);
  optimizer.zero_grad(true); optimizer.activate();
 }
 if (policy != previous || (!p.policy_installed && (policy.encoder_unfrozen || policy.augmentation_disabled))) {
  auto augmentation = p.options.gpu_augmentation;
  if (policy.augmentation_disabled) augmentation.enabled = false;
  p.owner->invalidate_compilation();
  p.owner->optimize_for_inference(static_cast<int>(std::max<std::uint64_t>(1, p.rank_slice.count)), true, p.options.compilation_mode);
  p.lanes->reconfigure(*p.owner, optimizer.parameter_names(), augmentation, static_cast<int>(std::max<std::uint64_t>(1, p.rank_slice.count)), p.options.compilation_mode);
  agree_model_inventory(p.distributed, *p.owner, "epoch-activation");
  p.reducer->rebuild(optimizer.parameter_names(), optimizer.parameters(), p.lanes->gradient_leaves());
 }
 p.policy_installed = true;
 p.clock->begin_epoch(epoch);
 p.cursor = p.continuation.data.epoch == epoch ? p.continuation.data.next_microbatch : 0;
 p.resume_cursor = p.cursor; p.waves = 0;
 if (p.cursor % p.contributions || p.cursor > p.draws.microbatches) throw std::runtime_error("Resume cursor is outside a complete model attempt");
 p.loader->begin_epoch(p.data_plan.rank_schedule(p.draws, p.distributed.rank, p.distributed.world_size), checked_training_product(p.cursor, p.rank_slice.count));
 if (p.cursor) p.metrics.restore_epoch(p.continuation.epoch_metrics); else p.metrics.reset_epoch();
 p.owner->train(); optimizer.zero_grad(true);
 p.epoch_started = std::chrono::steady_clock::now();
}
bool TrainingModel::exhausted() const { return impl_->cursor == impl_->draws.microbatches; }
std::uint64_t TrainingModel::attempt() {
 auto& p = *impl_;
 try {
 p.snapshot.require_inactive();
 if (exhausted()) return 0;
 auto& optimizer = p.optimizer_build.optimizer;
 p.reducer->begin_attempt(p.contributions); p.metrics.begin_attempt(p.contributions);
 auto ready = record_current_stream_event(p.events.pool(), p.options.device_id, "record trajectory parameters");
 if (!ready) throw std::runtime_error("training event capacity exhausted");
 auto& losses = p.losses;
 losses.begin_attempt();
 auto& current_donors = p.current_donors;
 std::size_t contributed = 0;
 while (contributed < p.contributions) {
  const auto wave_size = std::min(static_cast<std::size_t>(p.runtime.split().lane_threads), p.contributions - contributed);
  ParallelTrainingWave<TrainLaneResult> wave(wave_size, true, p.options.device_id, p.distributed, p.counts);
  try {
  for (std::size_t lane = 0; lane < wave_size; ++lane) {
   const auto microbatch = p.cursor;
   const auto offset = checked_training_product(microbatch, p.options.batch_size);
   auto augmentation = p.options.gpu_augmentation;
   if (p.epoch_policy.state().augmentation_disabled) augmentation.enabled = false;
   current_donors.clear();
   if (augmentation.enabled && augmentation.copy_paste_probability > 0) {
    const auto donors = p.donors.admit(*p.loader, microbatch % (p.options.lane_configuration.mode == TrainLaneMode::SharedGradients ? p.options.lanes : 1),
     std::span{p.draws.schedule->draw_keys}.subspan(offset, p.options.batch_size), std::span{p.draws.schedule->image_indices}.subspan(offset, p.options.batch_size), augmentation);
    current_donors.assign(donors.begin() + p.rank_slice.begin, donors.begin() + p.rank_slice.begin + p.rank_slice.count);
   }
   p.clock->consume_microbatch(); ++p.cursor;
   if (!p.rank_slice.count) { p.counts->publish(lane, 0); p.reducer->contribute_empty(); p.metrics.accumulate_empty(); }
   else {
    mmltk::backend::data::Batch batch{};
    if (!p.loader->next_batch(batch)) throw std::runtime_error("global training plan ended before its admitted microbatch");
    if (batch.num_images != p.rank_slice.count) { p.loader->release_batch(batch); throw std::runtime_error("rank batch differs from admitted slice"); }
    try {
     wave.add(p.lanes->enqueue(&p.runtime, *p.loader, batch, &*ready, p.contributions, p.scaler.enabled() ? p.scaler.current_scale() : 1.0, p.parameter_version,
      p.detection, *p.owner, p.options.device_id, p.loader->image_height(), p.loader->image_width(), p.shard.seed, p.epoch, p.distributed.rank, microbatch,
      p.precision.autocast_dtype != torch::kFloat32, p.precision.autocast_dtype, supervision_route(p.options.training_supervision), p.counts, *p.reducer, lane, current_donors));
    } catch (...) { p.loader->release_batch(batch); throw; }
   }
  }
  wave.resolve_counts(); contributed += wave_size;
  if (contributed == p.contributions) p.reducer->enable_gradient_launch_after_counts();
  wave.settle([&](TrainLaneResult& value) { p.lanes->settle(value, p.options.device_id); losses.accumulate(std::move(value.loss_terms));
   p.metrics.accumulate(value.loss, value.class_loss, value.box_loss, value.scalars, value.statistics); });
  } catch (...) {
   // Claim a submission/admission error before the wave cancels its peers.
   p.failure(p.shard.model_id, std::current_exception());
   throw;
  }
  ++p.waves;
 }
 p.reducer->finish_attempt();
 mmltk::common::logging::ScopedProfile profile_optimizer{"rfdetr.train.optimizer"};
 const auto found_inf = p.scaler.check_and_unscale_(optimizer);
 p.last_metrics = p.metrics.complete_step(found_inf, p.contributions, p.cursor, p.distributed);
 const bool overflow = !p.last_metrics.gradients_finite;
 if (!p.last_metrics.loss_finite || (overflow && !p.scaler.enabled())) {
  const auto failure = losses.failure(optimizer.parameters(), optimizer.parameter_names());
  if (!losses.details(optimizer.parameters(), optimizer.parameter_names()).empty()) p.failure(p.shard.model_id, std::make_exception_ptr(failure));
  // Every rank reached the agreed fatal numerical decision. Settle its turn
  // before a healthy rank reports only the generic peer symptom.
  distributed_barrier(p.distributed);
  p.scaler.update(overflow); optimizer.zero_grad(true); throw failure;
 }
 p.clock->prepare_attempt();
 optimizer.set_lrs(p.clock->state().absolute_lrs, 1.0); optimizer.set_momentum(p.clock->state().held_momentum);
 if (!overflow && p.options.clip_max_norm > 0) optimizer.clip_grad_norm_(p.options.clip_max_norm);
 p.scaler.step(optimizer, overflow); p.scaler.update(overflow); optimizer.zero_grad(true);
 p.clock->finish_attempt(!overflow);
 if (p.ema) p.ema->update();
 if (overflow) mmltk::common::logging::warn([&](auto& logger) { logger.warn("model {} skipped optimizer update{}", p.shard.model_id, losses.details(optimizer.parameters(), optimizer.parameter_names())); });
 ++p.parameter_version;
 ready->retire();
 p.lanes->harvest_timing();
 p.continuation.data.epoch = p.epoch; p.continuation.data.next_microbatch = p.cursor;
 return overflow ? 0 : checked_training_product(p.contributions, p.options.batch_size);
 } catch (...) {
  p.failure(p.shard.model_id, std::current_exception());
  throw;
 }
}
void TrainingModel::end_epoch() {
 mmltk::common::logging::ScopedProfile profile_drain{"rfdetr.train.drain_loader"};
 auto& p = *impl_;
 p.snapshot.require_inactive();
 if (!exhausted() || p.cursor == 0 || p.cursor % p.contributions) throw std::logic_error("incomplete training epoch");
 mmltk::backend::data::Batch batch{};
 while (p.loader->next_batch(batch)) p.loader->release_batch(batch);
 p.lanes->settle_targets();
 p.epoch_closed = true;
 p.continuation.data.epoch = p.epoch + 1; p.continuation.data.next_microbatch = 0;
}
TrainingSnapshotPublication TrainingModel::begin_publication() {
 auto& p = *impl_;
 p.snapshot.require_inactive();
 p.continuation.schedule = p.clock->state(); p.continuation.epoch_policy = p.epoch_policy.state();
 p.continuation.epoch_metrics = p.metrics.state(); p.continuation.data.donors = p.donors.state();
 return p.snapshot.begin(p.ordinary, p.optimizer_build.optimizer.eligible_parameter_names(), p.ema ? &*p.ema : nullptr);
}
TrainingMetricProgress TrainingModel::progress(TrainingPhase phase) const {
 const auto& p = *impl_;
 TrainingMetricProgress value;
 value.phase = phase; value.model_id = p.shard.model_id; value.scope = TrainingRecordScope::Model;
 value.epoch = p.epoch; value.total_epochs = p.options.epochs;
 value.completed_batches = p.cursor; value.total_batches = p.draws.microbatches;
 value.completed_images = checked_training_product(p.cursor, p.options.batch_size); value.total_images = checked_training_product(p.draws.microbatches, p.options.batch_size);
 value.completed_waves = p.waves; value.optimizer_steps = p.cursor / p.contributions;
 value.global_optimizer_step = p.clock->state().consumed_attempts; value.steps_per_epoch = p.draws.attempts;
 value.train_lanes = p.runtime.split().lane_threads; value.rank_local = false;
 value.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - p.epoch_started).count();
 value.batches_per_second = value.elapsed_seconds > 0 ? (p.cursor - p.resume_cursor) / value.elapsed_seconds : 0;
 value.images_per_second = value.batches_per_second * p.options.batch_size;
 value.scalars = p.metrics.state().scalar_means();
 const auto denominator = static_cast<double>(std::max<std::uint64_t>(1, p.metrics.state().microbatches));
 value.train_loss = p.metrics.state().loss_sum / denominator; value.class_loss = p.metrics.state().class_loss_sum / denominator; value.box_loss = p.metrics.state().box_loss_sum / denominator;
 value.step_loss = p.last_metrics.step_loss; value.step_class_loss = p.last_metrics.step_class_loss; value.step_box_loss = p.last_metrics.step_box_loss;
 value.scalars.total = p.metrics.state().microbatches ? std::optional{value.train_loss} : std::nullopt;
 value.scalars.images_per_second = value.images_per_second;
 const auto& lrs = p.clock->state().absolute_lrs;
 if (!lrs.empty()) { const auto [low, high] = std::minmax_element(lrs.begin(), lrs.end()); value.scalars.learning_rate = lrs.front(); value.scalars.learning_rate_min = *low; value.scalars.learning_rate_max = *high; }
 if (p.epoch_closed) value.epoch_global_loss = p.metrics.epoch_average();
 return value;
}
EvalPassResult TrainingModel::evaluate(TrainingValidationRuntime& validation, EvaluatedWeights kind) {
 auto& p = *impl_;
 std::optional<ModelEma::Selection> selection;
 if (kind == EvaluatedWeights::Ema) { if (!p.ema) throw std::logic_error("EMA evaluation was not admitted"); selection.emplace(*p.ema, *p.owner); }
 auto evaluation_options = p.options;
 if (p.options.lane_configuration.mode != TrainLaneMode::SharedGradients)
  evaluation_options.output_dir /= "model-" + std::to_string(p.shard.model_id) + (kind == EvaluatedWeights::Ema ? "/ema" : "/ordinary");
 auto result = evaluate_model(evaluation_options, validation, *p.owner, p.events, p.detection, p.options.validation_loss, EvaluationPurpose::ScheduledValidation, kind, p.epoch, &p.metrics);
 if (selection) selection->restore();
 p.owner->train();
 return result;
}
TrainingArtifactCandidate TrainingModel::save_candidate(const NativeCheckpointMetadata& metadata, const std::filesystem::path& directory, std::string_view session,
 std::string_view initialization, std::string_view configuration, std::string_view validation, std::uint64_t merge, EvaluatedWeights weights, const EvalSummary& summary) {
 auto& p = *impl_;
 const auto metric = training_selection_metric(summary, p.detection.include_masks);
 TrainingArtifact value;
 value.session_id = session; value.model_id = p.shard.model_id; value.initialization = initialization; value.configuration = configuration; value.validation = validation;
 value.epoch = p.epoch; value.attempt = p.clock->state().consumed_attempts; value.merge = merge; value.weights = weights; value.selection_metric = metric; value.evaluation = summary;
 value.path = std::filesystem::absolute(directory) / ("model-" + std::to_string(value.model_id) + "-epoch-" + std::to_string(value.epoch) + "-" + (weights == EvaluatedWeights::Ema ? "ema-" : "ordinary-") + training_artifact_identity() + ".pt");
 p.snapshot.save_weights(value.path, metadata, weights == EvaluatedWeights::Ema, p.options.class_layout_path);
 auto admission = std::make_shared<TrainingArtifactAdmission>(value.path);
 value = admission->describe(std::move(value));
 admission->release_decoded_state();
 return {std::move(value), std::move(admission)};
}
void TrainingModel::remember_candidate(TrainingArtifactCandidate value) {
 if (value.artifact.model_id != id() || !value.admission) throw std::invalid_argument("invalid model validation candidate");
 value.admission->require_matches(value.artifact);
 if (!impl_->best || *value.artifact.selection_metric > *impl_->best->artifact.selection_metric) impl_->best = std::move(value);
}
void TrainingModel::save_ordinary_epoch(const NativeCheckpointMetadata& metadata, const std::filesystem::path& directory) {
 auto& p = *impl_;
 p.snapshot.save_weights(directory / ("model-" + std::to_string(id()) + "-epoch-" + std::to_string(p.epoch) + "-ordinary-" + training_artifact_identity() + ".pt"), metadata, false, p.options.class_layout_path);
}
const std::optional<TrainingArtifactCandidate>& TrainingModel::best() const { return impl_->best; }
void TrainingModel::save_resume(const std::filesystem::path& path, const NativeCheckpointMetadata& metadata, std::string_view attempt, const std::filesystem::path& original_descriptor) {
 mmltk::common::logging::ScopedProfile profile_resume{"rfdetr.train.save.resume"};
 auto& p = *impl_;
 p.snapshot.save_resume(path, metadata, p.optimizer_build.optimizer, p.scaler, p.options, p.epoch, p.ema ? p.ema->completed_updates() : 0,
  attempt, original_descriptor, p.continuation);
}
NativeRfDetrModel& TrainingModel::model() { impl_->snapshot.require_inactive(); return *impl_->owner; }
const std::vector<NormalizedModelStateEntry>& TrainingModel::ordinary() const { return impl_->ordinary; }
std::uint64_t TrainingModel::id() const { return impl_->shard.model_id; }
const TrainingScheduleState& TrainingModel::schedule() const { return impl_->clock->state(); }
void TrainingModel::ordinary_changed() { impl_->snapshot.require_inactive(); ++impl_->parameter_version; }
}  // namespace mmltk::backend::models::rfdetr
