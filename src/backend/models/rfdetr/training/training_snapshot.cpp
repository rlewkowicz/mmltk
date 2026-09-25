#include "src/backend/models/rfdetr/contract/cli.h"
#include "checkpoint.h"
#include "detail/training_snapshot.h"
#include "detail/checkpoint_private.h"
#include "src/backend/ml/torch/archive.h"
#include <stdexcept>
#include <unordered_map>
import mmltk.common.logging.profile_utils;
namespace mmltk::backend::models::rfdetr {
namespace torch_cuda = mmltk::backend::ml::cuda;
std::vector<NormalizedModelStateEntry> collect_module_state(const NativeRfDetrModel& model) {
 const auto& module = (model);
 std::vector<NormalizedModelStateEntry> state;
 const auto parameters = module.named_parameters(true);
 state.reserve(parameters.size() + module.named_buffers(true).size());
 for (const auto& item : parameters) {
  NormalizedModelStateEntry entry;
  entry.name = item.key();
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
ModelStateLoadSummary load_training_model_weights(NativeRfDetrModel& model, const DecodedNativeModelState& checkpoint, const TrainingSupervisionRoute route) {
 const ResolvedClassLayout source_layout(checkpoint.metadata.class_layout);
 auto candidate = model.stage_normalized_state(
  checkpoint.entries(), route_is_active(route) ? detail::NormalizedModelStateAdmission::FreshTransfer : detail::NormalizedModelStateAdmission::PartialFreshTransfer, &source_layout);
 auto summary = candidate.summary;
 model.commit_normalized_state(std::move(candidate));
 return summary;
}
std::vector<NormalizedModelStateEntry> ema_state_entries(const std::vector<std::string>& param_names, const ModelEma& ema) {
 const auto& shadows = ema.shadow_params();
 if (shadows.size() != param_names.size()) throw std::runtime_error("RF-DETR EMA parameter count changed unexpectedly");
 std::vector<NormalizedModelStateEntry> state;
 state.reserve(param_names.size());
 for (std::size_t index = 0; index < param_names.size(); ++index) state.push_back({param_names[index], shadows[index].detach()});
 return state;
}
// One immutable epoch snapshot serves each synchronous archive in publication order.
// The archive-local CPU entries borrow completed slot views until save returns.
void save_snapshot_checkpoint(const std::filesystem::path& path, const NativeCheckpointMetadata& metadata, const std::vector<NormalizedModelStateEntry>& ordinary,
 const std::vector<NormalizedModelStateEntry>& ema, torch_cuda::TensorReadbackBuffers& readback, const std::filesystem::path& descriptor) {
 DecodedNativeModelState checkpoint;
 checkpoint.metadata = metadata;
 std::vector<NormalizedModelStateEntry> entries;
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
 checkpoint.replace_entries(std::move(entries));
 save_native_checkpoint(path, checkpoint, descriptor);
}
void save_resume_checkpoint(const std::filesystem::path& checkpoint_path, const NativeCheckpointMetadata& metadata, const NativeOptimizer& optimizer, const GradScaler& grad_scaler,
 const TrainRequest& options, int epoch, int64_t ema_completed_updates, const std::vector<NormalizedModelStateEntry>& model_state,
 const std::vector<NormalizedModelStateEntry>& ema_state, std::string_view attempt_id, const std::filesystem::path& original_descriptor, torch_cuda::TensorReadbackBuffers& readback, detail::TrainingContinuationValues& continuation) {
 mmltk::common::logging::ScopedProfile profile_rfdetr_train_save_resume_total{"rfdetr.train.save.resume.total"};
 std::filesystem::create_directories(checkpoint_path.parent_path());
 torch::serialize::OutputArchive archive;
 detail::write_native_checkpoint_metadata(archive, metadata);
 continuation.epoch = epoch;
 continuation.grad_scaler_scale = static_cast<double>(grad_scaler.current_scale());
 continuation.grad_scaler_growth_tracker = grad_scaler.growth_tracker();
 continuation.ema_completed_updates = ema_completed_updates;
 continuation.training_attempt_id = std::string(attempt_id);
 continuation.training_original_descriptor = original_descriptor.string();
 detail::write_training_continuation(archive, options, continuation);
 optimizer.reserve_checkpoint(readback, model_state.size() + ema_state.size());
 {
  mmltk::common::logging::ScopedProfile profile_rfdetr_train_save_resume_write_state{"rfdetr.train.save.resume.write_state"};
  detail::write_resume_state_archive(archive, "state", model_state, readback, 0);
 }
 torch::serialize::OutputArchive optimizer_archive;
 {
  mmltk::common::logging::ScopedProfile profile_rfdetr_train_save_resume_optimizer_state{"rfdetr.train.save.resume.optimizer_state"};
  optimizer.save(optimizer_archive, readback, model_state.size() + ema_state.size());
 }
 archive.write("optimizer", optimizer_archive);
 if (options.use_ema) {
  {
   mmltk::common::logging::ScopedProfile profile_rfdetr_train_save_resume_ema_write_state{"rfdetr.train.save.resume.ema_write_state"};
   detail::write_resume_state_archive(archive, "ema_state", ema_state, readback, model_state.size());
  }
 }
 {
  mmltk::common::logging::ScopedProfile profile_rfdetr_train_save_resume_archive_save_to{"rfdetr.train.save.resume.archive_save_to"};
  readback.Complete();
  detail::publish_native_checkpoint_archive(archive, checkpoint_path, options.class_layout_path);
 }
}
ResumeState load_resume_checkpoint_state(const std::filesystem::path& checkpoint_path, const DecodedNativeModelState& admitted, const detail::TrainingContinuation& continuation, NativeOptimizer& optimizer,
 const std::vector<std::string>& parameter_names, const std::vector<torch::Tensor>& parameters, std::span<const std::uint8_t> active) {
 auto* retained = admitted.admitted_archive();
 if (!retained || !admitted.class_artifact) throw std::invalid_argument("full resume requires an admitted current native archive");
 admitted.class_artifact->RequireUnchanged();
 auto& archive = *retained;
 if (cli_enum_spelling(continuation.configuration.recipe.optimizer) != optimizer.kind_name())
  throw std::runtime_error("native RF-DETR resume checkpoint optimizer_kind does not match current training options");
 if (parameter_names.size() != parameters.size()) throw std::runtime_error("native RF-DETR active optimizer parameter inventory is inconsistent");
 ResumeState state;
 state.attempt_id = continuation.values.training_attempt_id;
 state.start_epoch = static_cast<int>(continuation.values.data.epoch);
 if (continuation.configuration.use_ema) {
  torch::serialize::InputArchive ema_archive;
  archive.read("ema_state", ema_archive);
  state.ema_cpu_shadow = detail::read_ema_shadow_archive(ema_archive, parameter_names);
  ModelEma::validate_cpu_shadow(parameters, *state.ema_cpu_shadow);
 }
 torch::serialize::InputArchive optimizer_archive;
 archive.read("optimizer", optimizer_archive);
 try {
  state.optimizer_candidate = optimizer.stage_load(optimizer_archive, active);
 } catch (const std::exception& error) { throw std::runtime_error("failed to load native RF-DETR optimizer state from " + checkpoint_path.string() + ": " + error.what()); }
 state.scaler_scale = static_cast<float>(continuation.values.grad_scaler_scale);
 state.scaler_growth_tracker = static_cast<int>(continuation.values.grad_scaler_growth_tracker);
 admitted.class_artifact->RequireUnchanged();
 return state;
}
void TrainingSnapshot::begin(const NativeRfDetrModel& model) {
 readback_.Begin();
 ordinary_ = collect_module_state(model);
 ema_.clear();
 detail::reserve_state_archive(ordinary_, readback_, 0);
}
void TrainingSnapshot::prepare_ema(const std::vector<std::string>& names, const ModelEma* ema) {
 if (ema) ema_ = ema_state_entries(names, *ema);
 detail::reserve_state_archive(ema_, readback_, ordinary_.size());
}
void TrainingSnapshot::save_weights(const std::filesystem::path& path, const NativeCheckpointMetadata& metadata, bool selected, const std::filesystem::path& descriptor) {
 static const std::vector<NormalizedModelStateEntry> no_ema;
 save_snapshot_checkpoint(path, metadata, ordinary_, selected ? ema_ : no_ema, readback_, descriptor);
}
void TrainingSnapshot::save_resume(const std::filesystem::path& path, const NativeCheckpointMetadata& metadata, const NativeOptimizer& optimizer, const GradScaler& scaler, const TrainRequest& options,
 int epoch, int64_t ema_completed_updates, std::string_view attempt_id, const std::filesystem::path& descriptor, detail::TrainingContinuationValues& continuation) {
 save_resume_checkpoint(path, metadata, optimizer, scaler, options, epoch, ema_completed_updates, ordinary_, ema_, attempt_id, descriptor, readback_, continuation);
}
void TrainingSnapshot::release() {
 readback_.Release();
 ordinary_.clear();
 ema_.clear();
}
}  // namespace mmltk::backend::models::rfdetr
