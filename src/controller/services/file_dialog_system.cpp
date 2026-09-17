#include "src/controller/services/file_dialog_system.h"
#include <string_view>
#include "src/controller/contracts/compute.h"
namespace mmltk::controller {
namespace {
[[nodiscard]] std::string bounded_dialog_detail(const std::string_view detail) { return std::string{detail.substr(0, services::kFileDialogTextCapacity)}; }
}  // namespace
services::FileDialogSelection NativeFileDialogRuntime::Open(const services::ResolvedFileDialog& resolved, const std::stop_token stop) {
    if (!client_.valid()) throw contracts::UnavailableError("file dialog service is unavailable");
    services::FileDialogRequest request{resolved.descriptor.title, resolved.descriptor.mode, resolved.descriptor.filter};
    if (!request.valid()) throw contracts::InvalidIntentError("file dialog declaration is invalid");
    auto cancellation = services::FileDialogCancellationSource::Mint();
    std::stop_callback bridge(stop, [&source = cancellation.first] { static_cast<void>(source.RequestCancel()); });
    const auto result = client_.run(request, std::move(cancellation.second));
    services::FileDialogSelection selection{.target = resolved.target};
    if (result.disposition == services::FileDialogDisposition::Cancelled || result.disposition == services::FileDialogDisposition::Reaped) return selection;
    if (result.disposition != services::FileDialogDisposition::Selected || !result.path.valid()) throw contracts::FailedError("file dialog operation failed");
    selection.result = services::FileDialogSelected{std::string(result.path.bytes.data(), result.path.size)};
    if (resolved.descriptor.defer_apply()) return selection;
    auto update =
        services::resolve_file_dialog_path(services::file_dialog_stable_id(selection.target), std::get<services::FileDialogSelected>(selection.result).path);
    if (!update) throw contracts::FailedError("selected file path cannot be applied");
    contracts::SettingsUpdateRequest request_update;
    request_update.updates.emplace_back(std::move(*update));
    static_cast<void>(settings_.Update(std::move(request_update)));
    return selection;
}
FileDialogSystem::FileDialogSystem(RuntimeFactory factory, SystemEventSink<event_type> events) : factory_(std::move(factory)), events_(std::move(events)) {
    if (!factory_) throw contracts::UnavailableError("file dialog runtime factory is unavailable");
}
FileDialogSystem::~FileDialogSystem() = default;
FileDialogSnapshot FileDialogSystem::Open(const services::FileDialogOpen request) {
    const auto resolved = services::file_dialog_catalog().resolve(request);
    if (!resolved) throw contracts::InvalidIntentError("file dialog selector is invalid");
    run_.Start({
        .prepare =
            [this, request] {
                std::scoped_lock lock(mutex_);
                const auto next = contracts::next_compute_generation(state_.generation);
                if (!next) throw contracts::FailedError("file dialog operation generation exhausted");
                state_.generation = *next;
                state_.active = true;
                state_.cancellation_requested = false;
                state_.target = request.target;
                state_.selection.reset();
            },
        .work = [this, resolved = *resolved](const std::stop_token stop) -> direct::LocalRun::Notification {
            services::FileDialogSelection terminal{.target = resolved.target};
            std::string detail;
            bool failed = false;
            try {
                if (!runtime_) runtime_ = factory_();
                if (!runtime_) throw std::runtime_error("file dialog runtime is unavailable");
                terminal = runtime_->Open(resolved, stop);
                if (!terminal.valid_for(resolved.target)) throw std::runtime_error("file dialog runtime returned an invalid selection");
            } catch (const std::exception& error) {
                failed = true;
                detail = bounded_dialog_detail(error.what());
            } catch (...) {
                failed = true;
                detail = "file dialog operation failed";
            }
            if (failed) runtime_.reset();
            FileDialogSnapshot settled;
            {
                std::scoped_lock lock(mutex_);
                state_.active = false;
                state_.cancellation_requested = false;
                state_.target = resolved.target;
                state_.selection = failed ? std::nullopt : std::optional{terminal};
                settled = state_;
            }
            if (failed)
                return [this, settled = std::move(settled), detail = std::move(detail)]() mutable noexcept {
                    direct::PublishLazyNoexcept(events_, [&] { return event_type{FileDialogFailed{std::move(settled), std::move(detail)}}; });
                };
            return [this, settled = std::move(settled)]() mutable noexcept {
                direct::PublishLazyNoexcept(events_, [&] { return event_type{FileDialogCompleted{std::move(settled)}}; });
            };
        },
        .failure = [this](std::exception_ptr) -> direct::LocalRun::Notification {
            runtime_.reset();
            FileDialogSnapshot settled;
            {
                std::scoped_lock lock(mutex_);
                state_.active = false;
                state_.cancellation_requested = false;
                state_.selection.reset();
                settled = state_;
            }
            return [this, settled = std::move(settled)]() mutable noexcept {
                direct::PublishLazyNoexcept(events_, [&] { return event_type{FileDialogFailed{std::move(settled), "file dialog worker failed"}}; });
            };
        },
    });
    return snapshot();
}
FileDialogSnapshot FileDialogSystem::Stop() noexcept {
    bool accepted = false;
    {
        std::scoped_lock lock(mutex_);
        if (state_.active && !state_.cancellation_requested) {
            state_.cancellation_requested = true;
            accepted = true;
        }
    }
    if (accepted) static_cast<void>(run_.Stop());
    return snapshot();
}
void FileDialogSystem::Shutdown() noexcept {
    static_cast<void>(Stop());
    run_.StopAndJoin();
}
FileDialogSnapshot FileDialogSystem::snapshot() const {
    std::scoped_lock lock(mutex_);
    return state_;
}
}  // namespace mmltk::controller
