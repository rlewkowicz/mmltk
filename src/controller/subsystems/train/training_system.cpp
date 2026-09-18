#include "src/controller/subsystems/train/training_system.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <future>
#include <thread>
#include "src/common/io/file_digest.h"
#include <ranges>
#include <stdexcept>
#include <utility>
#include "src/common/concurrency/event_cancellation.h"
#include "src/controller/services/train_process_client.h"
#include "src/controller/services/train_run_store.h"
#include "src/backend/models/rfdetr/training/checkpoint.h"
#include "src/backend/models/rfdetr/core/artifact_publication.h"
#include "src/controller/subsystems/system/compute_intent_materializer.h"
namespace mmltk::controller {
namespace {
[[nodiscard]] contracts::ProviderEffectResult reconcile_result(const services::VastReconciliation& result, const int fallback) {
    const int instance = result.instance ? result.instance->instance_id : fallback;
    using Source = services::VastReconciliation::Disposition;
    using Target = contracts::ProviderReconciliationDisposition;
    if (result.disposition == Source::Applied) return {.disposition = Target::Applied, .instance_id = instance};
    if (result.disposition == Source::NotApplied) return {.disposition = Target::NotApplied};
    return {.disposition = Target::Inconclusive, .instance_id = instance};
}
}  // namespace
mmltk::backend::models::rfdetr::TrainingCheckpoint TrainingRuntime::InspectCheckpoint(const std::filesystem::path& path, std::stop_token stop) {
    return mmltk::backend::models::rfdetr::inspect_training_checkpoint(path, stop);
}
NativeTrainingRuntime::NativeTrainingRuntime(NativeTrainingConfiguration configuration) : config_(std::move(configuration)) {}
contracts::ComputeTerminal NativeTrainingRuntime::Train(mmltk::backend::models::rfdetr::TrainRequest request, const std::stop_token stop,
                                                        const std::function<void(const services::TrainProcessProgress&)>& progress) {
    if (config_.training_executable.empty()) throw contracts::UnavailableError("local training executable is unavailable");
    auto process = services::TrainProcessClient::launch(request, config_.training_executable);
    mmltk::common::concurrency::ScopedEventCancellation<services::TrainProcessStopSource> cancellation{stop};
    struct Observer final {
        const std::function<void(const services::TrainProcessProgress&)>* sink;
        static void Report(void* context, const services::TrainProcessProgress& value) noexcept {
            try {
                (*static_cast<Observer*>(context)->sink)(value);
            } catch (...) {}
        }
    } observer{&progress};
    const auto result = process.Run(cancellation.ConsumeToken(), {.context = &observer, .report = &Observer::Report});
    const auto outcome = result.terminal.outcome == services::TrainProcessExitOutcome::Succeeded   ? contracts::ComputeOperationOutcome::Succeeded
                         : result.terminal.outcome == services::TrainProcessExitOutcome::Cancelled ? contracts::ComputeOperationOutcome::Cancelled
                                                                                                   : contracts::ComputeOperationOutcome::Failed;
    return contracts::make_compute_terminal(outcome, 0, result.terminal.final_progress ? result.terminal.final_progress->progress.completed : 0, {},
                                            result.terminal.error);
}
contracts::ProviderQueryResult NativeTrainingRuntime::Query(const contracts::ProviderPreferences& preferences, const std::stop_token stop) {
    if (!config_.provider.valid()) throw contracts::UnavailableError("provider access is unavailable");
    mmltk::common::concurrency::ScopedEventCancellation<services::VastCancellationSource> cancellation{stop};
    const auto offers = services::VastClient{config_.provider}.query(preferences, cancellation.token());
    return services::materialize_provider_query_result(offers);
}
contracts::ProviderEffectResult NativeTrainingRuntime::Mutate(
    const contracts::ProviderMutation mutation, const contracts::ProviderPreferences& preferences, const contracts::ProviderOfferIdentity offer,
    const int instance,
    // CLEANUP-IGNORE: Only the provider guard and scoped cancellation agree; query, mutation, and reconciliation settle differently.
    const std::string_view launch_token, const std::stop_token stop) {
    if (!config_.provider.valid()) throw contracts::UnavailableError("provider access is unavailable");
    mmltk::common::concurrency::ScopedEventCancellation<services::VastCancellationSource> cancellation{stop};
    services::VastEffectAttempt attempt;
    try {
        services::VastClient client{config_.provider};
        if (mutation == contracts::ProviderMutation::Create) {
            const auto created = client.create(offer.offer_id, preferences, launch_token, attempt, cancellation.token());
            if (!created.success || created.instance_id <= 0) return contracts::provider_effect_not_applied("provider did not allocate the selected offer");
            return {.disposition = contracts::ProviderReconciliationDisposition::Applied, .instance_id = created.instance_id};
        }
        client.mutate(mutation, instance, attempt, cancellation.token());
        return reconcile_result(client.reconcile({.mutation = mutation, .instance_id = instance, .launch_token = {}}, cancellation.token()), instance);
    } catch (const services::VastBridgeError& error) {
        const bool cancelled = error.kind() == services::VastBridgeFailureKind::Cancelled;
        return attempt.started() ? contracts::provider_effect_inconclusive(error.what(), cancelled)
                                 : contracts::provider_effect_not_applied(error.what(), cancelled);
    } catch (const std::exception& error) {
        return attempt.started() ? contracts::provider_effect_inconclusive(error.what()) : contracts::provider_effect_not_applied(error.what());
    } catch (...) {
        return attempt.started() ? contracts::provider_effect_inconclusive("provider mutation failed")
                                 : contracts::provider_effect_not_applied("provider mutation failed");
    }
    // CLEANUP-IGNORE: Only the provider guard and scoped cancellation agree; query, mutation, and reconciliation settle differently.
}
contracts::ProviderEffectResult NativeTrainingRuntime::Reconcile(const services::VastReconciliationRequest& request, const std::stop_token stop) {
    if (!config_.provider.valid()) throw contracts::UnavailableError("provider access is unavailable");
    mmltk::common::concurrency::ScopedEventCancellation<services::VastCancellationSource> cancellation{stop};
    try {
        return reconcile_result(services::VastClient{config_.provider}.reconcile(request, cancellation.token()), request.instance_id);
    } catch (const services::VastBridgeError& error) {
        return contracts::provider_effect_inconclusive(error.what(), error.kind() == services::VastBridgeFailureKind::Cancelled);
    } catch (const std::exception& error) { return contracts::provider_effect_inconclusive(error.what()); } catch (...) {
        return contracts::provider_effect_inconclusive("provider reconciliation failed");
    }
}
class TrainingSystem::Impl final {
   private:
    friend class TrainingSystem;

