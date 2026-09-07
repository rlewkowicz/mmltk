#include "src/controller/presentation/presentation_system.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include "src/common/io/scoped_fd.h"
#include "src/controller/presentation/detail/monotonic_identity.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"

namespace mmltk::controller {

namespace gpu = mmltk::frameworks::gpu;

class PresentationSystem::Impl final {
   public:
    static constexpr std::size_t kMaximumSources = 8U;

    Impl(const VisualDeviceSettings settings, PresentationNativeWriterFactory factory, const std::span<const VisualSourceReader> sources,
         SystemEventSink<event_type> events, VisualDiagnosticSink diagnostics)
        : settings_(settings), events_(std::move(events)), diagnostics_(diagnostics), writer_(CreateWriter(settings, factory)) {
        if (!settings_.valid() || sources.empty() || sources.size() > kMaximumSources)
            throw contracts::InvalidIntentError("Presentation configuration is invalid");
        for (const auto& source : sources) {
            if (!source.source.valid() || !source.latest || !source.borrow ||
                std::ranges::count(sources, source.source, &VisualSourceReader::source) != 1)
                throw contracts::InvalidIntentError("Presentation source registry is invalid");
        }
        sources_ = sources;
        control_fd_.reset(::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK));
        if (control_fd_.get() < 0) throw std::runtime_error("Presentation native event wake is unavailable");
        worker_ = std::jthread([this](const std::stop_token stop) noexcept {
            try {
                Run(stop);
            } catch (...) { Failed(std::current_exception()); }
        });
    }

    PresentationSnapshot Select(const PresentationSourceIdentity source) {
        if (Find(source) == sources_.end()) throw contracts::InvalidIntentError("Presentation source is unknown");
        {
            std::scoped_lock lock(mutex_);
            if (terminal_failure_) throw contracts::FailedError(*terminal_failure_);
            if (stopping_) throw contracts::UnavailableError("Presentation admission is closed");
            selection_generation_ = presentation::detail::advance_monotonic_identity(selection_generation_);
            state_.selected = source;
            AdvanceRevision();
            pending_ = Pending{
                .source = source,
                .generation = selection_generation_,
            };
        }
        if (!Wake()) {
            Failed(std::make_exception_ptr(std::runtime_error("Presentation control notification failed")));
            throw contracts::FailedError("Presentation control notification failed");
        }
        return snapshot();
    }

    void Observe(const RendererObservation observation) {
        PresentationSnapshot completed;
        bool publish = false;
        bool redraw_enqueued = false;
        {
            std::scoped_lock lock(mutex_);
            const auto completed_sample = std::max(state_.browser_completed_sample, observation.completed_sample);
            if (state_.browser_completed_sample != completed_sample) {
                state_.browser_completed_sample = completed_sample;
                AdvanceRevision();
                completed = state_;
                publish = true;
            }
            if (observation.redraw_requested && state_.selected.valid() && !stopping_ && !pending_) {
                pending_ = Pending{
                    .source = state_.selected,
                    .generation = selection_generation_,
                };
                redraw_enqueued = true;
            }
        }
        if (publish) Publish(event_type{PresentationCompleted{completed}});
        if (redraw_enqueued && !Wake()) Failed(std::make_exception_ptr(std::runtime_error("Presentation control notification failed")));
    }

    void SetExpectedBrowserProcessGroup(const pid_t process_group) {
        {
            std::scoped_lock lock(mutex_);
            expected_process_group_ = process_group;
        }
        if (!Wake()) {
            Failed(std::make_exception_ptr(std::runtime_error("Presentation control notification failed")));
            throw contracts::FailedError("Presentation control notification failed");
        }
    }

    void BrowserPeerLost() noexcept {
        std::scoped_lock lock(mutex_);
        browser_terminal_ = true;
    }

    void CloseAdmission() noexcept {
        {
            std::scoped_lock lock(mutex_);
            stopping_ = true;
            pending_.reset();
        }
        static_cast<void>(Wake());
    }

    PresentationSnapshot snapshot() const {
        std::scoped_lock lock(mutex_);
        return state_;
    }

    PresentationShutdownResult Shutdown() noexcept {
        {
            std::scoped_lock lock(mutex_);
            if (!browser_terminal_) return PresentationShutdownResult::BrowserTerminalRequired;
            if (shutdown_started_) return PresentationShutdownResult::Stopped;
            shutdown_started_ = true;
            stopping_ = true;
            pending_.reset();
        }
        worker_.request_stop();
        static_cast<void>(Wake());
        if (worker_.joinable()) worker_.join();
        RetireWriter();
        {
            std::scoped_lock lock(mutex_);
            stopped_ = true;
        }
        return PresentationShutdownResult::Stopped;
    }
    PresentationShutdownResult Stop() noexcept { return Shutdown(); }
    bool stopped() const noexcept {
        std::scoped_lock lock(mutex_);
        return stopped_;
    }

