#include "validation_system.h"
#include "src/controller/services/run_output.h"
#include "validation_runtime.h"
#include "detail/validation_samples.h"
#include "detail/validation_sample_output.h"
#include <cmath>
#include "src/controller/presentation/workspace_input.h"
#include <algorithm>
#include <exception>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <utility>
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/subsystems/system/model_system.h"
#include "src/controller/subsystems/system/detail/cuda_runtime_resources.h"
#include "src/controller/subsystems/system/compute_intent_materializer.h"
#include "src/controller/runtime/local_run.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/gui_settings_mutation.h"
#include "src/common/system/execution_policy.h"
namespace mmltk::controller {
class CudaValidationRuntime::Impl final : public detail::CudaSessionRuntimeState<mmltk::backend::models::rfdetr::ValidationSession> {
public:
 using CudaSessionRuntimeState::CudaSessionRuntimeState;
};
CudaValidationRuntime::CudaValidationRuntime(DirectComputeConfiguration c) : impl_(std::make_unique<Impl>(c)) {}
CudaValidationRuntime::~CudaValidationRuntime() = default;
ValidationRuntimeResult CudaValidationRuntime::Run(
 mmltk::backend::models::rfdetr::ValidateRequest operation, std::stop_token stop, const ComputeProgressSink& progress, const mmltk::backend::models::rfdetr::ValidationDelivery& delivery) {
 ValidationRuntimeResult result;
 try {
  result.terminal = impl_->resources.Run(
   [this, &result, &progress, &delivery, stop, operation = std::move(operation)](const mmltk::backend::ml::runtime::BorrowedCommandStream stream) mutable {
    operation.device_id = impl_->resources.device();
    operation.compile_cuda_device_id = impl_->resources.device();
    operation = mmltk::backend::models::rfdetr::finalize_validate_request(std::move(operation));
    std::uint64_t sequence = 0U;
    auto callbacks = delivery;
    callbacks.stop = stop;
    callbacks.progress = [&](std::size_t completed, std::size_t total) {
     if (progress) progress({++sequence, completed, total, "Validating"});
    };
    auto evaluated = impl_->session.Run(operation, stream, callbacks);
    if (evaluated.backends.size() > 1U) throw std::logic_error("GUI validation requires one selected backend");
    std::exception_ptr report_failure;
    if (!evaluated.cancelled && operation.write_report_json && !operation.report_json_path.empty()) {
     try {
      mmltk::backend::models::rfdetr::write_validation_report(operation, evaluated);
      result.report = operation.report_json_path;
     } catch (...) { report_failure = std::current_exception(); }
    }
    if (!evaluated.backends.empty()) result.evaluation = std::move(evaluated.backends.begin()->second);
    if (report_failure) std::rethrow_exception(report_failure);
    return contracts::make_compute_terminal(evaluated.cancelled ? contracts::ComputeOperationOutcome::Cancelled : contracts::ComputeOperationOutcome::Succeeded, 0U, evaluated.processed_images);
   },
   stop);
 } catch (...) { result.terminal = contracts::compute_failure_terminal(std::current_exception(), "validation runtime failed"); }
 return result;
}
class ValidationSystem::Impl final {
public:
 Impl(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model, ValidationRuntimeFactory factory, SystemEventSink<ValidationSystem::event_type> events,
  std::optional<mmltk::frameworks::gpu::DeviceExecution> execution, VisualDeviceSettings visual, mmltk::backend::imaging::raster::RenderedImageWriter::PngEncoder encoder)
     : settings_(settings),
       dataset_(dataset),
       model_(model),
       factory_(std::move(factory)),
       events_(std::move(events)),
       configuration_{std::move(execution)},
       previews_(visual.valid()),
       samples_(visual, [this] { Changed(); }),
       sample_output_(
        configuration_, visual,
        [this](const auto& path) {
         {
          std::scoped_lock lock(mutex_);
          state_.output.samples_directory = path.parent_path().string();
          ++state_.output.completed_samples;
          state_.output.recent_sample = path.string();
         }
         Changed();
        },
        std::move(encoder)) {
  if (!factory_) throw contracts::UnavailableError("compute runtime factory is unavailable");
  samples_.SetDisplay(settings_.validation_display_settings());
 }
 mmltk::backend::models::rfdetr::ValidationDelivery Delivery(std::uint64_t generation, const std::filesystem::path& directory, contracts::ValidationRunPreview preview) {
  mmltk::backend::models::rfdetr::ValidationDelivery delivery;
  delivery.samples_selected = [this, generation, directory, preview](auto indices, auto) {
   sample_output_.Begin(directory, preview, indices);
   if (previews_) {
    try {
     sample_output_.UseCaptureContext(samples_.CaptureContext());
     samples_.Begin(generation, indices);
    } catch (const mmltk::backend::ml::runtime::CudaOperationError&) { throw; } catch (...) { /* Interactive sample admission remains optional. */
    }
   }
  };
  delivery.sample = [this, generation](auto sample) {
   auto raw = sample_output_.Capture(sample);
   if (previews_ && raw) {
    try {
     samples_.Adopt(generation, std::move(sample), std::move(raw));
    } catch (const mmltk::backend::ml::runtime::CudaOperationError&) { throw; } catch (...) { /* Required output and metrics do not depend on display adoption. */
    }
   }
  };
  return delivery;
 }
 ~Impl() { Shutdown(); }
 [[nodiscard]] contracts::ComputeUiState Start(contracts::ValidationRunPreview preview) {
  if (!std::isfinite(preview.display.confidence_threshold) || preview.display.confidence_threshold < 0.0F || preview.display.confidence_threshold > 1.0F)
   throw contracts::InvalidIntentError("validation preview confidence must be between zero and one");
  const auto settings = settings_.materialization_facts();
  if (!settings.loaded) throw contracts::UnavailableError("settings are unavailable");
  const auto selection = model_.selection();
  run_.Start({
   .policy = configuration_.worker_policy(),
   .prepare =
    [this] {
     std::scoped_lock lock(mutex_);
     const auto next = contracts::next_compute_generation(state_.generation_frontier);
     if (!next) throw contracts::FailedError("compute operation generation exhausted");
     contracts::begin_compute(state_, *next, "Inspecting selected inputs");
    },
   .work = [this, settings = settings.settings, selection, preview](const std::stop_token stop) mutable -> direct::LocalRun::Notification {
    const auto generation = operation().generation_frontier;
    auto terminal = run_checked_compute(
     [&](const ComputeProgressSink& progress) {
      if (stop.stop_requested()) return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
      const auto inspection = dataset_.Inspect({contracts::resolve_validation_source(settings), {}, {}}, selection.key.preset, selection.key.resolution, stop);
      if (stop.stop_requested()) return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
      auto prepared = subsystems::system::ComputeIntentMaterializer::Validation(settings, inspection, selection);
      if (!prepared) throw contracts::InvalidIntentError(prepared.error().detail);
      if (stop.stop_requested()) return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
      const auto& output = settings.workflows.validate.output;
      const auto directory = services::reserve_run_output(contracts::workflow_output_root(output, contracts::FeatureId::Validate), !output.automatic);
      if (!prepared->report_json_path.empty()) prepared->report_json_path = directory / prepared->report_json_path.filename();
      {
       std::scoped_lock lock(mutex_);
       state_.output.directory = directory.string();
      }
      Changed();
      if (!runtime_) runtime_ = factory_();
      if (!runtime_) throw std::runtime_error("compute runtime is unavailable");
      auto delivery = Delivery(generation, directory, preview);
      auto result = [&] {
       try {
        return runtime_->Run(std::move(*prepared), stop, progress, delivery);
       } catch (...) {
        const auto failure = std::current_exception();
        // Publish any completed selected image before this run becomes terminal.
        // A secondary save failure must not replace the runtime's original error.
        try {
         sample_output_.Finish(false);
        } catch (...) {}
        std::rethrow_exception(failure);
       }
      }();
      {
       std::scoped_lock lock(mutex_);
       if (!result.report.empty()) state_.output.artifacts.push_back(std::move(result.report));
       evaluation_ = std::move(result.evaluation);
       evaluation_generation_ = state_.generation_frontier;
      }
      try {
       sample_output_.Finish(result.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
      } catch (...) {
       auto failure = contracts::compute_failure_terminal(std::current_exception(), "validation sample output failed");
       failure.completed = result.terminal.completed;
       return failure;
      }
      return std::move(result.terminal);
     },
     [this](const contracts::ComputeProgress& progress) { Progress(progress); });
    if (terminal.outcome == contracts::ComputeOperationOutcome::Failed) runtime_.reset();
    return Complete(generation, std::move(terminal));
   },
   .failure = [this](const std::exception_ptr failure) -> direct::LocalRun::Notification {
    runtime_.reset();
    return Complete(operation().generation_frontier, contracts::compute_failure_terminal(failure, "compute worker failed"));
   },
  });
  return operation();
 }
 [[nodiscard]] contracts::ComputeUiState Stop() noexcept {
  static_cast<void>(run_.Stop());
  std::scoped_lock lock(mutex_);
  contracts::cancel_compute(state_);
  return state_;
 }
 void Shutdown() noexcept {
  static_cast<void>(Stop());
  run_.StopAndJoin();
  samples_.Shutdown();
 }
 [[nodiscard]] contracts::ComputeUiState operation() const {
  std::scoped_lock lock(mutex_);
  return state_;
 }
 [[nodiscard]] direct::LocalRun::Notification Complete(std::uint64_t generation, contracts::ComputeTerminal terminal) {
  samples_.Settle(generation, terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
  {
   std::scoped_lock lock(mutex_);
   if (generation != state_.generation_frontier) return {};
   terminal.generation = generation;
   contracts::complete_compute(state_, std::move(terminal));
  }
  return [this]() mutable noexcept { Changed(); };
 }
 void Progress(const contracts::ComputeProgress& progress) noexcept {
  contracts::ComputeUiState snapshot;
  {
   std::scoped_lock lock(mutex_);
   if (!contracts::advance_compute(state_, progress)) return;
   snapshot = state_;
  }
  direct::PublishLazyNoexcept(events_, [&] { return ValidationSystem::event_type{ValidationProgress{std::move(snapshot)}}; });
 }
 ValidationSnapshot snapshot() const {
  auto result = samples_.snapshot();
  std::scoped_lock lock(mutex_);
  result.operation = state_;
  if (evaluation_ && evaluation_generation_ == state_.generation_frontier) {
   result.metrics = evaluation_->summary;
   result.detail_rows = static_cast<std::uint32_t>(evaluation_->details.size());
  }
  return result;
 }
 void Changed() noexcept {
  direct::PublishLazyNoexcept(events_, [&] { return ValidationSystem::event_type{ValidationChanged{snapshot()}}; });
 }
 SettingsSystem& settings_;
 DatasetSystem& dataset_;
 ModelSystem& model_;
 ValidationRuntimeFactory factory_;
 SystemEventSink<ValidationSystem::event_type> events_;
 mutable std::mutex mutex_;
 contracts::ComputeUiState state_{};
 std::unique_ptr<ValidationRuntime> runtime_;
 DirectComputeConfiguration configuration_;
 direct::LocalRun run_;
 std::optional<mmltk::backend::models::rfdetr::ValidationBackendResult> evaluation_;
 std::uint64_t evaluation_generation_ = 0U;
 WorkspaceInput input_;
 bool previews_ = false;
 detail::ValidationSamples samples_;
 detail::ValidationSampleOutput sample_output_;
};
ValidationSystem::ValidationSystem(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model, ValidationRuntimeFactory factory, SystemEventSink<event_type> events,
 std::optional<mmltk::frameworks::gpu::DeviceExecution> execution, VisualDeviceSettings visual, mmltk::backend::imaging::raster::RenderedImageWriter::PngEncoder encoder)
    : impl_(std::make_unique<Impl>(settings, dataset, model, std::move(factory), std::move(events), std::move(execution), visual, std::move(encoder))) {}
ValidationSystem::~ValidationSystem() = default;
ValidationSnapshot ValidationSystem::Start(contracts::ValidateWorkflowIntent intent) {
 static_cast<void>(impl_->Start(intent.preview));
 return snapshot();
}
ValidationSnapshot ValidationSystem::Stop() noexcept {
 static_cast<void>(impl_->Stop());
 return snapshot();
}
ValidationSnapshot ValidationSystem::SelectSample(ValidationSampleIdentity identity) {
 impl_->samples_.Select(identity);
 return snapshot();
}
ValidationSnapshot ValidationSystem::CloseDetail() {
 impl_->samples_.CloseDetail();
 return snapshot();
}
void ValidationSystem::DisplaySettingsChanged() noexcept {
 try {
  impl_->samples_.SetDisplay(impl_->settings_.validation_display_settings());
 } catch (const std::exception& error) { std::fprintf(stderr, "Validation display settings failed: %s\n", error.what()); } catch (...) {
  std::fputs("Validation display settings failed: unknown error\n", stderr);
 }
}
ValidationSnapshot ValidationSystem::SetOverlays(ValidationOverlays overlays) {
 impl_->samples_.SetOverlays(overlays);
 return snapshot();
}
mmltk::backend::models::rfdetr::EvaluationDetailPage ValidationSystem::Details(mmltk::backend::models::rfdetr::EvaluationDetailQuery query) const {
 namespace rfdetr = mmltk::backend::models::rfdetr;
 if (query.count == 0U || query.count > rfdetr::kEvaluationDetailPageSize) throw contracts::InvalidIntentError("validation detail page is too large");
 std::scoped_lock lock(impl_->mutex_);
 if (!impl_->evaluation_ || query.generation != impl_->evaluation_generation_ || query.generation != impl_->state_.generation_frontier)
  throw contracts::InvalidIntentError("validation metric query is stale");
 const auto& rows = impl_->evaluation_->details;
 if (query.offset > rows.size()) throw contracts::InvalidIntentError("validation metric offset is invalid");
 rfdetr::EvaluationDetailPage result{query.generation, static_cast<std::uint32_t>(rows.size()), query.offset, {}};
 const auto count = std::min<std::size_t>(query.count, rows.size() - query.offset);
 result.rows.assign(rows.begin() + query.offset, rows.begin() + query.offset + count);
 for (auto& row : result.rows) {
  if (row.category) {
   const auto& catalog = impl_->evaluation_->class_catalog;
   if (!catalog || *row.category >= catalog->size()) throw std::logic_error("validation detail catalog is unavailable");
   row.category_name = mmltk::backend::data::catalog::ClassName{catalog->names()[*row.category]};
  }
 }
 return result;
}
void ValidationSystem::Shutdown() noexcept { impl_->Shutdown(); }
ValidationSnapshot ValidationSystem::snapshot() const { return impl_->snapshot(); }
std::optional<ValidationImageMetadata> ValidationSystem::ImageSnapshot(const VisualFrame& frame) const { return impl_->samples_.ImageSnapshot(frame); }
VisualSourceObservation ValidationSystem::ObserveSource() const {
 const auto image = impl_->samples_.snapshot();
 return {image.frame, image.frame.revision};
}
VisualDocumentRead ValidationSystem::BorrowDocument(const VisualFrame& frame) const { return impl_->samples_.BorrowDocument(frame); }
mmltk::frameworks::gpu::BorrowedImageProductReadView ValidationSystem::BorrowFrame() const { return impl_->samples_.BorrowFrame(); }
mmltk::frameworks::gpu::BorrowedImageWorkspace ValidationSystem::BorrowWorkspace() const { return impl_->samples_.BorrowWorkspace(); }
mmltk::frameworks::gpu::ImageWorkspaceObservation ValidationSystem::ObserveWorkspace() const { return impl_->samples_.ObserveWorkspace(); }
void ValidationSystem::RequestWorkspace(VisualWorkspaceRequest request) { impl_->samples_.RequestWorkspace(std::move(request)); }
void ValidationSystem::Input(WorkspaceMouse mouse) {
 std::scoped_lock lock(impl_->mutex_);
 impl_->input_.Accept(std::move(mouse), PresentationSourceKind::Validation);
}
void ValidationSystem::SetInputPeer(std::uint64_t epoch) {
 std::scoped_lock lock(impl_->mutex_);
 impl_->input_.SetPeer(epoch);
}
}  // namespace mmltk::controller
