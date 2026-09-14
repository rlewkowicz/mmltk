#include "src/controller/subsystems/system/dataset_system.h"

#include <atomic>
#include <stdexcept>
#include <utility>

#include "src/controller/contracts/compute.h"

namespace mmltk::controller {

ArtifactDatasetRuntime::ArtifactDatasetRuntime() = default;
ArtifactDatasetRuntime::ArtifactDatasetRuntime(services::ArtifactStore store, services::ArtifactDiagnosticObserver diagnostics)
    : store_(std::move(store)), diagnostics_(diagnostics) {}
services::ArtifactCompileResult ArtifactDatasetRuntime::Compile(const services::ArtifactCompileRequest& request, const std::stop_token stop,
                                                                const std::function<void(const contracts::ArtifactProgress&)>& progress) {
    auto cancellation = services::ArtifactCancellationSource::Mint();
    std::stop_callback bridge(stop, [&source = cancellation.first] { static_cast<void>(source.RequestCancel()); });
    services::ArtifactProgressObserver observer{
        .context = const_cast<std::function<void(const contracts::ArtifactProgress&)>*>(&progress),
        .report = [](void* context, const contracts::ArtifactProgress& value) noexcept {
            try {
                (*static_cast<std::function<void(const contracts::ArtifactProgress&)>*>(context))(value);
            } catch (...) {}
        }};
    return store_.compile(request, cancellation.second, observer, diagnostics_);
}
contracts::ArtifactInspection ArtifactDatasetRuntime::Inspect(
    const std::array<std::filesystem::path, contracts::kArtifactSplitCapacity>& paths, const std::string_view preset,
    const std::uint32_t resolution, const std::stop_token stop) {
    auto cancellation = services::ArtifactCancellationSource::Mint();
    std::stop_callback bridge(stop, [&source = cancellation.first] { static_cast<void>(source.RequestCancel()); });
    return store_.inspect(paths, preset, resolution, cancellation.second);
}

DatasetSystem::DatasetSystem(SettingsSystem& settings, RuntimeFactory factory, SystemEventSink<event_type> events)
    : settings_(settings), factory_(std::move(factory)), events_(std::move(events)) {
    if (!factory_) throw contracts::UnavailableError("dataset runtime factory is unavailable");
}
DatasetSystem::~DatasetSystem() = default;

direct::LocalRun::Notification DatasetSystem::changed(std::optional<contracts::ArtifactUiState> settled) {
    if (!settled) return {};
    return [this, settled = std::move(*settled)]() mutable noexcept {
        direct::PublishLazyNoexcept(events_, [&] { return event_type{DatasetChanged{std::move(settled)}}; });
    };
}

contracts::ArtifactUiState DatasetSystem::Compile(contracts::WorkflowIntent<contracts::FeatureId::Train>) {
    const auto settings = settings_.materialization_facts();
    if (!settings.loaded) throw contracts::UnavailableError("settings are unavailable");
    auto request = services::materialize_artifact_compile(settings.settings);
    if (!request) throw contracts::InvalidIntentError(request.error().detail);
    run_.Start({
        .prepare =
            [this] {
                std::scoped_lock lock(mutex_);
                if (activity_ != DatasetActivity::None) throw contracts::BusyError("dataset operation is active");
                const auto next = contracts::next_compute_generation(state_.generation);
                if (!next) throw contracts::FailedError("dataset operation generation exhausted");
                activity_ = DatasetActivity::Compile;
                state_.generation = *next;
                state_.active = true;
                state_.progress = {};
                state_.terminal = {
                    .outcome = contracts::ArtifactTerminalOutcome::Idle,
                    .artifact = {},
                    .detail = {},
                };
            },
        .work = [this, request = std::move(*request)](const std::stop_token stop) -> direct::LocalRun::Notification {
            services::ArtifactCompileResult result;
            std::string detail;
            bool failed = false;
            std::atomic_bool malformed_progress = false;
            try {
                if (!runtime_) runtime_ = factory_();
                if (!runtime_) throw std::runtime_error("dataset runtime is unavailable");
                result = runtime_->Compile(request, stop, [this, &malformed_progress](const contracts::ArtifactProgress& value) {
                    if (!value.valid()) {
                        malformed_progress.store(true, std::memory_order_relaxed);
                        return;
                    }
                    progress(value);
                });
                failed = malformed_progress.load(std::memory_order_relaxed) ||
                         result.output.string().size() > contracts::kArtifactPathCapacity || !result.inspection.valid() ||
                         (!result.cancelled && (result.output.empty() || !result.inspection.available()));
                if (failed)
                    detail = contracts::bounded_artifact_detail(
                        malformed_progress.load(std::memory_order_relaxed) ? "dataset compiler returned invalid progress"
                        : !result.inspection.valid()                       ? "dataset compiler returned an invalid inspection"
                        : result.inspection.detail.empty()                 ? "dataset compiler returned no compatible artifact"
                                                                           : result.inspection.detail);
            } catch (const std::exception& error) {
                failed = true;
                detail = contracts::bounded_artifact_detail(error.what());
            } catch (...) {
                failed = true;
                detail = "dataset compile failed";
            }
            contracts::ArtifactTerminal terminal;
            if (failed) {
                runtime_.reset();
                terminal = {
                    .outcome = contracts::ArtifactTerminalOutcome::Failed,
                    .artifact = {},
                    .detail = detail,
                };
            } else if (result.cancelled || stop.stop_requested()) {
                terminal = {
                    .outcome = contracts::ArtifactTerminalOutcome::Cancelled,
                    .artifact = {},
                    .detail = {},
                };
            } else {
                terminal = {.outcome = contracts::ArtifactTerminalOutcome::Succeeded,
                            .artifact = result.output.string().substr(0, contracts::kArtifactPathCapacity),
                            .detail = {}};
            }
            std::optional<contracts::ArtifactUiState> settled;
            {
                std::scoped_lock lock(mutex_);
                if (activity_ == DatasetActivity::Compile) {
                    state_.active = false;
                    state_.inspection = failed ? contracts::ArtifactInspection{} : result.inspection;
                    state_.terminal = terminal;
                    activity_ = DatasetActivity::None;
                    settled = state_;
                }
            }
            return changed(std::move(settled));
        },
        .failure = [this](std::exception_ptr) -> direct::LocalRun::Notification {
            runtime_.reset();
            std::optional<contracts::ArtifactUiState> settled;
            {
                std::scoped_lock lock(mutex_);
                if (activity_ == DatasetActivity::Compile) {
                    state_.active = false;
                    state_.terminal.outcome = contracts::ArtifactTerminalOutcome::Failed;
                    state_.terminal.detail = "dataset worker failed";
                    state_.inspection.compatible = false;
                    state_.inspection.splits.clear();
                    state_.inspection.detail.clear();
                    activity_ = DatasetActivity::None;
                    settled = state_;
                }
            }
            return changed(std::move(settled));
        },
    });
    return snapshot();
}

contracts::ArtifactUiState DatasetSystem::Stop() noexcept {
    static_cast<void>(run_.Stop());
    std::scoped_lock lock(mutex_);
    if (state_.active)
        state_.terminal = {
            .outcome = contracts::ArtifactTerminalOutcome::CancellationRequested,
            .artifact = {},
            .detail = {},
        };
    return state_;
}
void DatasetSystem::Shutdown() noexcept {
    static_cast<void>(Stop());
    run_.StopAndJoin();
}
contracts::ArtifactInspection DatasetSystem::Inspect(std::array<std::filesystem::path, contracts::kArtifactSplitCapacity> paths,
                                                     std::string preset, const std::uint32_t resolution, const std::stop_token stop) {
    for (std::size_t index = 0; index < paths.size(); ++index) {
        if (paths[index].empty()) continue;
        paths[index] = paths[index].lexically_normal();
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (paths[index] == paths[previous]) {
                paths[index].clear();
                break;
            }
        }
    }
    {
        std::scoped_lock lock(mutex_);
        if (activity_ != DatasetActivity::None) throw contracts::BusyError("dataset operation is active");
        activity_ = DatasetActivity::Inspect;
    }
    const auto retire = [this] {
        std::scoped_lock lock(mutex_);
        if (activity_ == DatasetActivity::Inspect) activity_ = DatasetActivity::None;
    };
    contracts::ArtifactInspection result;
    try {
        if (stop.stop_requested()) {
            retire();
            return {};
        }
        if (!runtime_) runtime_ = factory_();
        if (!runtime_) throw contracts::UnavailableError("dataset runtime is unavailable");
        result = runtime_->Inspect(paths, preset, resolution, stop);
    } catch (const contracts::ApplicationError&) {
        runtime_.reset();
        retire();
        throw;
    } catch (const std::exception& error) {
        runtime_.reset();
        retire();
        throw contracts::FailedError(contracts::bounded_artifact_detail(error.what()));
    } catch (...) {
        runtime_.reset();
        retire();
        throw contracts::FailedError("dataset inspection failed");
    }
    if (stop.stop_requested()) {
        retire();
        return {};
    }
    if (!result.available()) {
        runtime_.reset();
        if (!result.valid()) {
            retire();
            throw contracts::FailedError("dataset runtime returned an invalid inspection");
        }
        retire();
        throw contracts::UnavailableError(result.detail.empty() ? "dataset artifact is unavailable" : result.detail);
    }
    {
        std::scoped_lock lock(mutex_);
        state_.inspection = result;
    }
    retire();
    return result;
}
contracts::ArtifactUiState DatasetSystem::snapshot() const {
    std::scoped_lock lock(mutex_);
    return state_;
}
void DatasetSystem::progress(const contracts::ArtifactProgress& value) noexcept {
    std::uint64_t generation = 0U;
    bool active = false;
    contracts::ArtifactTerminal terminal;
    {
        std::scoped_lock lock(mutex_);
        if (!state_.active) return;
        state_.progress = value;
        generation = state_.generation;
        active = state_.active;
        terminal = state_.terminal;
    }
    direct::PublishLazyNoexcept(events_, [&] { return event_type{DatasetProgress{generation, active, std::move(terminal), value}}; });
}

}  // namespace mmltk::controller