   private:
    struct Pending final {
        PresentationSourceIdentity source{};
        std::uint64_t generation = 0U;
    };
    struct InFlight final {
        VisualFrame frame{};
        std::uint64_t selection_generation = 0U;
    };

    void AdvanceRevision() { state_.revision = presentation::detail::advance_monotonic_identity(state_.revision); }

    std::span<const VisualSourceReader>::iterator Find(const PresentationSourceIdentity source) {
        return std::ranges::find(sources_, source, &VisualSourceReader::source);
    }

    [[nodiscard]] static std::unique_ptr<PresentationNativeWriter> CreateWriter(const VisualDeviceSettings settings,
                                                                                const PresentationNativeWriterFactory& factory) {
        if (!settings.valid() || !factory) throw contracts::InvalidIntentError("Presentation configuration is invalid");
        auto writer = factory();
        if (!writer) throw std::runtime_error("Presentation native writer is unavailable");
        return writer;
    }

    void RunCycle(const std::stop_token stop) {
        std::optional<Pending> pending;
        const VisualSourceReader* reader = nullptr;
        std::optional<pid_t> expected_process_group;
        bool wake_again = false;
        {
            std::scoped_lock lock(mutex_);
            if (stopping_) return;
            expected_process_group = std::exchange(expected_process_group_, std::nullopt);
            if (!in_flight_ && pending_) {
                pending = *pending_;
                pending_.reset();
                const auto found = Find(pending->source);
                if (found != sources_.end()) {
                    reader = std::addressof(*found);
                    in_flight_ = InFlight{
                        .frame =
                            {
                                .source = pending->source,
                            },
                        .selection_generation = pending->generation,
                    };
                }
            }
        }
        if (expected_process_group) writer_->SetExpectedBrowserProcessGroup(*expected_process_group);
        if (reader) {
            const VisualFrame frame = reader->latest();
            if (frame.valid() && (frame.extent.width > settings_.maximum_width || frame.extent.height > settings_.maximum_height))
                throw contracts::InvalidIntentError("presentation source exceeds backbuffer bounds");
            bool submit = false;
            {
                std::scoped_lock lock(mutex_);
                const bool reserved =
                    in_flight_ && in_flight_->selection_generation == pending->generation && in_flight_->frame.source == pending->source;
                if (reserved && !stopping_ && !stop.stop_requested() && frame.valid() && frame.source == pending->source &&
                    pending->generation == selection_generation_ && state_.selected == pending->source) {
                    in_flight_->frame = frame;
                    submit = true;
                } else if (reserved) {
                    in_flight_.reset();
                    wake_again = pending_.has_value();
                }
            }
            if (submit) writer_->Submit(frame, pending->generation, *reader);
        }
        std::uint64_t pump_generation = 0U;
        std::uint64_t pump_frame_revision = 0U;
        {
            std::scoped_lock lock(mutex_);
            if (stopping_ || stop.stop_requested()) return;
            pump_generation = selection_generation_;
            if (in_flight_) pump_frame_revision = in_flight_->frame.revision;
        }
        if (pump_frame_revision != 0U)
            diagnostics_({
                .system = VisualSystemKind::Presentation,
                .operation = VisualDiagnosticOperation::PresentationPumpStarted,
                .device = settings_.device,
                .generation = pump_generation,
                .value = pump_frame_revision,
            });
        const PresentationNativeOutcome outcome = writer_->Pump(pump_generation);
        if (pump_frame_revision != 0U)
            diagnostics_({
                .system = VisualSystemKind::Presentation,
                .operation = VisualDiagnosticOperation::PresentationPumpCompleted,
                .device = settings_.device,
                .generation = pump_generation,
                .value = pump_frame_revision,
                .detail = static_cast<std::uint64_t>(outcome.progress),
            });
        PresentationSnapshot completed;
        PresentationSnapshot capability_snapshot;
        bool publish_capability = false;
        bool publish = false;
        {
            std::scoped_lock lock(mutex_);
            if (stopping_) return;
            const auto capability =
                outcome.progress == PresentationNativeProgress::Published ? outcome.publication.capability : outcome.capability;
            if (state_.capability != capability) {
                state_.capability = capability;
                AdvanceRevision();
                capability_snapshot = state_;
                publish_capability = true;
            }
            switch (outcome.progress) {
                case PresentationNativeProgress::Waiting:
                    break;
                case PresentationNativeProgress::Superseded:
                    if (in_flight_ && in_flight_->selection_generation == outcome.selection_generation) {
                        if (in_flight_->selection_generation == selection_generation_ && state_.selected == in_flight_->frame.source)
                            pending_ = Pending{
                                .source = state_.selected,
                                .generation = selection_generation_,
                            };
                        in_flight_.reset();
                    }
                    wake_again = pending_.has_value();
                    break;
                case PresentationNativeProgress::Published:
                    break;
            }
            if (outcome.progress != PresentationNativeProgress::Published) {
                completed = state_;
            } else {
                if (!in_flight_ || outcome.selection_generation != in_flight_->selection_generation || !outcome.publication.valid())
                    throw std::runtime_error("Presentation native publication failed");
                const auto frame = in_flight_->frame;
                const bool current = in_flight_->selection_generation == selection_generation_ && state_.selected == frame.source;
                in_flight_.reset();
                if (current) {
                    completed = state_;
                    completed.completed = frame;
                    completed.timeline_ready = outcome.publication.timeline_ready;
                    completed.presentation_revision = outcome.publication.presentation_revision;
                    completed.revision = presentation::detail::advance_monotonic_identity(completed.revision);
                    state_ = completed;
                    publish = true;
                }
                wake_again = pending_.has_value();
            }
        }
        if (wake_again && !Wake()) Failed(std::make_exception_ptr(std::runtime_error("Presentation control notification failed")));
        if (publish_capability) {
            diagnostics_({
                .system = VisualSystemKind::Presentation,
                .operation = VisualDiagnosticOperation::PresentationCapabilityPublished,
                .device = settings_.device,
                .generation = capability_snapshot.capability.generation,
                .value = capability_snapshot.revision,
                .detail = static_cast<std::uint64_t>(capability_snapshot.capability.condition),
                .context = {.capacity_width = capability_snapshot.capability.extent.width,
                            .capacity_height = capability_snapshot.capability.extent.height,
                            .surface_high = capability_snapshot.capability.surface_high,
                            .surface_low = capability_snapshot.capability.surface_low,
                            .selection_generation = pump_generation,
                            .frame_revision = pump_frame_revision,
                            .condition = static_cast<std::uint64_t>(capability_snapshot.capability.condition),
                            .outcome = 1U},
            });
            Publish(event_type{PresentationCapabilityChanged{capability_snapshot}});
        }
        if (!publish) return;
        diagnostics_({
            .system = VisualSystemKind::Presentation,
            .operation = VisualDiagnosticOperation::TimelineReady,
            .device = settings_.device,
            .generation = outcome.publication.capability.generation,
            .value = completed.timeline_ready,
            .context = {.capacity_width = outcome.publication.capability.extent.width,
                        .capacity_height = outcome.publication.capability.extent.height,
                        .surface_high = outcome.publication.capability.surface_high,
                        .surface_low = outcome.publication.capability.surface_low,
                        .selection_generation = pump_generation,
                        .frame_revision = completed.completed.revision,
                        .condition = static_cast<std::uint64_t>(outcome.publication.capability.condition),
                        .outcome = 1U},
        });
        Publish(event_type{PresentationCompleted{completed}});
    }

