
#include "detail/training_continuation.h"
#include <cmath>
#include <cstring>
#include <meta>
#include <span>
#include <string_view>
#include <type_traits>
#include "src/backend/ml/torch/archive.h"
#include "detail/checkpoint_private.h"
#include "src/frameworks/serialization/reflected_cbor.h"
namespace mmltk::backend::models::rfdetr {
void validate_resume_continuation_manifest(const ResumeContinuationManifest& manifest) {
 if (manifest.ema_requested != manifest.ema_present) {
  throw std::runtime_error(manifest.ema_requested ? "native RF-DETR resume checkpoint is missing required EMA state" : "native RF-DETR resume checkpoint contains EMA state while EMA is disabled");
 }
 if (manifest.scaler_scale.has_value() != manifest.scaler_growth_tracker.has_value()) { throw std::runtime_error("native RF-DETR resume checkpoint gradient scaler state is incomplete"); }
 if ((manifest.scaler_scale.has_value() && (!std::isfinite(*manifest.scaler_scale) || *manifest.scaler_scale <= 0.0 || *manifest.scaler_scale > std::numeric_limits<float>::max())) ||
     (manifest.scaler_growth_tracker.has_value() && (*manifest.scaler_growth_tracker < 0 || *manifest.scaler_growth_tracker > static_cast<int64_t>(std::numeric_limits<int>::max())))) {
  throw std::runtime_error("native RF-DETR resume checkpoint gradient scaler state is invalid");
 }
}
namespace detail {
std::optional<TrainingContinuation> admit_training_configuration(NativeRfDetrConfig& config, torch::serialize::InputArchive* archive, const TrainRequest& options) {
 std::optional<TrainingContinuation> continuation;
 if (!options.resume_path.empty()) {
  if (!archive) throw std::runtime_error("--resume requires a native RF-DETR .pt checkpoint: " + options.resume_path.string());
  continuation = read_training_continuation(*archive);
  if (!continuation) throw std::runtime_error("--resume requires a full training checkpoint");
  require_active_training_continuation(*continuation, options);
 }
 if (!continuation) apply_stock_training_coefficients(config);
 config.training_supervision = options.training_supervision;
 return continuation;
}
namespace serialization = mmltk::frameworks::serialization;
namespace {
template <class Values, class Visitor>
void visit_continuation_values(Values& values, Visitor&& visitor) {
 template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^TrainingContinuationValues, std::meta::access_context::current()))) {
  visitor(std::define_static_string(std::meta::identifier_of(member)), values.[:member:]);
 }
}
// Convert only the genuinely different archive scalar representation. Comparing
// doubles before conversion back to float avoids accepting rounded forged facts.
template <class T>
auto archive_scalar(const T& value) {
 if constexpr (std::is_floating_point_v<T>)
  return static_cast<double>(value);
 else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
  return static_cast<int64_t>(value);
 else
  return value;
}
template <class T>
void write_scalar(torch::serialize::OutputArchive& archive, const char* key, const T& value) {
 const auto scalar = archive_scalar(value);
 using Scalar = std::remove_cvref_t<decltype(scalar)>;
 if constexpr (std::is_same_v<Scalar, std::string>)
  mmltk::backend::ml::serialization::write_string(archive, key, scalar);
 else if constexpr (std::is_same_v<Scalar, bool>)
  mmltk::backend::ml::serialization::write_bool(archive, key, scalar);
 else if constexpr (std::is_same_v<Scalar, int64_t>)
  mmltk::backend::ml::serialization::write_int(archive, key, scalar);
 else if constexpr (std::is_same_v<Scalar, double>)
  mmltk::backend::ml::serialization::write_double(archive, key, scalar);
 else {
  constexpr auto capacity = serialization::reflected_maximum_cbor_bytes<T>();
  serialization::wire::ByteBuffer bytes;
  const auto encoded = serialization::encode(value, bytes, {.max_bytes = capacity, .max_items = std::numeric_limits<std::uint64_t>::max(), .max_depth = 32});
  if (!encoded) throw std::runtime_error("invalid continuation structure");
  auto tensor = torch::empty({static_cast<int64_t>(bytes.size())}, torch::kUInt8);
  std::memcpy(tensor.data_ptr(), bytes.data(), bytes.size());
  archive.write(key, tensor);
 }
}
template <class T>
auto read_scalar(torch::serialize::InputArchive& archive, const char* key) {
 using Scalar = decltype(archive_scalar(T{}));
 if constexpr (std::is_arithmetic_v<Scalar> || std::same_as<Scalar, std::string>) {
  const auto value = mmltk::backend::ml::serialization::read_optional_value<Scalar>(archive, key);
  if (!value) throw std::runtime_error(std::string("full training checkpoint is missing ") + key);
  return *value;
 } else {
  const auto tensor = mmltk::backend::ml::serialization::require_tensor(archive, key);
  constexpr auto capacity = serialization::reflected_maximum_cbor_bytes<T>();
  if (!tensor.is_cpu() || tensor.scalar_type() != torch::kUInt8 || tensor.dim() != 1 || !tensor.is_contiguous() || static_cast<std::uint64_t>(tensor.numel()) > capacity)
   throw std::runtime_error("invalid continuation structure");
  auto value = serialization::decode<T>({std::span(reinterpret_cast<const std::byte*>(tensor.const_data_ptr()), static_cast<std::size_t>(tensor.numel())), {}}, {.max_bytes = capacity, .max_items = std::numeric_limits<std::uint64_t>::max(), .max_depth = 32});
  if (!value) throw std::runtime_error("invalid continuation values");
  return *value;
 }
}
void validate_values(const TrainingContinuationValues& values, const TrainRequest& request, bool present_ema) {
 const bool requested_ema = request.use_ema;
 if (values.epoch < 0 || values.epoch >= std::numeric_limits<int>::max()) throw std::runtime_error("invalid checkpoint epoch");
 if (values.ema_completed_updates < 0 || values.ema_completed_updates == std::numeric_limits<int64_t>::max() || (!requested_ema && values.ema_completed_updates != 0))
  throw std::runtime_error("invalid checkpoint EMA completed update count");
 if (std::isnan(values.best_regular_metric) || std::isnan(values.best_ema_metric)) throw std::runtime_error("checkpoint best metric is invalid");
 if (values.training_attempt_id.empty() || values.training_attempt_id.size() > 64) throw std::runtime_error("invalid checkpoint attempt identity");
 if (values.training_original_descriptor.size() > mmltk::frameworks::reflection::kMaximumPathBytes) throw std::runtime_error("invalid original checkpoint descriptor provenance");
 validate_resume_continuation_manifest({requested_ema, present_ema, values.grad_scaler_scale, values.grad_scaler_growth_tracker});
 const auto expected = derive_execution_facts(request, values.execution.settings_revision);
 if (values.execution.logical_models != expected.logical_models || values.execution.microbatches_per_attempt != expected.microbatches_per_attempt ||
     values.execution.effective_batch_per_model != expected.effective_batch_per_model || values.execution.aggregate_round_images != expected.aggregate_round_images ||
     values.execution.configured_capacity != expected.configured_capacity) throw std::runtime_error("checkpoint execution products differ from saved request");
 const auto donor_streams = request.lane_configuration.mode == TrainLaneMode::SharedGradients ? request.lanes : 1;
 if (!values.data.plan_hash || values.data.shards.size() != expected.logical_models || values.data.donors.size() != checked_training_product(request.batch_size, donor_streams) ||
     values.data.epoch < static_cast<std::uint64_t>(values.epoch) || values.data.epoch > static_cast<std::uint64_t>(values.epoch) + 1 ||
     (values.data.epoch > static_cast<std::uint64_t>(values.epoch) && values.data.next_microbatch != 0) ||
     values.data.next_microbatch % expected.microbatches_per_attempt) throw std::runtime_error("invalid checkpoint logical data continuation");
 if (values.data.next_microbatch && (values.epoch_metrics.microbatches != values.data.next_microbatch || !std::isfinite(values.epoch_metrics.loss_sum) || !std::isfinite(values.epoch_metrics.class_loss_sum) || !std::isfinite(values.epoch_metrics.box_loss_sum)))
  throw std::runtime_error("checkpoint epoch metric accumulation differs from data cursor");
 const auto& clock = values.schedule;
 if (!clock.epoch_event_applied || clock.epoch != static_cast<std::uint64_t>(values.epoch) || clock.nb_ref % expected.microbatches_per_attempt ||
     clock.steps_ref != clock.nb_ref / expected.microbatches_per_attempt || clock.consumed_microbatches % expected.microbatches_per_attempt || clock.consumed_attempts != clock.consumed_microbatches / expected.microbatches_per_attempt)
  throw std::runtime_error("inconsistent checkpoint scheduler clocks");
 TrainingSchedule schedule(request.recipe, clock.absolute_lrs, std::vector<TrainingGroupRole>(clock.absolute_lrs.size(), TrainingGroupRole::Ordinary), request.epochs,
  clock.nb_ref, expected.microbatches_per_attempt);
 schedule.restore(clock);
}
bool has_continuation_fields(torch::serialize::InputArchive& archive) {
 bool present = false;
 const auto probe = [&](const char* key, const auto&...) {
  c10::IValue value;
  present = archive.try_read(key, value) || present;
 };
 TrainingContinuationValues values;
 visit_continuation_values(values, probe);
 probe("optimizer");
 probe("ema_state");
 probe("training_configuration_cbor");
 probe("training_supervision_config_cbor");
 return present;
}
}  // namespace
void write_training_configuration(torch::serialize::OutputArchive& archive, const TrainRequest& request) {
 constexpr auto capacity = serialization::reflected_maximum_cbor_bytes<TrainRequest>();
 serialization::wire::ByteBuffer bytes;
 const auto encoded = serialization::encode(request, bytes, {.max_bytes = capacity, .max_items = 4096, .max_depth = 32});
 if (!encoded) throw std::runtime_error("training configuration violates its checkpoint schema");
 auto tensor = torch::empty({static_cast<int64_t>(bytes.size())}, torch::kUInt8);
 std::memcpy(tensor.data_ptr(), bytes.data(), bytes.size());
 archive.write("training_configuration_cbor", tensor);
}
TrainRequest read_training_configuration(torch::serialize::InputArchive& archive) {
 torch::Tensor configuration;
 if (!archive.try_read("training_configuration_cbor", configuration)) throw std::runtime_error("full checkpoint is missing current saved training configuration");
 constexpr auto capacity = serialization::reflected_maximum_cbor_bytes<TrainRequest>();
 if (!configuration.defined() || !configuration.is_cpu() || configuration.scalar_type() != torch::kUInt8 || configuration.dim() != 1 || configuration.numel() <= 0 ||
     static_cast<std::size_t>(configuration.numel()) > capacity)
  throw std::runtime_error("invalid checkpoint training configuration");
 configuration = configuration.contiguous();
 const auto request = serialization::decode<TrainRequest>(
  {std::span(reinterpret_cast<const std::byte*>(configuration.const_data_ptr()), static_cast<std::size_t>(configuration.numel())), {}}, {.max_bytes = capacity, .max_items = 4096, .max_depth = 32});
 if (!request) throw std::runtime_error("invalid checkpoint training configuration values");
 validate_train_request(*request);
 return *request;
}
void write_training_continuation(torch::serialize::OutputArchive& archive, const TrainRequest& request, const TrainingContinuationValues& values) {
 validate_train_request(request);
 validate_values(values, request, request.use_ema);
 write_training_configuration(archive, request);
 write_training_supervision_config(archive, request.training_supervision);
 const auto write = [&](const char* key, const auto& value) { write_scalar(archive, key, value); };
 visit_continuation_values(values, write);

}
std::optional<TrainingContinuation> read_training_continuation(torch::serialize::InputArchive& archive) {
 torch::serialize::InputArchive optimizer;
 if (!archive.try_read("optimizer", optimizer)) {
  if (has_continuation_fields(archive)) throw std::runtime_error("full training checkpoint is missing optimizer continuation");
  return std::nullopt;
 }
 TrainingContinuation result;
 result.configuration = read_training_configuration(archive);
 visit_continuation_values(result.values, [&]<class T>(const char* key, T& value) { value = read_scalar<T>(archive, key); });
 require_resume_training_supervision_config(archive, result.configuration.training_supervision);
 c10::IValue ema_value;
 const bool has_ema = archive.try_read("ema_state", ema_value);
 torch::serialize::InputArchive ema;
 if (has_ema && !archive.try_read("ema_state", ema)) throw std::runtime_error("checkpoint EMA continuation is not an archive");
 validate_values(result.values, result.configuration, has_ema);
 return result;
}
void require_active_training_continuation(const TrainingContinuation& saved, const TrainRequest& active) {
 if (active.epochs < saved.configuration.epochs) throw std::runtime_error("Resume cannot shorten the epoch horizon");
 const auto& prior = saved.configuration;
 if (prior.recipe != active.recipe || prior.lane_configuration != active.lane_configuration || prior.data_policy != active.data_policy ||
     prior.batch_size != active.batch_size || prior.grad_accum_steps != active.grad_accum_steps || prior.lanes != active.lanes || prior.seed != active.seed ||
     prior.unfreeze_encoder_last_epochs != active.unfreeze_encoder_last_epochs || prior.disable_augmentation_last_epochs != active.disable_augmentation_last_epochs ||
     prior.freeze_encoder != active.freeze_encoder || prior.gpu_augmentation != active.gpu_augmentation)
  throw std::runtime_error("Resume recipe, logical data, or final epoch policy differs from saved configuration");
 if (saved.configuration.use_ema != active.use_ema) throw std::runtime_error("resume EMA selection differs from the saved training configuration");
 if (saved.configuration.training_supervision != active.training_supervision) throw std::runtime_error("native RF-DETR resume checkpoint training supervision configuration does not match");
}
}  // namespace detail
}  // namespace mmltk::backend::models::rfdetr