   public:
    struct Pending final {
        contracts::ProviderMutation mutation = contracts::ProviderMutation::Create;
        contracts::ProviderOfferIdentity offer{};
        contracts::ProviderPreferences preferences{};
        int instance = 0;
        std::string launch_token;
    };
    Impl(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model, std::optional<mmltk::common::system::ExecutionPolicyRequest> policy,
         RuntimeFactory factory, SystemEventSink<event_type> events)
        : settings_(settings), dataset_(dataset), model_(model), factory_(std::move(factory)), events_(std::move(events)) {
        if (!factory_) throw contracts::UnavailableError("training runtime factory is unavailable");
        std::promise<void> initialized;
        auto ready = initialized.get_future();
        inspection_worker_ = std::jthread([this, policy = std::move(policy), initialized = std::move(initialized)]() mutable {
            std::optional<mmltk::common::system::ScopedExecutionPolicy> placement;
            try {
                if (policy) placement.emplace(*policy);
            } catch (...) {
                initialized.set_exception(std::current_exception());
                return;
            }
            initialized.set_value();
            InspectLoop();
        });
        // Constructor failure unwinds through jthread's join before any captured
        // state disappears. No archive work is admitted until policy is verified.
        ready.get();
    }
    ~Impl() { StopInspection(); }
    void StopInspection() noexcept {
        std::stop_source stop{std::nostopstate};
        {
            std::scoped_lock lock(mutex_);
            inspection_shutdown_ = true;
            inspection_pending_.reset();
            stop = inspection_stop_;
        }
        stop.request_stop();
        inspection_ready_.notify_one();
        if (inspection_worker_.joinable()) inspection_worker_.join();
    }
    mmltk::backend::models::rfdetr::TrainingCheckpointInspection Inspect(std::filesystem::path path) {
        using Status = mmltk::backend::models::rfdetr::TrainingInspectionStatus;
        std::stop_source previous{std::nostopstate};
        mmltk::backend::models::rfdetr::TrainingCheckpointInspection result;
        {
            std::scoped_lock lock(mutex_);
            if (inspection_shutdown_) throw contracts::UnavailableError("checkpoint inspection is shutting down");
            const auto generation = contracts::next_compute_generation(state_.inspection.generation);
            if (!generation) throw contracts::FailedError("checkpoint inspection generation exhausted");
            previous = inspection_stop_;
            inspection_stop_ = path.empty() ? std::stop_source{std::nostopstate} : std::stop_source{};
            state_.inspection = {.generation = *generation, .path = std::move(path), .status = Status::Running};
            inspection_file_.reset();
            if (state_.inspection.path.empty()) {
                state_.inspection.status = Status::Cancelled;
                inspection_pending_.reset();
            } else {
                inspection_pending_ = state_.inspection;
            }
            result = state_.inspection;
            AdvanceObservation();
        }
        previous.request_stop();
        inspection_ready_.notify_one();
        return result;
    }
    void InspectLoop() noexcept {
        using Status = mmltk::backend::models::rfdetr::TrainingInspectionStatus;
        std::unique_ptr<TrainingRuntime> inspector;
        for (;;) {
            mmltk::backend::models::rfdetr::TrainingCheckpointInspection current;
            std::stop_token stop;
            {
                std::unique_lock lock(mutex_);
                inspection_ready_.wait(lock, [&] { return inspection_shutdown_ || inspection_pending_.has_value(); });
                if (inspection_shutdown_) return;
                current = std::move(*inspection_pending_);
                inspection_pending_.reset();
                stop = inspection_stop_.get_token();
            }
            std::optional<mmltk::common::io::FileSnapshot> identity;
            try {
                if (!inspector) inspector = factory_();
                if (!inspector) throw std::runtime_error("checkpoint inspection runtime unavailable");
                identity = mmltk::common::io::FileSnapshot::Read(current.path);
                current.checkpoint = inspector->InspectCheckpoint(current.path, stop);
                identity->RequireUnchanged(current.path);
                current.status = Status::Ready;
            } catch (const std::exception& error) {
                current.status = Status::Failed;
                current.error = std::string_view{error.what()}.substr(0, 1024);
            } catch (...) {
                current.status = Status::Failed;
                current.error = "checkpoint inspection failed";
            }
            if (stop.stop_requested()) current.status = Status::Cancelled;
            if (current.status != Status::Ready) { current.checkpoint.reset(); identity.reset(); }
            {
                std::scoped_lock lock(mutex_);
                if (inspection_shutdown_ || state_.inspection.generation != current.generation) continue;
                state_.inspection = current;
                inspection_file_ = identity;
                AdvanceObservation();
            }
            direct::PublishLazyNoexcept(events_, [&] { return event_type{TrainingInspectionChanged{std::move(current)}}; });
        }
    }
    mmltk::backend::models::rfdetr::TrainingCheckpoint PreparedCheckpoint(const std::filesystem::path& path) const {
        std::scoped_lock lock(mutex_);
        const auto& inspection = state_.inspection;
        if (inspection.status != mmltk::backend::models::rfdetr::TrainingInspectionStatus::Ready || !inspection.checkpoint || !inspection_file_ ||
            (path != inspection.path && path != inspection.checkpoint->path))
            throw contracts::InvalidIntentError("inspect the selected checkpoint before preparing resume");
        inspection_file_->RequireUnchanged(inspection.path);
        return *inspection.checkpoint;
    }
    TrainingRuntime& runtime() {
        if (!runtime_) runtime_ = factory_();
        if (!runtime_) throw std::runtime_error("training runtime is unavailable");
        return *runtime_;
    }
    [[nodiscard]] TrainingSnapshot snapshot() const {
        std::scoped_lock lock(mutex_);
        return state_;
    }
    [[nodiscard]] direct::LocalRun::Notification notification(event_type event) {
        return [this, event = std::move(event)]() mutable noexcept {
            if (!events_) return;
            try {
                events_(std::move(event));
            } catch (...) {}
        };
    }
    services::TrainRunStore run_store_;
    std::mutex run_store_mutex_;
    [[nodiscard]] TrainingSnapshot Start(std::filesystem::path resume = {}) {
        auto facts = settings_.materialization_facts();
        if (!facts.loaded) throw contracts::UnavailableError("settings are unavailable");
        if (resume.empty())
            facts.settings.workflows.train.request.resume_path.clear();
        else if (facts.settings.workflows.train.request.resume_path != resume)
            throw contracts::InvalidIntentError("Resume settings no longer match the selected checkpoint");
        const auto selection = model_.selection();
        run_.Start({
            .prepare =
                [this] {
                    std::scoped_lock lock(mutex_);
                    const auto next = contracts::next_compute_generation(state_.local.generation_frontier);
                    if (!next) throw contracts::FailedError("training operation generation exhausted");
                    state_.activity = TrainingActivity::Local;
                    state_.output_directory.clear();
                    state_.local.active = true;
                    state_.local.generation_frontier = *next;
                    state_.local.progress = {};
                    state_.local.terminal = contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Running, state_.local.generation_frontier);
                    state_.local.terminal.detail = "Inspecting selected training inputs";
                    AdvanceObservation();
                },
            .work = [this, settings = facts.settings, selection](const std::stop_token stop) mutable -> direct::LocalRun::Notification {
                contracts::ComputeTerminal terminal;
                bool failed = false;
                std::atomic_bool malformed_progress = false;
                try {
                    const auto& selected = settings.workflows.train.request;
                    contracts::ArtifactInspection inspection;
                    if (!stop.stop_requested()) {
                        inspection = dataset_.Inspect({selected.train_compiled_path, selected.val_compiled_path, selected.test_compiled_path},
                                                      selection.key.preset, selection.key.resolution, stop);
                    }
                    if (stop.stop_requested()) {
                        terminal = contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
                    } else {
                        auto request = subsystems::system::ComputeIntentMaterializer::LocalTrain(settings, inspection, selection);
                        if (!request) throw contracts::InvalidIntentError(request.error().detail);
                        std::optional<mmltk::backend::models::rfdetr::TrainingCheckpoint> continuation;
                        if (!request->resume_path.empty()) {
                            continuation = mmltk::backend::models::rfdetr::inspect_training_checkpoint(request->resume_path, stop);
                            if (!continuation->resumable) throw contracts::InvalidIntentError("selected artifact is not resumable");
                        }
                        if (stop.stop_requested()) throw mmltk::backend::models::rfdetr::ArtifactPublicationCancelled{};
                        request->output_dir = services::TrainRunStore::ResolveOutput(request->output_dir, continuation, settings.workflows.train.auto_output);
                        {
                            std::scoped_lock lock(mutex_);
                            state_.output_directory = request->output_dir;
                            state_.metrics.reset();
                            state_.persistence = {};
                            AdvanceObservation();
                        }
                        direct::PublishLazyNoexcept(events_, [&] { return event_type{TrainingChanged{snapshot()}}; });
                        terminal = runtime().Train(std::move(*request), stop, [this, &malformed_progress](const services::TrainProcessProgress& update) {
                            const auto& progress = update.progress;
                            if (!progress.valid()) {
                                malformed_progress.store(true, std::memory_order_relaxed);
                                return;
                            }
                            TrainingProgress observation;
                            {
                                std::scoped_lock lock(mutex_);
                                if (!state_.local.active || !contracts::compute_progress_follows(progress, state_.local.progress.sequence)) return;
                                state_.local.progress = progress;
                                if (update.metrics) state_.metrics = update.metrics;
                                state_.persistence = update.persistence;
                                if (state_.local.terminal.outcome == contracts::ComputeOperationOutcome::Running) state_.local.terminal.detail.clear();
                                AdvanceObservation();
                                observation = {
                                    .revision = state_.revision,
                                    .activity = state_.activity,
                                    .local = state_.local,
                                    .metrics = state_.metrics,
                                    .persistence = state_.persistence,
                                };
                            }
                            direct::PublishLazyNoexcept(events_, [&] { return event_type{std::move(observation)}; });
                        });
                    }
                    if (malformed_progress.load(std::memory_order_relaxed) || !terminal.valid_worker_terminal())
                        throw std::runtime_error("local training runtime returned an invalid result");
                    failed = terminal.outcome == contracts::ComputeOperationOutcome::Failed;
                } catch (const mmltk::backend::models::rfdetr::ArtifactPublicationCancelled&) {
                    terminal = contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
                } catch (...) {
                    failed = true;
                    terminal = contracts::compute_failure_terminal(std::current_exception(), "local training failed");
                }
                if (failed) runtime_.reset();
                {
                    std::scoped_lock lock(mutex_);
                    if (stop.stop_requested() && !failed) terminal = contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
                    terminal.generation = state_.local.generation_frontier;
                    state_.local.active = false;
                    state_.local.terminal = terminal;
                    state_.activity = TrainingActivity::Idle;
                    AdvanceObservation();
                }
                auto settled = snapshot();
                return notification(event_type{TrainingChanged{std::move(settled)}});
            },
            .failure = [this](std::exception_ptr) { return FailLocal(); },
        });
        return snapshot();
    }
    [[nodiscard]] TrainingSnapshot Query() {
        const auto preferences = settings_.provider_preferences();
        if (!preferences.valid()) throw contracts::UnavailableError("provider preferences are unavailable");
        run_.Start({
            .prepare =
                [this] {
                    std::scoped_lock lock(mutex_);
                    const auto next = contracts::next_compute_generation(state_.offers.revision);
                    if (!next || !contracts::next_compute_generation(*next)) throw contracts::FailedError("provider query revision exhausted");
                    state_.activity = TrainingActivity::ProviderQuery;
                    state_.offers.revision = *next;
                    state_.offers.outcome = contracts::ProviderQueryOutcome::Idle;
                    state_.offers.cancellation_requested = false;
                    state_.offers.detail.clear();
                    AdvanceObservation();
                },
            .work = [this, preferences](const std::stop_token stop) -> direct::LocalRun::Notification {
                contracts::ProviderQueryResult result;
                bool failed = false;
                try {
                    result = contracts::normalize_provider_query_result(runtime().Query(preferences, stop));
                    failed = result.outcome == contracts::ProviderQueryOutcome::Failed || result.outcome == contracts::ProviderQueryOutcome::Rejected;
                } catch (const std::exception& error) {
                    failed = true;
                    result = {.outcome = contracts::ProviderQueryOutcome::Failed, .detail = contracts::bounded_provider_detail(error.what())};
                } catch (...) {
                    failed = true;
                    result = {.outcome = contracts::ProviderQueryOutcome::Failed, .detail = "provider query failed"};
                }
                if (failed) runtime_.reset();
                {
                    std::scoped_lock lock(mutex_);
                    AdvanceRevision(state_.offers.revision, "provider query revision exhausted");
                    if (stop.stop_requested()) {
                        result.outcome = contracts::ProviderQueryOutcome::Cancelled;
                        result.detail = "provider query cancelled";
                        result.offers.clear();
                        failed = false;
                    }
                    state_.offers.outcome = result.outcome;
                    state_.offers.cancellation_requested = false;
                    state_.offers.detail = contracts::bounded_provider_detail(result.detail);
                    state_.offers.offers =
                        result.outcome == contracts::ProviderQueryOutcome::Succeeded ? std::move(result.offers) : std::vector<contracts::ProviderOffer>{};
                    // CLEANUP-IGNORE: Query settlement clears selection; local-compute settlement owns terminal
                    // progress instead.
                    state_.offers.selected.reset();
                    state_.activity = TrainingActivity::Idle;
                    AdvanceObservation();
                }
                auto settled = snapshot();
                return notification(event_type{TrainingChanged{std::move(settled)}});
            },
            .failure = [this](std::exception_ptr) { return FailQuery(); },
        });
        return snapshot();
    }
    [[nodiscard]] TrainingSnapshot Mutate(Pending pending, const bool reconcile) {
        auto failure_pending = pending;
        run_.Start({
            .prepare =
                [this] {
                    std::scoped_lock lock(mutex_);
                    const auto next = contracts::next_compute_generation(state_.remote.revision);
                    if (!next || !contracts::next_compute_generation(*next)) throw contracts::FailedError("remote operation revision exhausted");
                    state_.activity = TrainingActivity::Remote;
                    state_.remote.revision = *next;
                    state_.remote.outcome = contracts::RemoteOperationOutcome::Idle;
                    state_.remote.detail.clear();
                    AdvanceObservation();
                },
            .work = [this, pending = std::move(pending), reconcile](const std::stop_token stop) mutable -> direct::LocalRun::Notification {
                contracts::ProviderEffectResult result;
                bool worker_failed = false;
                try {
                    result = reconcile ? runtime().Reconcile(
                                             {.mutation = pending.mutation,
                                              .instance_id = pending.mutation == contracts::ProviderMutation::Create ? 0 : pending.instance,
                                              .launch_token = pending.mutation == contracts::ProviderMutation::Create ? pending.launch_token : std::string{}},
                                             stop)
                                       : runtime().Mutate(pending.mutation, pending.preferences, pending.offer, pending.instance, pending.launch_token, stop);
                    if (!result.valid()) throw std::runtime_error("provider runtime returned an invalid effect");
                } catch (const std::exception& error) {
                    worker_failed = true;
                    result = contracts::provider_effect_not_applied(error.what());
                } catch (...) {
                    worker_failed = true;
                    result = contracts::provider_effect_not_applied("provider operation failed");
                }
                if (worker_failed) runtime_.reset();
                {
                    std::scoped_lock lock(mutex_);
                    SettleRemote(pending, result, reconcile);
                    AdvanceRevision(state_.remote.revision, "remote operation revision exhausted");
                    state_.activity = TrainingActivity::Idle;
                    AdvanceObservation();
                }
                auto settled = snapshot();
                return notification(event_type{TrainingChanged{std::move(settled)}});
            },
            .failure = [this, pending = std::move(failure_pending)](std::exception_ptr) mutable -> direct::LocalRun::Notification {
                return FailRemote(std::move(pending));
            },
        });
        return snapshot();
    }
    [[nodiscard]] direct::LocalRun::Notification FailLocal() {
        runtime_.reset();
        {
            std::scoped_lock lock(mutex_);
            state_.local.active = false;
            state_.activity = TrainingActivity::Idle;
            state_.local.terminal.outcome = contracts::ComputeOperationOutcome::Failed;
            state_.local.terminal.generation = state_.local.generation_frontier;
            state_.local.terminal.completed = 0;
            state_.local.terminal.output.clear();
            state_.local.terminal.detail = "local training worker failed";
            AdvanceObservation();
        }
        return notification(event_type{TrainingChanged{snapshot()}});
    }
    [[nodiscard]] direct::LocalRun::Notification FailQuery() {
        runtime_.reset();
        {
            std::scoped_lock lock(mutex_);
            state_.activity = TrainingActivity::Idle;
            AdvanceRevision(state_.offers.revision, "provider query revision exhausted");
            state_.offers.outcome = contracts::ProviderQueryOutcome::Failed;
            state_.offers.cancellation_requested = false;
            state_.offers.offers.clear();
            state_.offers.selected.reset();
            state_.offers.detail = "provider query worker failed";
            AdvanceObservation();
        }
        return notification(event_type{TrainingChanged{snapshot()}});
    }
    [[nodiscard]] direct::LocalRun::Notification FailRemote(Pending pending) {
        runtime_.reset();
        {
            std::scoped_lock lock(mutex_);
            pending_ = std::move(pending);
            state_.activity = TrainingActivity::Idle;
            AdvanceRevision(state_.remote.revision, "remote operation revision exhausted");
            state_.remote.phase = contracts::RemoteSessionPhase::Unknown;
            state_.remote.outcome = contracts::RemoteOperationOutcome::Inconclusive;
            state_.remote.reconciliation_pending = true;
            state_.remote.detail = "provider operation worker failed";
            AdvanceObservation();
        }
        return notification(event_type{TrainingChanged{snapshot()}});
    }
    void SettleRemote(const Pending& operation, const contracts::ProviderEffectResult& result, bool reconcile) {
        using Disposition = contracts::ProviderReconciliationDisposition;
        if (result.disposition == Disposition::Applied) {
            if (operation.mutation == contracts::ProviderMutation::Create && result.instance_id <= 0) {
                pending_ = operation;
                state_.remote.phase = contracts::RemoteSessionPhase::Unknown;
                state_.remote.outcome = contracts::RemoteOperationOutcome::Inconclusive;
                state_.remote.detail = "provider create applied without an instance identity";
            } else {
                pending_.reset();
                state_.remote.instance_id = operation.mutation == contracts::ProviderMutation::Create ? result.instance_id : operation.instance;
                state_.remote.phase =
                    operation.mutation == contracts::ProviderMutation::Stop ? contracts::RemoteSessionPhase::Stopped : contracts::RemoteSessionPhase::Running;
                state_.remote.outcome = contracts::RemoteOperationOutcome::Applied;
                state_.remote.detail.clear();
            }
        } else if (result.disposition == Disposition::Inconclusive) {
            pending_ = operation;
            state_.remote.phase = contracts::RemoteSessionPhase::Unknown;
            state_.remote.outcome = contracts::RemoteOperationOutcome::Inconclusive;
            state_.remote.detail = contracts::bounded_provider_detail(
                result.detail.empty() ? (reconcile ? "provider reconciliation is inconclusive" : "provider mutation reconciliation is inconclusive")
                                      : result.detail);
        } else {
            pending_.reset();
            state_.remote.outcome = result.cancelled ? contracts::RemoteOperationOutcome::Cancelled : contracts::RemoteOperationOutcome::Failed;
            state_.remote.detail = contracts::bounded_provider_detail(result.detail.empty() ? "provider mutation was not applied" : result.detail);
            if (operation.mutation == contracts::ProviderMutation::Create) {
                state_.remote.phase = contracts::RemoteSessionPhase::Absent;
                state_.remote.instance_id = 0;
            } else {
                state_.remote.phase =
                    operation.mutation == contracts::ProviderMutation::Start ? contracts::RemoteSessionPhase::Stopped : contracts::RemoteSessionPhase::Running;
            }
        }
        state_.remote.reconciliation_pending = pending_.has_value();
    }
    static void AdvanceRevision(std::uint64_t& revision, const char* detail) {
        const auto next = contracts::next_compute_generation(revision);
        if (!next) throw contracts::FailedError(detail);
        revision = *next;
    }
    void AdvanceObservation() { AdvanceRevision(state_.revision, "training observation revision exhausted"); }