    void Run(const std::stop_token stop) {
        while (!stop.stop_requested()) {
            const int native_fd = writer_->poll_fd();
            short native_events = POLLIN;
            if (writer_->wants_write()) native_events |= POLLOUT;
            // CLEANUP-IGNORE: Presentation waits on its writer and control event; Firefox owns a separate process loop.
            std::array<pollfd, 2U> descriptors{{
                // CLEANUP-IGNORE: The writer descriptor is a Presentation-owned event source.
                {.fd = native_fd, .events = native_events, .revents = 0},
                {.fd = control_fd_.get(), .events = POLLIN, .revents = 0},
            }};
            const int result = ::poll(descriptors.data(), descriptors.size(), -1);
            if (result < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("Presentation native event wait failed");
            }
            if ((descriptors[1].revents & POLLIN) != 0) {
                std::uint64_t wake = 0U;
                ssize_t consumed = -1;
                do {
                    consumed = ::read(control_fd_.get(), &wake, sizeof(wake));
                } while (consumed < 0 && errno == EINTR);
                if (consumed < 0 && errno != EAGAIN) throw std::runtime_error("Presentation control wake failed");
                if (stop.stop_requested()) return;
            }
            if ((descriptors[0].revents & POLLNVAL) != 0) throw std::runtime_error("Presentation native descriptor is invalid");
            RunCycle(stop);
        }
    }

