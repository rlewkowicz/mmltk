#include "src/controller/subsystems/system/model_system.h"
#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>
#include "src/controller/contracts/compute.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/core/class_artifact.h"
#include "src/backend/models/rfdetr/core/model_info.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
import mmltk.backend.models.rfdetr.model_export;
import mmltk.backend.models.rfdetr.inference.runtime_backend;
#include "src/controller/subsystems/system/compute_intent_materializer.h"
namespace mmltk::controller {
ModelArtifactAdmission ArtifactModelRuntime::Acquire(const contracts::ModelSelectionKey& key, const std::filesystem::path& custom, const int inspection_device, const std::stop_token stop,
 const std::function<void(const contracts::ModelProgress&)>& progress) {
 if (!key.valid()) throw std::invalid_argument("model selection key is invalid");
 namespace rfdetr = mmltk::backend::models::rfdetr;
 const auto* preset = key.from_scratch ? rfdetr::find_preset_catalog_entry(key.preset) : nullptr;
 if (key.from_scratch && !preset) throw std::invalid_argument("unknown RF-DETR preset");
 std::filesystem::path artifact;
 if (key.source == contracts::ModelSelectionSource::Custom) {
  if (stop.stop_requested()) throw std::runtime_error("model selection cancelled");
  progress({.stage = contracts::ModelProgressStage::Verifying, .activity = "Verifying selected model artifact"});
  std::error_code error;
  if (custom.empty() || !std::filesystem::is_regular_file(custom, error) || error) throw std::runtime_error("selected model artifact is unavailable");
  if (stop.stop_requested()) throw std::runtime_error("model selection cancelled");
  artifact = custom;
 } else {
  mmltk::common::concurrency::ScopedEventCancellation<services::ArtifactCancellationSource> cancellation{stop};
  // CLEANUP-IGNORE: Artifact weight and dataset progress observers adapt distinct typed service callbacks at their
  // respective runtime boundaries.
  const services::ArtifactWeightProgressObserver observer{
   .context = const_cast<std::function<void(const contracts::ModelProgress&)>*>(&progress), .report = [](void* context, const contracts::ModelProgress& value) noexcept {
   try {
    (*static_cast<std::function<void(const contracts::ModelProgress&)>*>(context))(value);
   } catch (...) {}
  }
  };
  artifact = key.from_scratch ? store_.canonical_encoder_weight_path(preset->backbone.kind, cancellation.token(), observer)
                              : store_.canonical_weight_path(key.preset, cancellation.token(), observer);
 }
 if (stop.stop_requested()) throw std::runtime_error("model selection cancelled");
 if (key.from_scratch) {
  auto config = rfdetr::native_config_from_preset(*preset);
  config.resolution = static_cast<int>(key.resolution);
  // Dataset admission establishes output meaning and query sizing at launch.
  config.num_classes = 0;
  config.keypoints_per_class.clear();
  return {.artifact = artifact.string(), .task = preset->task, .training_config = std::move(config),
          .training_capabilities = preset->training_capabilities,
          .encoder_pretraining = rfdetr::encoder_pretraining_provenance(*rfdetr::find_encoder_pretraining_asset(preset->backbone.kind))};
 }
 rfdetr::ModelClassLayout layout;
 rfdetr::ModelTask task = rfdetr::ModelTask::Detection;
 std::optional<rfdetr::NativeRfDetrConfig> training_config;
 std::optional<rfdetr::EncoderPretrainingProvenance> encoder_pretraining;
 if (key.input == contracts::ModelArtifactInputKind::Weights) {
  auto resolved = rfdetr::resolve_model_state(artifact, key.preset, key.resolution, key.class_layout_path, {}, stop);
  encoder_pretraining = resolved.artifacts.encoder_pretraining;
  layout = std::move(resolved.artifacts.class_layout);
  training_config = std::move(resolved.artifacts.config);
  task = training_config->task;
 } else if (key.input == contracts::ModelArtifactInputKind::Onnx) {
  const rfdetr::ClassArtifactAdmission admission(artifact, key.class_layout_path, {}, stop);
  const auto info = rfdetr::load_onnx_model_info(artifact, admission.output_roles(), admission.output_contract());
  layout = admission.Resolve(info.num_classes, info.class_layout, stop);
  task = info.output_contract ? info.output_contract->task : info.has_masks ? rfdetr::ModelTask::Segmentation : rfdetr::ModelTask::Detection;
 } else {
  rfdetr::ModelArtifactRequest request;
  request.tensorrt_path = artifact;
  request.class_layout_path = key.class_layout_path;
  const auto info = rfdetr::inspect_tensorrt_model(request, inspection_device, stop);
  if (!info.class_layout) throw std::runtime_error("TensorRT class inspection is unavailable");
  layout = *info.class_layout;
  task = info.output_contract ? info.output_contract->task : info.has_masks ? rfdetr::ModelTask::Segmentation : rfdetr::ModelTask::Detection;
 }
 if (stop.stop_requested()) throw std::runtime_error("model selection cancelled");
 const auto capabilities = training_config ? rfdetr::model_training_capabilities(training_config->task, training_config->dec_layers) : rfdetr::PresetTrainingCapabilities{};
 return {
  .artifact = artifact.string(),
  .class_layout = rfdetr::ResolvedClassLayout(std::move(layout)).summary(),
  .task = task,
  .training_config = std::move(training_config),
  .training_capabilities = capabilities,
  .encoder_pretraining = std::move(encoder_pretraining)
 };
}
ModelSystem::ModelSystem(SettingsSystem& settings, RuntimeFactory factory, SystemEventSink<event_type> events) : settings_(settings), factory_(std::move(factory)), events_(std::move(events)) {
 if (!factory_) throw contracts::UnavailableError("model runtime factory is unavailable");
}
ModelSystem::~ModelSystem() = default;
direct::LocalRun::Notification ModelSystem::changed(contracts::ModelUiState settled) {
 return [this, settled = std::move(settled)]() mutable noexcept { direct::PublishLazyNoexcept(events_, [&] { return event_type{ModelChanged{std::move(settled)}}; }); };
}
contracts::ModelUiState ModelSystem::Select(const contracts::ModelSelectionRequest request) {
 if (!request.valid()) throw contracts::InvalidIntentError("model workflow is invalid");
 {
  std::scoped_lock lock(mutex_);
  if (state_.active) throw contracts::BusyError("model selection is active");
 }
 const auto settings = settings_.materialization_facts();
 if (!settings.loaded || settings.revision == 0U) throw contracts::UnavailableError("settings are unavailable");
 if (request.workflow == contracts::FeatureId::Export && !settings.settings.workflows.export_state.export_onnx && !settings.settings.workflows.export_state.build_tensorrt)
  throw contracts::InvalidIntentError("Select ONNX or TensorRT for export");
 auto input = subsystems::system::ComputeIntentMaterializer::ModelInputFor(settings.settings, request.workflow);
 if (!input) throw contracts::InvalidIntentError(input.error().detail);
 run_.Start({
  .prepare =
   [this] {
  std::scoped_lock lock(mutex_);
  if (state_.active) throw contracts::BusyError("model selection is active");
  const auto next = contracts::next_compute_generation(state_.generation);
  if (!next) throw contracts::FailedError("model selection generation exhausted");
  state_.generation = *next;
  state_.active = true;
  state_.progress = {};
  state_.terminal = {};
 },
  .work = [this, input = std::move(*input)](const std::stop_token stop) mutable -> direct::LocalRun::Notification {
  contracts::ModelSelection selection;
  contracts::ModelSelectionResult terminal;
  bool failed = false;
  std::atomic_bool malformed_progress = false;
  try {
   if (!runtime_) runtime_ = factory_();
   if (!runtime_) throw std::runtime_error("model runtime is unavailable");
   auto artifact = runtime_->Acquire(input.key, input.custom_artifact, input.inspection_device.value_or(0), stop, [this, &malformed_progress](const contracts::ModelProgress& value) {
    if (!value.valid()) {
     malformed_progress.store(true, std::memory_order_relaxed);
    } else {
     progress(value);
    }
   });
   if (malformed_progress.load(std::memory_order_relaxed)) throw std::runtime_error("model runtime returned invalid progress");
   if (stop.stop_requested()) {
    terminal.outcome = contracts::ModelSelectionOutcome::Cancelled;
   } else {
    selection = {
     .key = std::move(input.key),
     .inspection_device = input.inspection_device,
     .artifact = std::move(artifact.artifact),
     .class_layout = std::move(artifact.class_layout),
     .task = artifact.task,
     .training_config = std::move(artifact.training_config),
     .training_capabilities = artifact.training_capabilities,
     .encoder_pretraining = std::move(artifact.encoder_pretraining)
    };
    if (!selection.valid()) throw std::runtime_error("model runtime returned an invalid selection");
    terminal.outcome = contracts::ModelSelectionOutcome::Accepted;
   }
  } catch (const std::exception& error) {
   if (stop.stop_requested()) {
    terminal.outcome = contracts::ModelSelectionOutcome::Cancelled;
   } else {
    failed = true;
    terminal = {.outcome = contracts::ModelSelectionOutcome::Rejected, .detail = contracts::bounded_model_detail(error.what())};
   }
  } catch (...) {
   if (stop.stop_requested()) {
    terminal.outcome = contracts::ModelSelectionOutcome::Cancelled;
   } else {
    failed = true;
    terminal = {.outcome = contracts::ModelSelectionOutcome::Rejected, .detail = "model selection failed"};
   }
  }
  contracts::ModelUiState settled;
  {
   std::scoped_lock lock(mutex_);
   state_.active = false;
   state_.progress = {};
   state_.terminal = std::move(terminal);
   if (state_.terminal.outcome == contracts::ModelSelectionOutcome::Accepted)
    state_.selection = std::move(selection);
   else
    state_.selection = {};
   settled = state_;
  }
  if (failed) runtime_.reset();
  return changed(std::move(settled));
 },
  .failure = [this](std::exception_ptr) -> direct::LocalRun::Notification {
  runtime_.reset();
  contracts::ModelUiState settled;
  {
   std::scoped_lock lock(mutex_);
   state_.active = false;
   state_.progress = {};
   state_.selection = {};
   state_.terminal = {.outcome = contracts::ModelSelectionOutcome::Rejected, .detail = "model worker failed"};
   settled = state_;
   // CLEANUP-IGNORE: Model failure settlement and cancellation use model-selection terminal facts;
   // Dataset uses artifact inspection and compile facts.
  }
  return changed(std::move(settled));
 },
 });
 return snapshot();
}
contracts::ModelUiState ModelSystem::Stop() noexcept {
 static_cast<void>(run_.Stop());
 std::scoped_lock lock(mutex_);
 if (state_.active) state_.terminal = {.outcome = contracts::ModelSelectionOutcome::CancellationRequested, .detail = "cancellation requested"};
 return state_;
}
void ModelSystem::Shutdown() noexcept {
 static_cast<void>(Stop());
 run_.StopAndJoin();
}
contracts::ModelUiState ModelSystem::snapshot() const {
 std::scoped_lock lock(mutex_);
 return state_;
}
contracts::ModelSelection ModelSystem::selection() const {
 std::scoped_lock lock(mutex_);
 return state_.selection;
}
void ModelSystem::progress(const contracts::ModelProgress& value) noexcept {
 ModelProgressChanged event;
 {
  std::scoped_lock lock(mutex_);
  if (!state_.active) return;
  state_.progress = value;
  event = {.generation = state_.generation, .active = state_.active, .terminal = state_.terminal, .progress = value};
 }
 direct::PublishLazyNoexcept(events_, [&] { return event_type{std::move(event)}; });
}
}  // namespace mmltk::controller