   private:
    SettingsSystem& settings_;
    DatasetSystem& dataset_;
    ModelSystem& model_;
    RuntimeFactory factory_;
    SystemEventSink<event_type> events_;
    mutable std::mutex mutex_;
    TrainingSnapshot state_{};
    std::unique_ptr<TrainingRuntime> runtime_;
    std::optional<Pending> pending_;
    std::condition_variable inspection_ready_;
    std::optional<mmltk::backend::models::rfdetr::TrainingCheckpointInspection> inspection_pending_;
    std::optional<mmltk::common::io::FileSnapshot> inspection_file_;
    std::stop_source inspection_stop_;
    bool inspection_shutdown_ = false;
    std::jthread inspection_worker_;
    // CLEANUP-IGNORE: Training owns one LocalRun behind its sealed facade; compute systems have separate cores.
    direct::LocalRun run_;
};
TrainingSystem::TrainingSystem(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model,
                               std::optional<mmltk::common::system::ExecutionPolicyRequest> policy, RuntimeFactory factory,
                               SystemEventSink<event_type> events)
    : impl_(std::make_unique<Impl>(settings, dataset, model, std::move(policy), std::move(factory), std::move(events))) {}
TrainingSystem::~TrainingSystem() = default;
mmltk::backend::models::rfdetr::TrainingOpenedRun TrainingSystem::OpenRun(mmltk::backend::models::rfdetr::TrainingDirectoryQuery query) {
    std::lock_guard store_lock(impl_->run_store_mutex_);
    return impl_->run_store_.Open(query.directory);
}
mmltk::backend::models::rfdetr::TrainingHistoryPage TrainingSystem::History(mmltk::backend::models::rfdetr::TrainingHistoryQuery query) {
    std::lock_guard lock(impl_->run_store_mutex_);
    return impl_->run_store_.Read(query);
}
mmltk::backend::models::rfdetr::TrainingCheckpointInspection TrainingSystem::InspectCheckpoint(mmltk::backend::models::rfdetr::TrainingCheckpointQuery query) {
    if (query.path.empty()) throw contracts::InvalidIntentError("checkpoint path is empty");
    return impl_->Inspect(std::move(query.path));
}
mmltk::backend::models::rfdetr::TrainingCheckpointInspection TrainingSystem::CancelCheckpointInspection() { return impl_->Inspect({}); }
mmltk::backend::models::rfdetr::TrainingCheckpoint TrainingSystem::PrepareResume(mmltk::backend::models::rfdetr::TrainingCheckpointQuery query) {
    if (snapshot().activity != TrainingActivity::Idle) throw contracts::BusyError("training is active");
    auto checkpoint = impl_->PreparedCheckpoint(query.path);
    if (!checkpoint.resumable || !checkpoint.configuration) throw contracts::InvalidIntentError("selected artifact is not a full resumable checkpoint");
    impl_->settings_.RestoreTrainingCheckpoint(*checkpoint.configuration, checkpoint.path);
    return checkpoint;
}
TrainingSnapshot TrainingSystem::Resume(mmltk::backend::models::rfdetr::TrainingCheckpointQuery query) {
    auto checkpoint = impl_->PreparedCheckpoint(query.path);
    if (!checkpoint.resumable) throw contracts::InvalidIntentError("selected artifact is not resumable");
    return impl_->Start(checkpoint.path);
}
TrainingSnapshot TrainingSystem::Start(contracts::WorkflowIntent<contracts::FeatureId::Train>) { return impl_->Start(); }
TrainingSnapshot TrainingSystem::Stop(contracts::WorkflowIntent<contracts::FeatureId::Train>) noexcept {
    TrainingSnapshot result;
    std::stop_source stop{std::nostopstate};
    {
        std::scoped_lock lock(impl_->mutex_);
        if (impl_->state_.activity == TrainingActivity::Local &&
            impl_->state_.local.terminal.outcome != contracts::ComputeOperationOutcome::CancellationRequested) {
            stop = impl_->run_.CurrentStopSource();
            impl_->state_.local.terminal =
                contracts::make_compute_terminal(contracts::ComputeOperationOutcome::CancellationRequested, impl_->state_.local.generation_frontier);
            impl_->AdvanceObservation();
        }
        result = impl_->state_;
    }
    static_cast<void>(stop.request_stop());
    return result;
}
void TrainingSystem::Shutdown() noexcept {
    static_cast<void>(Stop({}));
    impl_->StopInspection();
    impl_->run_.StopAndJoin();
}
TrainingSnapshot TrainingSystem::Query(contracts::ProviderQueryIntent) { return impl_->Query(); }
TrainingSnapshot TrainingSystem::Select(const contracts::ProviderOfferIdentity identity) {
    if (impl_->run_.active()) throw contracts::BusyError("training operation is active");
    std::scoped_lock lock(impl_->mutex_);
    if (impl_->state_.activity != TrainingActivity::Idle) throw contracts::BusyError("training operation is active");
    if (!identity.valid() ||
        std::ranges::find(impl_->state_.offers.offers, identity.offer_id, &contracts::ProviderOffer::offer_id) == impl_->state_.offers.offers.end())
        throw contracts::InvalidIntentError("selected provider offer is stale or unknown");
    Impl::AdvanceRevision(impl_->state_.offers.revision, "provider selection revision exhausted");
    impl_->state_.offers.selected = identity;
    impl_->state_.offers.detail.clear();
    impl_->AdvanceObservation();
    return impl_->state_;
}
TrainingSnapshot TrainingSystem::Clear(contracts::ProviderClearIntent) {
    std::stop_source stop{std::nostopstate};
    TrainingSnapshot result;
    {
        std::scoped_lock lock(impl_->mutex_);
        if (impl_->state_.activity == TrainingActivity::ProviderQuery) {
            if (impl_->state_.offers.cancellation_requested) throw contracts::BusyError("provider query cancellation is already requested");
            Impl::AdvanceRevision(impl_->state_.offers.revision, "provider clear revision exhausted");
            stop = impl_->run_.CurrentStopSource();
            impl_->state_.offers.cancellation_requested = true;
            impl_->state_.offers.detail = "provider query cancellation requested";
            impl_->state_.offers.offers.clear();
            impl_->state_.offers.selected.reset();
            impl_->AdvanceObservation();
        } else if (impl_->state_.activity != TrainingActivity::Idle || impl_->run_.CurrentStopSource().stop_possible()) {
            throw contracts::BusyError("training operation is active");
        } else {
            Impl::AdvanceRevision(impl_->state_.offers.revision, "provider clear revision exhausted");
            const auto revision = impl_->state_.offers.revision;
            impl_->state_.offers = {};
            impl_->state_.offers.revision = revision;
            impl_->AdvanceObservation();
        }
        result = impl_->state_;
    }
    static_cast<void>(stop.request_stop());
    return result;
}
TrainingSnapshot TrainingSystem::StartRemote(contracts::ProviderStartIntent) {
    const auto current = impl_->snapshot();
    if (current.activity != TrainingActivity::Idle) throw contracts::BusyError("training operation is active");
    if (current.remote.phase == contracts::RemoteSessionPhase::Running) throw contracts::InvalidIntentError("remote training is already running");
    if (current.remote.reconciliation_pending) throw contracts::InvalidIntentError("remote training requires reconciliation");
    if (current.remote.phase == contracts::RemoteSessionPhase::Stopped) {
        if (current.remote.instance_id <= 0) throw contracts::InvalidIntentError("remote training session is unavailable");
        return impl_->Mutate({.mutation = contracts::ProviderMutation::Start, .instance = current.remote.instance_id, .launch_token = {}}, false);
    }
    if (!current.offers.selected) throw contracts::InvalidIntentError("no provider offer is selected");
    const auto preferences = impl_->settings_.provider_preferences();
    if (!preferences.valid()) throw contracts::UnavailableError("provider preferences are unavailable");
    return impl_->Mutate({.mutation = contracts::ProviderMutation::Create,
                          .offer = *current.offers.selected,
                          .preferences = preferences,
                          .launch_token = "mmltk-vast-" + std::to_string(current.offers.revision) + "-" + std::to_string(current.offers.selected->offer_id)},
                         false);
}
TrainingSnapshot TrainingSystem::StopRemote(contracts::ProviderStopIntent) {
    const auto snapshot = impl_->snapshot();
    if (snapshot.activity != TrainingActivity::Idle) throw contracts::BusyError("training operation is active");
    const auto& current = snapshot.remote;
    if (current.instance_id <= 0) throw contracts::InvalidIntentError("remote training session is unavailable");
    if (current.reconciliation_pending) throw contracts::InvalidIntentError("remote training requires reconciliation");
    if (current.phase != contracts::RemoteSessionPhase::Running) throw contracts::InvalidIntentError("remote training is not running");
    return impl_->Mutate({.mutation = contracts::ProviderMutation::Stop, .instance = current.instance_id, .launch_token = {}}, false);
}
TrainingSnapshot TrainingSystem::RetryReconciliation() {
    if (impl_->run_.active()) throw contracts::BusyError("training operation is active");
    std::optional<Impl::Pending> pending;
    {
        std::scoped_lock lock(impl_->mutex_);
        pending = impl_->pending_;
    }
    if (!pending) throw contracts::InvalidIntentError("no provider reconciliation is pending");
    return impl_->Mutate(std::move(*pending), true);
}
TrainingSnapshot TrainingSystem::snapshot() const { return impl_->snapshot(); }
}  // namespace mmltk::controller