    [[nodiscard]] bool Wake() noexcept {
        const std::uint64_t wake = 1U;
        ssize_t written = -1;
        do {
            written = ::write(control_fd_.get(), &wake, sizeof(wake));
        } while (written < 0 && errno == EINTR);
        return written == static_cast<ssize_t>(sizeof(wake)) || (written < 0 && errno == EAGAIN);
    }

    void Failed(const std::exception_ptr failure) noexcept {
        auto detail = visual_failure_detail(failure, "Presentation native writer failed");
        bool publish = false;
        PresentationSnapshot failed;
        std::uint64_t failed_selection_generation = 0U;
        {
            std::scoped_lock lock(mutex_);
            pending_.reset();
            in_flight_.reset();
            stopping_ = true;
            if (!terminal_failure_) terminal_failure_ = detail;
            publish = !failure_published_;
            if (publish) {
                AdvanceRevision();
                failed = state_;
                failure_published_ = true;
            }
            failed_selection_generation = selection_generation_;
        }
        if (!publish) return;
        report_visual_worker_failure(diagnostics_, VisualSystemKind::Presentation, settings_.device, detail, failed_selection_generation);
        Publish(event_type{PresentationFailed{std::move(failed), std::move(detail)}});
    }

    void RetireWriter() noexcept {
        std::shared_ptr<PresentationNativeWriter> retired;
        {
            std::scoped_lock lock(mutex_);
            retired = std::move(writer_);
            pending_.reset();
            in_flight_.reset();
        }
        const auto result = retired ? retired->BrowserPeerLost() : PresentationNativeWriter::Retirement{};
        if (result.safe_to_destroy) {
            retired.reset();
        } else {
            // Capacity and shared custody are reserved before the worker starts.
            // A terminal receiver retains its borrowed source and CUDA resources.
            std::move(retirement_).Install(gpu::TerminalCudaCustody::Share(std::move(retired)), cudaErrorUnknown);
        }
        if (!result.all_released) Failed({});
    }

    void Publish(event_type event) noexcept { publish_visual_event_noexcept(events_, std::move(event)); }

    VisualDeviceSettings settings_;
    std::span<const VisualSourceReader> sources_;
    SystemEventSink<event_type> events_;
    VisualDiagnosticSink diagnostics_{};
    mutable std::mutex mutex_;
    PresentationSnapshot state_;
    std::optional<Pending> pending_;
    std::optional<InFlight> in_flight_;
    std::optional<pid_t> expected_process_group_;
    std::uint64_t selection_generation_ = 0U;
    bool stopping_ = false;
    bool browser_terminal_ = false;
    bool shutdown_started_ = false;
    bool stopped_ = false;
    bool failure_published_ = false;
    std::optional<std::string> terminal_failure_;
    gpu::TerminalCudaRetirementOwner terminal_{1U};
    gpu::TerminalCudaRetirementLease retirement_{gpu::ReserveTerminalCudaLease(terminal_)};
    std::shared_ptr<PresentationNativeWriter> writer_;
    mmltk::common::io::ScopedFd control_fd_;
    std::jthread worker_;
};

PresentationSystem::PresentationSystem(const VisualDeviceSettings settings, PresentationNativeWriterFactory factory,
                                       const std::span<const VisualSourceReader> sources, SystemEventSink<event_type> events,
                                       VisualDiagnosticSink diagnostics)
    : impl_(std::make_unique<Impl>(settings, std::move(factory), sources, std::move(events), diagnostics)) {}
PresentationSystem::~PresentationSystem() {
    if (!impl_->stopped()) std::terminate();
}
PresentationSnapshot PresentationSystem::Select(const PresentationSourceIdentity source) { return impl_->Select(source); }
void PresentationSystem::Observe(const RendererObservation observation) { impl_->Observe(observation); }
void PresentationSystem::SetExpectedBrowserProcessGroup(const pid_t process_group) { impl_->SetExpectedBrowserProcessGroup(process_group); }
void PresentationSystem::BrowserPeerLost() noexcept { impl_->BrowserPeerLost(); }
void PresentationSystem::CloseAdmission() noexcept { impl_->CloseAdmission(); }
PresentationShutdownResult PresentationSystem::Stop() noexcept { return impl_->Stop(); }
PresentationShutdownResult PresentationSystem::Shutdown() noexcept { return impl_->Shutdown(); }
bool PresentationSystem::stopped() const noexcept { return impl_->stopped(); }
PresentationSnapshot PresentationSystem::snapshot() const { return impl_->snapshot(); }

}  // namespace mmltk::controller
