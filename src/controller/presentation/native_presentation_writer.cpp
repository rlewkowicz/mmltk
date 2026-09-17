#include "src/controller/presentation/presentation_system.h"
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#include <cuda_runtime_api.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include "src/common/io/event_fd.h"
#include "src/common/io/scoped_fd.h"
#include "src/common/types/generation.h"
#include <charconv>
#include "src/controller/presentation/abi/workspace_frame_signal.h"
#include "src/controller/presentation/detail/native_presentation_writer_test_access.h"
#include "src/controller/presentation/detail/workspace_surface_import_channel.h"
#include "src/frameworks/gpu/external_graphics_timeline.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
import mmltk.backend.imaging.raster;
namespace mmltk::controller {
void PresentationAcceptanceGate::SetWake(std::function<void()> wake) {
    std::scoped_lock lock(mutex_);
    wake_ = std::move(wake);
}
void PresentationAcceptanceGate::SetObserver(std::function<void(Receipt)> observer) {
    std::scoped_lock lock(mutex_);
    observer_ = std::move(observer);
}
bool PresentationAcceptanceGate::Arm() {
    std::scoped_lock lock(mutex_);
    if (stopped_ || armed_ || held_ || !observer_) return false;
    armed_ = true;
    capacity_seen_ = false;
    return true;
}
bool PresentationAcceptanceGate::Release() {
    std::function<void()> wake;
    {
        std::scoped_lock lock(mutex_);
        if (stopped_ || !held_) return false;
        held_ = false;
        wake = wake_;
    }
    if (wake) wake();
    return true;
}
bool PresentationAcceptanceGate::Hold(const Receipt receipt) {
    std::function<void(Receipt)> observe;
    bool capacity = false;
    {
        std::scoped_lock lock(mutex_);
        if (stopped_) return false;
        if (held_) return true;
        if (!armed_) return false;
        armed_ = false;
        held_ = true;
        receipt_ = receipt;
        observe = observer_;
        capacity = capacity_seen_;
    }
    if (observe) observe(receipt);
    if (capacity) ObserveCapacity();
    return true;
}
void PresentationAcceptanceGate::ObserveCapacity() {
    std::function<void(Receipt)> observe;
    Receipt receipt;
    {
        std::scoped_lock lock(mutex_);
        if (stopped_ || (!armed_ && !held_)) return;
        capacity_seen_ = true;
        if (!held_ || receipt_.boundary == Boundary::Capacity) return;
        receipt_.boundary = Boundary::Capacity;
        receipt = receipt_;
        observe = observer_;
    }
    if (observe) observe(receipt);
}
void PresentationAcceptanceGate::HoldSupersession(const std::uint64_t high, const std::uint64_t low) {
    std::function<void(Receipt)> observe;
    {
        std::scoped_lock lock(mutex_);
        if (stopped_ || supersession_held_) return;
        supersession_held_ = true;
        observe = observer_;
    }
    if (observe) observe({.source_high = high, .source_low = low, .boundary = Boundary::Supersession});
}
bool PresentationAcceptanceGate::SupersessionHeld() {
    std::scoped_lock lock(mutex_);
    return supersession_held_;
}
bool PresentationAcceptanceGate::ReleaseSupersession() {
    std::function<void()> wake;
    {
        std::scoped_lock lock(mutex_);
        if (stopped_ || !supersession_held_) return false;
        supersession_held_ = false;
        wake = wake_;
    }
    if (wake) wake();
    return true;
}
void PresentationAcceptanceGate::Stop() noexcept {
    std::function<void()> wake;
    {
        std::scoped_lock lock(mutex_);
        stopped_ = true;
        armed_ = false;
        held_ = false;
        supersession_held_ = false;
        wake = std::move(wake_);
        observer_ = {};
    }
    if (wake) wake();
}
namespace {
namespace gpu = mmltk::frameworks::gpu;
namespace native = mmltk::controller::presentation;
namespace abi = native::workspace_surface_import;
using mmltk::common::io::ScopedFd;
struct SampleArena final {
    native::WorkspaceSurfaceImportId id{};
    std::uint64_t generation = 0U;
    VisualExtent extent{};
    PresentationSubmittedSource submitted{};
    abi::Record layout{};
    bool ready = false;
    bool binding_retired = false;
};
struct AdmittedSource;
struct Pending final {
    PresentationSubmittedSource submitted{};
    const VisualSourceReader* reader = nullptr;
    contracts::DiagnosticLink link{};
    std::uint64_t requested_owner = 0U;
    AdmittedSource* destination = nullptr;
    std::vector<std::byte> metadata{};
};
struct Transfer final {
    AdmittedSource* source;
    Pending pending;
    PresentationPublication publication;
    gpu::ImagePlaneView plane;
    std::uint64_t physical_revision = 0U;
};
// Callback storage is stable until stream settlement, including callback
// return. Notifications alone never discharge that destruction obligation.
struct Completion final {
    int fd = -1;
    std::atomic_bool ready{false};
    cudaError_t status = cudaSuccess;
};
constexpr std::size_t kPixelSampleCount = 25U;
struct PixelDeviceDeleter final {
    void operator()(std::uint32_t* value) const noexcept {
        if (value) static_cast<void>(cudaFree(value));
    }
};
struct AdmittedSource final {
    explicit AdmittedSource(const abi::Record& imported, gpu::DeviceContext shared_context, int wake, VisualDiagnosticSink diagnostic_sink)
        : description(imported), context(std::move(shared_context)), stream(context), diagnostics(diagnostic_sink) {
        completion.fd = wake;
    }
    void AdoptWorkspace(std::shared_ptr<gpu::ImageWorkspace> allocation, std::weak_ptr<ScopedFd> wake) {
        workspace = std::move(allocation);
        workspace_retirement = workspace->ObserveRetirement();
        workspace_release_wake = std::make_shared<const std::function<void()>>([wake] {
            if (const auto descriptor = wake.lock()) static_cast<void>(mmltk::common::io::signal_event_fd(descriptor->get()));
        });
        workspace->SetDisplayAvailabilitySink(workspace_release_wake);
    }
    ~AdmittedSource() noexcept {
        // The callback's wake can precede its return. Keep its source-owned
        // storage alive until this native completion owner settles the stream.
        gpu::ImageStreamSettlement settled;
        Cleanup(VisualDiagnosticOperation::PresentationSourceStreamSettlementStarted, VisualDiagnosticOperation::PresentationSourceStreamSettlementCompleted,
                [&] {
                    settled = stream.Settle();
                    if (!settled.completion_reached) std::terminate();
                });
        if (workspace || workspace_release_span) {
            // Admission rollback has no queued request. Explicit retirement
            // retains pending sources in the writer until this step can finish.
            try {
                if (!ReleaseWorkspace(settled)) std::terminate();
                // Display cleanup may have bound another device's context.
                context.Bind();
            } catch (...) { std::terminate(); }
        }
        Cleanup(VisualDiagnosticOperation::PresentationSourceProbeDeviceReleaseStarted,
                VisualDiagnosticOperation::PresentationSourceProbeDeviceReleaseCompleted, [&] { pixel_device.reset(); });
        Cleanup(VisualDiagnosticOperation::PresentationSourceProbeHostReleaseStarted, VisualDiagnosticOperation::PresentationSourceProbeHostReleaseCompleted,
                [&] { pixels.reset(); });
        release_span.reset();
        transfer.reset();
        Cleanup(VisualDiagnosticOperation::PresentationSourceStreamDestructionStarted, VisualDiagnosticOperation::PresentationSourceStreamDestructionCompleted,
                [&] { auto owned = std::move(stream); });
        Cleanup(VisualDiagnosticOperation::PresentationSourceSignalReleaseStarted, VisualDiagnosticOperation::PresentationSourceSignalReleaseCompleted, [&] {
            signal = {};
            edge.reset();
        });
        Cleanup(VisualDiagnosticOperation::PresentationSourceTimelineDestructionStarted,
                VisualDiagnosticOperation::PresentationSourceTimelineDestructionCompleted, [&] { timeline.reset(); });
    }
    [[nodiscard]] std::optional<std::exception_ptr> ReleaseWorkspace(std::optional<gpu::ImageStreamSettlement> completed = {}) {
        if (!workspace_release_started) {
            const auto settled = completed ? *completed : stream.Settle();
            if (!settled.completion_reached) {
                if (settled.failure) std::rethrow_exception(settled.failure);
                throw std::runtime_error("native source stream retirement is unsettled");
            }
            retirement_failure = gpu::combine_image_failures(retirement_failure, settled.failure);
            if (timeline && timeline->Release() != cudaSuccess)
                retirement_failure =
                    gpu::combine_image_failures(retirement_failure, std::make_exception_ptr(std::runtime_error("native source timeline retirement failed")));
            workspace_release_started = true;
            workspace_retirement.SetWake(workspace_release_wake);
            workspace_release_span.emplace(diagnostics, [&] {
                return visual_diagnostic_boundary(VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Presentation,
                                                                       .operation = VisualDiagnosticOperation::PresentationSourceWorkspaceReleaseStarted,
                                                                       .device = context.device(),
                                                                       .generation = generation,
                                                                       .context = {.surface_high = description.id_high,
                                                                                   .surface_low = description.id_low,
                                                                                   .workspace = native::workspace_source_diagnostic(description)}},
                                                  VisualDiagnosticOperation::PresentationSourceWorkspaceReleaseCompleted);
            });
            workspace.reset();
        }
        const auto result = workspace_retirement.TakeResult();
        if (!result.complete && !workspace_retirement.TransferToProducer()) return std::nullopt;
        // Outcome 2 is a custody handoff, not successful physical release.
        const auto failure = gpu::combine_image_failures(retirement_failure, result.claimed ? result.settlement.failure : std::exception_ptr{});
        const auto outcome = failure ? 0U : !result.complete ? 2U : 1U;
        if (workspace_release_span) workspace_release_span->FinishWith([&](auto& fact) { fact.context.outcome = outcome; });
        workspace_release_span.reset();
        workspace_retirement.SetWake({});
        return failure;
    }
    [[nodiscard]] native::WorkspaceSurfaceImportId id() const noexcept { return {description.id_high, description.id_low}; }
    [[nodiscard]] native::WorkspaceSurfaceImportId arena() const noexcept { return {description.arena_high, description.arena_low}; }
    const abi::Record description;
    // Retain the writer's long-lived context through this source's stream and
    // CUDA resource destruction. Physical source turnover creates no context.
    gpu::DeviceContext context;
    std::shared_ptr<gpu::ImageWorkspace> workspace;
    gpu::ImageWorkspace::Retirement workspace_retirement;
    std::shared_ptr<const std::function<void()>> workspace_release_wake;
    bool workspace_release_started = false;
    bool retirement_requested = false;
    std::exception_ptr retirement_failure;
    std::optional<services::RuntimeDiagnosticSpan<VisualDiagnosticSink, VisualDiagnosticFact>> workspace_release_span;
    std::uint64_t generation = 0U;
    std::optional<gpu::ExternalGraphicsTimeline> timeline;
    ScopedFd edge;
    native::WorkspaceSurfaceFrameSignal signal;
    std::uint64_t next_transfer = 1U;
    bool withdrawing = false;
    std::shared_ptr<std::atomic_bool> preparing = std::make_shared<std::atomic_bool>(false);
    gpu::ImageStream stream;
    Completion completion;
    std::optional<Transfer> transfer;
    const VisualSourceReader* producer = nullptr;
    bool detaching = false;
    bool exposed = false;
    bool acquired = false;
    bool release_submitted = false;
    bool release_complete = false;
    std::optional<services::RuntimeDiagnosticSpan<VisualDiagnosticSink, VisualDiagnosticFact>> release_span;
    std::unique_ptr<gpu::PinnedHostBuffer> pixels;
    std::unique_ptr<std::uint32_t, PixelDeviceDeleter> pixel_device;
    std::array<std::uint32_t, 2U * kPixelSampleCount> pixel_coordinates{};
    bool probe_failed = false;
    bool probe_pending = false;
    bool probe_source_copy_pending = false;
    VisualDiagnosticSink diagnostics;

   private:
    template <class Release>
    void Cleanup(VisualDiagnosticOperation started, VisualDiagnosticOperation completed, Release&& release) noexcept {
        services::RuntimeDiagnosticSpan span(diagnostics, [&] {
            return visual_diagnostic_boundary(VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Presentation,
                                                                   .operation = started,
                                                                   .device = context.device(),
                                                                   .generation = generation,
                                                                   .context = {.surface_high = description.id_high,
                                                                               .surface_low = description.id_low,
                                                                               .workspace = native::workspace_source_diagnostic(description)}},
                                              completed);
        });
        std::forward<Release>(release)();
        span.FinishWith([](auto& fact) { fact.context.outcome = 1U; });
    }
};
class NativePresentationWriter final : public PresentationNativeWriter {
   public:
    NativePresentationWriter(VisualDeviceSettings settings, PresentationNativeConfiguration configuration, VisualDiagnosticSink diagnostics,
                             gpu::DeviceExecution execution)
        : NativePresentationWriter(
              settings, std::move(configuration), diagnostics, execution,
              gpu::DeviceContext(settings.device, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, settings.numa_node, execution)) {}
    NativePresentationWriter(VisualDeviceSettings settings, PresentationNativeConfiguration configuration, VisualDiagnosticSink diagnostics,
                             gpu::DeviceExecution execution, gpu::DeviceContext context)
        : settings_(settings),
          configuration_(std::move(configuration)),
          diagnostics_(diagnostics),
          execution_(std::move(execution)),
          channel_(configuration_.import_socket, diagnostics_),
          context_(std::move(context)),
          wake_(std::make_shared<ScopedFd>(::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK))) {
        if (wake_->get() < 0) throw std::runtime_error("presentation completion eventfd creation failed");
        completion_.fd = wake_->get();
        sources_.reserve(kSourceCapacity);
        if (configuration_.completion_acceptance) {
            configuration_.completion_acceptance->SetWake([wake = wake_] { static_cast<void>(mmltk::common::io::signal_event_fd(wake->get())); });
        }
    }
    ~NativePresentationWriter() override {
        if (configuration_.completion_acceptance) configuration_.completion_acceptance->SetWake({});
        if (SettlePhysicalWork() != Retirement::Released) std::terminate();
        try {
            for (auto& source : sources_) {
                source->context.Bind();
                source->pixel_device.reset();
                if (source->pixels) static_cast<void>(source->pixels->ReleaseSettled());
            }
        } catch (...) { std::terminate(); }
    }
    void Submit(PresentationSubmittedSource submitted, const VisualSourceReader& reader) override {
        if (!submitted.valid() || reader.source != submitted.observation.frame.source || !reader.observe_workspace || !reader.request_workspace || pending_)
            throw std::invalid_argument("direct workspace submission is invalid");
        pending_ = Pending{
            .submitted = submitted, .reader = &reader, .link = diagnostics_.valid() ? services::DiagnosticSpanIds::Next() : contracts::DiagnosticLink{}};
        if (reader.image_metadata) {
            auto metadata = reader.image_metadata(submitted.observation.frame);
            if (metadata) pending_->metadata = std::move(*metadata);
        }
    }
    PresentationNativeOutcome Pump(std::uint64_t selection) override {
        {
            services::RuntimeDiagnosticSpan channel_span(diagnostics_, [&] {
                return visual_diagnostic_boundary(VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Presentation,
                                                                       .operation = VisualDiagnosticOperation::PresentationChannelPumpStarted,
                                                                       .device = settings_.device},
                                                  VisualDiagnosticOperation::PresentationChannelPumpCompleted);
            });
            channel_.pump();
            channel_span.FinishWith([](auto& fact) { fact.context.outcome = 1U; });
        }
        if (const auto error = channel_.terminal_error()) throw std::runtime_error(*error);
        while (auto retired = channel_.take_retirement()) {
            for (auto& source : sources_)
                if (source->id() == retired->id && source->generation == retired->generation) source->retirement_requested = true;
            for (auto& arena : retiring_)
                if (arena && arena->id == retired->id) {
                    DiagnoseArena(VisualDiagnosticOperation::PresentationRetirement, *arena, 1U);
                    arena.reset();
                }
        }
        FinishSourceRetirements();
        while (auto transition = channel_.take_source_transition()) ObserveSourceTransition(*transition);
        while (auto outcome = channel_.take_outcome()) Accept(std::move(*outcome));
        if (channel_.take_capacity_wake() && configuration_.completion_acceptance) configuration_.completion_acceptance->ObserveCapacity();
        DrainCompletion();
        RetireSources();
        PresentationNativeOutcome result;
        if (transfer_ && stage_ == Stage::ReadyComplete) result = Publish();
        if (!transfer_ && result.progress != PresentationNativeProgress::Published &&
            ((active_ && active_->binding_retired) || (candidate_ && candidate_->binding_retired))) {
            result.capability = Capability();
            if (pending_ && pending_->destination && pending_->destination->preparing->load(std::memory_order_acquire)) return result;
            const std::ptrdiff_t needed = int(active_ && active_->binding_retired) + int(candidate_ && candidate_->binding_retired);
            if (std::ranges::count_if(retiring_, [](const auto& arena) { return !arena; }) < needed) return result;
            if (pending_ && pending_->destination) pending_->destination->workspace->CancelDisplayWrite();
            pending_.reset();
            if (candidate_ && candidate_->binding_retired) RetireArena(candidate_, VisualDiagnosticOperation::PresentationCandidateWithdrawal);
            if (active_ && active_->binding_retired) RetireArena(active_, VisualDiagnosticOperation::PresentationActiveWithdrawal);
            result.progress = PresentationNativeProgress::BindingRetired;
        }
        if (!transfer_ && pending_ && result.progress != PresentationNativeProgress::Published) result = Prepare(selection);
        result.capability = result.progress == PresentationNativeProgress::Published ? result.publication.capability : Capability();
        return result;
    }
    int poll_fd() const noexcept override { return channel_.poll_fd(); }
    int completion_fd() const noexcept override { return wake_->get(); }
    bool wants_write() const noexcept override { return channel_.wants_write(); }
    void SetExpectedBrowserProcessGroup(pid_t group) override { channel_.set_expected_process_group(group); }
    Retirement BrowserPeerLost() noexcept override {
        static_cast<void>(channel_.consume_peer_loss());
        channel_.reset_peer();
        for (auto& source : sources_)
            if (source->workspace) source->workspace->Withdraw();
        const auto settlement = SettlePhysicalWork(true);
        if (settlement != Retirement::Released) return settlement;
        bool released = true;
        try {
            for (auto& source : sources_) { source->retirement_requested = true; }
            while (!sources_.empty()) {
                released = !FinishSourceRetirements(false) && released;
                if (sources_.empty()) break;
                // Queued requests are weak. Only an executing synchronous attempt
                // can retain a detached allocation here; attached raw custody is
                // handed to its producer without waiting.
                pollfd wake{wake_->get(), POLLIN, 0};
                int status;
                do { status = ::poll(&wake, 1, -1); } while (status < 0 && errno == EINTR);
                if (status < 0 || (wake.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return Retirement::UnsafeFailure;
                std::uint64_t value;
                ssize_t read;
                do { read = ::read(wake_->get(), &value, sizeof(value)); } while (read < 0 && errno == EINTR);
                if (read < 0 && errno != EAGAIN) return Retirement::UnsafeFailure;
            }
            for (const auto* arena : {active_.get(), candidate_.get(), retiring_[0].get(), retiring_[1].get()})
                if (arena) DiagnoseArena(VisualDiagnosticOperation::PresentationRetirement, *arena, released ? 1U : 0U);
            active_.reset();
            candidate_.reset();
            for (auto& arena : retiring_) arena.reset();
            pending_.reset();
        } catch (...) { return Retirement::UnsafeFailure; }
        return released ? Retirement::Released : Retirement::ReleasedWithFailure;
    }
    void TerminalCustodyInstalled() noexcept override {
        for (const auto& source : sources_)
            if (source->transfer && source->workspace->Acquired(source->transfer->physical_revision))
                Diagnose(VisualDiagnosticOperation::PresentationTerminalReadRetained, *source->transfer, 1U);
    }

   private:
    enum class Stage : std::uint8_t { Idle, ReadyPending, ReadyComplete };
    static void EnqueueCompletion(gpu::ImageStream& stream, Completion& completion) {
        if (cudaStreamAddCallback(
                reinterpret_cast<cudaStream_t>(stream.native_handle()),
                [](cudaStream_t, cudaError_t status, void* pointer) {
                    auto& signal = *static_cast<Completion*>(pointer);
                    signal.status = status;
                    signal.ready.store(true, std::memory_order_release);
                    static_cast<void>(mmltk::common::io::signal_event_fd(signal.fd));
                },
                &completion, 0U) != cudaSuccess)
            throw std::runtime_error("presentation completion submission failed");
    }
    void ReleaseRead() noexcept {
        if (source_read_) source_read_->Complete();
        source_read_.reset();
    }
    Retirement SettlePhysicalWork(bool browser_terminal = false) noexcept {
        if (transfer_) {
            const auto ready = transfer_->source->stream.Settle();
            if (!ready.completion_reached || ready.failure || (completion_.ready.load(std::memory_order_acquire) && completion_.status != cudaSuccess)) {
                if (source_read_) source_read_->Quarantine();
                return Retirement::UnsafeFailure;
            }
        }
        ReleaseRead();
        auto result = Retirement::Released;
        for (auto& source : sources_) {
            if (!source->workspace) continue;
            try {
                source->context.Bind();
            } catch (...) { return Retirement::UnsafeFailure; }
            const bool acquired = source->transfer && source->workspace->Acquired(source->transfer->physical_revision);
            // A lost socket notification is not GPU completion. After browser
            // shutdown, its terminal dispatcher receipt proves the exact read
            // and final Vulkan release completed even if Acquired never arrived.
            const bool terminal_complete = browser_terminal && acquired && source->workspace->TerminalReadComplete(source->transfer->physical_revision);
            if (source->release_submitted && !acquired && !source->release_complete) return Retirement::UnsafeFailure;
            const auto status = source->transfer ? cudaStreamQuery(reinterpret_cast<cudaStream_t>(source->stream.native_handle())) : cudaSuccess;
            if (status != cudaSuccess && status != cudaErrorNotReady) return Retirement::UnsafeFailure;
            if (source->completion.ready.load(std::memory_order_acquire) && source->completion.status != cudaSuccess) return Retirement::UnsafeFailure;
            if ((acquired && !source->release_submitted && !terminal_complete) ||
                (acquired && source->release_submitted && status == cudaErrorNotReady && !terminal_complete)) {
                if (!browser_terminal) return Retirement::UnsafeFailure;
                result = Retirement::RetainedBrowserRead;
                continue;
            }
            const auto settled = source->stream.Settle();
            if (!settled.completion_reached || settled.failure ||
                (source->completion.ready.load(std::memory_order_acquire) && source->completion.status != cudaSuccess))
                return Retirement::UnsafeFailure;
            if (acquired && !source->release_complete && (source->release_submitted || terminal_complete)) {
                try {
                    source->workspace->CompleteRead(source->transfer->physical_revision);
                    source->release_complete = true;
                } catch (...) { return Retirement::UnsafeFailure; }
            }
            if (browser_terminal && source->transfer && source->release_complete)
                Diagnose(VisualDiagnosticOperation::PresentationTerminalReadCompleted, *source->transfer, 1U);
            source->workspace->CancelDisplayWrite();
            source->transfer.reset();
            source->exposed = source->acquired = source->release_submitted = source->release_complete = false;
            if (source->release_span) source->release_span->FinishWith([](auto& fact) { fact.context.outcome = 1U; });
            source->release_span.reset();
        }
        if (ready_span_) ready_span_->FinishWith([](auto& fact) { fact.context.outcome = 1U; });
        ready_span_.reset();
        transfer_.reset();
        stage_ = Stage::Idle;
        return result;
    }
    void DrainCompletion() {
        std::uint64_t edge = 0U;
        ssize_t read;
        do { read = ::read(wake_->get(), &edge, sizeof(edge)); } while (read < 0 && errno == EINTR);
        if (read < 0 && errno != EAGAIN) throw std::runtime_error("presentation completion wake failed");
        if (completion_.ready.exchange(false, std::memory_order_acq_rel)) {
            if (completion_.status != cudaSuccess) throw std::runtime_error("presentation asynchronous graphics work failed");
            if (stage_ != Stage::ReadyPending || !transfer_) throw std::runtime_error("presentation ready completion has no publication");
            stage_ = Stage::ReadyComplete;
            if (ready_span_) ready_span_->FinishWith([](auto& fact) { fact.context.outcome = 1U; });
            ready_span_.reset();
            ReportPixels(*transfer_);
        }
        for (auto& source : sources_) {
            if (!source->transfer) continue;
            auto& transfer = *source->transfer;
            if (configuration_.completion_acceptance && source->completion.ready.load(std::memory_order_acquire) &&
                configuration_.completion_acceptance->Hold({source->description.id_high, source->description.id_low, transfer.publication.transfer_sequence,
                                                            transfer.publication.presentation_revision}))
                continue;
            if (source->completion.ready.exchange(false, std::memory_order_acq_rel)) {
                if (source->completion.status != cudaSuccess) throw std::runtime_error("presentation asynchronous source release failed");
                ReportPixels(transfer, VisualDiagnosticOperation::PresentationPixelAfterRelease);
                source->workspace->CompleteRead(transfer.physical_revision);
                source->release_complete = true;
            }
            if (!source->release_complete) continue;
            const auto& frame = transfer.pending.submitted.observation.frame;
            if (!channel_.read_settled(source->id(), {presentation_source_session(frame.source.kind), frame.revision},
                                       transfer.publication.presentation_revision, transfer.publication.transfer_sequence))
                continue;
            if (source->release_span) source->release_span->FinishWith([](auto& fact) { fact.context.outcome = 1U; });
            source->release_span.reset();
            source->transfer.reset();
            source->exposed = source->acquired = source->release_submitted = source->release_complete = false;
        }
    }
    static PresentationCapability CapabilityOf(const SampleArena& arena) noexcept {
        return {.surface_high = arena.id.high,
                .surface_low = arena.id.low,
                .extent = arena.extent,
                .generation = arena.generation,
                .condition = arena.ready ? PresentationCapabilityCondition::Ready : PresentationCapabilityCondition::Admitted};
    }
    PresentationCapability Capability() const noexcept {
        const auto* arena = candidate_ ? candidate_.get() : active_.get();
        return arena && channel_.claimable(arena->id) ? CapabilityOf(*arena) : PresentationCapability{};
    }
    static bool Contains(const SampleArena& arena, VisualExtent extent) noexcept {
        return arena.extent.width >= extent.width && arena.extent.height >= extent.height;
    }
    void RetireArena(std::unique_ptr<SampleArena>& arena, VisualDiagnosticOperation operation) {
        const auto slot = std::ranges::find_if(retiring_, [](const auto& value) { return !value; });
        if (slot == retiring_.end()) throw std::runtime_error("Firefox arena retirement capacity exceeded");
        const auto withdrawal = channel_.withdraw(arena->id);
        if (withdrawal.progress == native::WorkspaceSurfaceWithdrawalProgress::Invalid ||
            withdrawal.progress == native::WorkspaceSurfaceWithdrawalProgress::Capacity)
            throw std::runtime_error("Firefox arena withdrawal failed");
        DiagnoseArena(operation, *arena, 1U);
        *slot = std::move(arena);
        RetireSources();
    }
    SampleArena* EnsureArena(VisualExtent extent) {
        if (candidate_) {
            if (Contains(*candidate_, extent)) return candidate_.get();
            if (configuration_.completion_acceptance && configuration_.completion_acceptance->SupersessionHeld()) return nullptr;
            if (candidate_->ready && std::ranges::any_of(retiring_, [](const auto& arena) { return !arena; }))
                RetireArena(candidate_, VisualDiagnosticOperation::PresentationCandidateWithdrawal);
            return nullptr;
        }
        if (active_ && Contains(*active_, extent)) return active_.get();
        // A retained browser fallback can outlive a newer native publication
        // that was superseded before acquisition. Admit the third arena so
        // growth can replace that intermediate image without waiting for the
        // fallback it must preserve. Every retiring arena still needs its
        // own physical retirement receipt.
        const auto live = std::ranges::count_if(retiring_, [](const auto& arena) { return bool(arena); }) + bool(active_);
        if (live >= 3) return nullptr;
        auto arena = std::make_unique<SampleArena>();
        arena->id = native::WorkspaceSurfaceImportId::generate();
        arena->generation = mmltk::common::types::take_monotonic_identity(next_generation_);
        arena->submitted = pending_->submitted;
        arena->extent = {active_ ? std::max(active_->extent.width, extent.width) : extent.width,
                         active_ ? std::max(active_->extent.height, extent.height) : extent.height};
        if (pending_supersession_acceptance_ && active_ && pending_->submitted.observation.frame.source.kind == PresentationSourceKind::Upscale) {
            pending_supersession_acceptance_ = false;
            arena->extent = active_->extent;
            if (configuration_.completion_acceptance) configuration_.completion_acceptance->HoldSupersession(arena->id.high, arena->id.low);
        }
        DiagnoseArena(VisualDiagnosticOperation::PresentationArenaAdvertised, *arena, 1U);
        if (!channel_.admit_arena(arena->id, arena->generation, arena->extent.width, arena->extent.height, arena->submitted.selection_generation,
                                  arena->submitted.observation.frame.revision))
            return nullptr;
        candidate_ = std::move(arena);
        return candidate_.get();
    }
    gpu::ImageWorkspaceLayout Layout(const SampleArena& arena) const {
        const auto& packet = arena.layout;
        gpu::ImageWorkspaceLayout layout{.device_incarnation = packet.device_incarnation,
                                         .device = settings_.device,
                                         .width = packet.width,
                                         .height = packet.height,
                                         .pitch_bytes = packet.stride,
                                         .offset_bytes = packet.offset,
                                         .required_allocation_bytes = std::max<std::uint64_t>(packet.size, configuration_.minimum_allocation_bytes),
                                         .alignment_bytes = packet.alignment,
                                         .dedicated = packet.dedicated != 0U,
                                         .direct_sampling = packet.direct_sampling != 0U};
        std::ranges::copy(packet.device_uuid, layout.device_uuid.begin());
        if (!layout.valid()) throw std::runtime_error("Firefox returned an invalid workspace layout");
        return layout;
    }
    void Request(const VisualSourceReader& reader, const gpu::ImageWorkspaceObservation& observed, AdmittedSource& source, bool detach_only = false) {
        source.preparing->store(true, std::memory_order_release);
        VisualWorkspaceRequest request{.product_owner = observed.product_owner,
                                       .product_revision = observed.product_revision,
                                       .detach_only = detach_only,
                                       .destination = source.workspace,
                                       .ready = [completion = source.preparing, weak = std::weak_ptr{wake_}] {
                                           completion->store(false, std::memory_order_release);
                                           if (const auto wake = weak.lock()) static_cast<void>(mmltk::common::io::signal_event_fd(wake->get()));
                                       }};
        try {
            reader.request_workspace(std::move(request));
        } catch (...) {
            source.preparing->store(false, std::memory_order_release);
            source.workspace->CancelDisplayWrite();
            throw;
        }
    }
    void Accept(native::WorkspaceSurfaceImportOutcome outcome) {
        if (candidate_ && candidate_->id == outcome.id) {
            DiagnoseArena(VisualDiagnosticOperation::PresentationImportOutcome, *candidate_, outcome.imported ? 1U : 0U);
            if (!outcome.imported) {
                if (candidate_->binding_retired) return;
                throw std::runtime_error("Firefox arena admission failed");
            }
            if (outcome.layout.opcode != abi::Opcode::ArenaReady || outcome.layout.width != candidate_->extent.width ||
                outcome.layout.height != candidate_->extent.height)
                throw std::runtime_error("Firefox arena layout identity mismatch");
            candidate_->layout = outcome.layout;
            static_cast<void>(Layout(*candidate_));
            candidate_->ready = true;
            return;
        }
        for (auto& source : sources_) {
            if (source->id() != outcome.id) continue;
            if (!outcome.imported) {
                if (source->withdrawing || (active_ && active_->id == source->arena() && active_->binding_retired) ||
                    (candidate_ && candidate_->id == source->arena() && candidate_->binding_retired))
                    return;
                throw std::runtime_error("Firefox source admission failed");
            }
            if (source->withdrawing) return;
            if (outcome.memory_descriptor.get() < 0) throw std::runtime_error("Firefox source memory is unavailable");
            if (outcome.timeline_descriptor.get() < 0) throw std::runtime_error("Firefox source timeline is unavailable");
            source->context.Bind();
            DiagnoseSource(VisualDiagnosticOperation::PresentationSourceTimelineImportStarted, *source, 0U);
            source->timeline.emplace(std::move(outcome.timeline_descriptor));
            if (!source->workspace->QueueAllocation(std::move(outcome.memory_descriptor))) return;
            source->workspace->Admit(source->workspace->identity(), source->description.device_incarnation);
            DiagnoseSource(VisualDiagnosticOperation::PresentationSourceReady, *source, 1U);
            const auto* arena = candidate_ && candidate_->id == source->arena() ? candidate_.get() : active_.get();
            if (!arena || arena->id != source->arena()) throw std::runtime_error("Firefox source belongs to a retired arena");
            // Prepare owns current producer demand. A delayed import receipt
            // must not replace it with the product that initiated this source.
            return;
        }
        throw std::runtime_error("Firefox returned an unknown source admission");
    }
    void Withdraw(AdmittedSource& source) {
        if (source.withdrawing) return;
        source.workspace->Withdraw();
        if (source.acquired || source.release_submitted || (transfer_ && transfer_->source == &source) ||
            (source.transfer && source.workspace->Acquired(source.transfer->physical_revision)))
            return;
        const auto withdrawal = channel_.withdraw(source.id());
        if (withdrawal.progress == native::WorkspaceSurfaceWithdrawalProgress::Invalid ||
            withdrawal.progress == native::WorkspaceSurfaceWithdrawalProgress::Capacity)
            throw std::runtime_error("Firefox source withdrawal failed");
        source.withdrawing = true;
        DiagnoseSource(VisualDiagnosticOperation::PresentationSourceWithdrawal, source, 1U);
    }
    void RetireSources() {
        for (auto& source : sources_)
            if (source->workspace &&
                (source->workspace->retired() || std::ranges::any_of(retiring_, [&](const auto& arena) { return arena && source->arena() == arena->id; })))
                Withdraw(*source);
    }
    std::exception_ptr FinishSourceRetirements(bool propagate = true) {
        std::exception_ptr failure;
        std::erase_if(sources_, [&](auto& source) {
            if (!source->retirement_requested) return false;
            if (!source->workspace_release_started) {
                source->context.Bind();
                DiagnoseSource(VisualDiagnosticOperation::PresentationSourceRetirementStarted, *source, 0U);
            }
            const auto released = source->ReleaseWorkspace();
            if (!released) return false;
            failure = gpu::combine_image_failures(failure, *released);
            DiagnoseSource(VisualDiagnosticOperation::PresentationSourceRetirement, *source, *released ? 0U : 1U);
            return true;
        });
        if (failure && propagate) std::rethrow_exception(failure);
        return failure;
    }
    PresentationNativeOutcome Supersede() {
        const auto submitted = pending_->submitted;
        pending_.reset();
        return {.progress = PresentationNativeProgress::Superseded, .submitted = submitted};
    }
    PresentationNativeOutcome Prepare(std::uint64_t selection) {
        const auto wait = [&](std::string_view reason, const SampleArena* arena = nullptr, const gpu::ImageWorkspaceObservation* observed = nullptr,
                              const AdmittedSource* source = nullptr) {
            diagnostics_.Emit([&] {
                auto fact = presentation_diagnostic_fact(
                    VisualDiagnosticOperation::PresentationWorkspaceWait,
                    {.submitted = pending_->submitted, .publication = {.capability = arena ? CapabilityOf(*arena) : PresentationCapability{}}},
                    settings_.device);
                fact.failure_detail = reason;
                auto& progress = fact.context.workspace_progress;
                progress.requested_product_revision = pending_->submitted.observation.frame.revision;
                progress.suppressed_request_owner = pending_->requested_owner;
                progress.live_source_count = sources_.size();
                if (arena && arena->ready) {
                    const auto layout = Layout(*arena);
                    progress.expected_workspace_pitch = layout.pitch_bytes;
                    progress.expected_workspace_bytes = layout.required_allocation_bytes;
                    progress.expected_device_incarnation = layout.device_incarnation;
                    progress.workspace_layout_matches = observed && observed->workspace && observed->workspace->layout() == layout;
                }
                if (source) {
                    fact.context.workspace = native::workspace_source_diagnostic(source->description);
                    progress.admitted_allocation = source->workspace->identity();
                    progress.source_timeline_imported = source->timeline.has_value();
                    progress.source_acquired = source->acquired;
                    progress.source_release_submitted = source->release_submitted;
                    progress.source_withdrawing = source->withdrawing;
                }
                if (observed) {
                    progress.requested_product_owner = observed->product_owner;
                    progress.observed_product_owner = observed->product_owner;
                    progress.observed_product_revision = observed->product_revision;
                    if (observed->workspace) { observe_workspace_storage(fact.context, *observed->workspace); }
                }
                return fact;
            });
            return PresentationNativeOutcome{};
        };
        if (!channel_.connected()) return wait("peer_unavailable");
        if (pending_->reader->image_metadata && pending_->metadata.empty()) return Supersede();
        if (pending_->destination && pending_->destination->preparing->load(std::memory_order_acquire))
            return wait("producer_finalization_pending", nullptr, nullptr, pending_->destination);
        if (pending_->submitted.selection_generation != selection) {
            if (pending_->destination) pending_->destination->workspace->CancelDisplayWrite();
            return Supersede();
        }
        const auto& frame = pending_->submitted.observation.frame;
        auto* arena = EnsureArena(frame.extent);
        if (!arena || !arena->ready) return wait(arena ? "arena_not_ready" : "arena_unavailable", arena);
        const auto observed = pending_->reader->observe_workspace();
        if (observed.product_owner == 0U) return wait("product_unavailable", arena, &observed);
        if (observed.product_revision != frame.revision) {
            if (pending_->destination) pending_->destination->workspace->CancelDisplayWrite();
            return Supersede();
        }
        auto* admitted = pending_->destination;
        if (!admitted) {
            for (auto& source : sources_) {
                if (source->workspace == observed.workspace && !source->withdrawing && source->arena() == arena->id && !source->exposed && !source->transfer &&
                    !source->preparing->load(std::memory_order_acquire)) {
                    admitted = source.get();
                    break;
                }
            }
            for (auto& source : sources_)
                if (!admitted && !source->withdrawing && source->arena() == arena->id && !source->exposed && !source->transfer &&
                    !source->preparing->load(std::memory_order_acquire)) {
                    admitted = source.get();
                    break;
                }
            if (!admitted) {
                const auto count = std::ranges::count_if(sources_, [&](const auto& source) { return !source->withdrawing && source->arena() == arena->id; });
                if (count == 2 || sources_.size() == kSourceCapacity) return wait("shared_back_slot_unavailable", arena, &observed);
                const auto id = native::WorkspaceSurfaceImportId::generate();
                auto workspace = gpu::ImageWorkspace::Create(context_, Layout(*arena), execution_);
                auto packet = arena->layout;
                packet.opcode = abi::Opcode::Allocate;
                packet.descriptors = abi::kAllocateDescriptorCount;
                packet.id_high = id.high;
                packet.id_low = id.low;
                packet.arena_high = arena->id.high;
                packet.arena_low = arena->id.low;
                packet.allocation_identity = workspace->identity();
                packet.size = workspace->allocation_bytes();
                auto source = std::make_unique<AdmittedSource>(packet, context_, wake_->get(), diagnostics_);
                source->AdoptWorkspace(std::move(workspace), wake_);
                source->generation = mmltk::common::types::take_monotonic_identity(next_generation_);
                source->edge.reset(::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK));
                if (source->edge.get() < 0) throw std::runtime_error("native source eventfd creation failed");
                source->signal = native::WorkspaceSurfaceFrameSignal::create();
                auto access = source->workspace->ExportAccessDescriptor();
                if (!channel_.admit_source(source->description, source->generation, source->edge.get(), source->signal.descriptor(), access.get(),
                                           pending_->submitted.selection_generation, frame.revision))
                    return wait("source_admission_backpressure", arena, &observed);
                admitted = source.get();
                sources_.push_back(std::move(source));
            }
            pending_->destination = admitted;
        }
        if (!admitted->timeline || !admitted->workspace->admitted()) return wait("source_admission_pending", arena, &observed, admitted);
        if (admitted->producer && admitted->producer != pending_->reader && admitted->workspace->product_owner() != 0U) {
            if (!admitted->detaching) {
                admitted->detaching = true;
                Request(*admitted->producer, {}, *admitted, true);
            }
            return wait("previous_raw_product_detachment", arena, &observed, admitted);
        }
        admitted->detaching = false;
        admitted->producer = pending_->reader;
        services::RuntimeDiagnosticSpan borrow_span(
            diagnostics_,
            [&] {
                auto fact = presentation_diagnostic_fact(
                    VisualDiagnosticOperation::PresentationSourceBorrowStarted,
                    {.submitted = pending_->submitted, .publication = {.capability = CapabilityOf(*arena)}, .link = pending_->link}, settings_.device);
                fact.context.workspace = native::workspace_source_diagnostic(admitted->description);
                return visual_diagnostic_boundary(fact, VisualDiagnosticOperation::PresentationSourceBorrowCompleted);
            },
            pending_->link);
        if (pending_->requested_owner == 0U) {
            const bool direct = observed.workspace == admitted->workspace && admitted->workspace->Contains({observed.product_owner, frame.revision});
            auto raw = direct ? pending_->reader->borrow() : gpu::BorrowedImageProductReadView{};
            if (direct && (!raw.valid() || raw.plane(0U).revision() != frame.revision)) return Supersede();
            if (!admitted->workspace->ReserveDisplayWrite()) return wait("shared_back_slot_unavailable", arena, &observed, admitted);
            pending_->requested_owner = observed.product_owner;
            if (direct) {
                source_read_ = std::make_unique<gpu::ImageProductReadCompletion>(std::move(raw));
            } else {
                admitted->workspace->InvalidateWrite();
                Request(*pending_->reader, observed, *admitted);
                return wait("producer_finalization_pending", arena, &observed, admitted);
            }
        }
        if (!admitted->workspace->Contains({pending_->requested_owner, frame.revision})) {
            admitted->workspace->CancelDisplayWrite();
            return Supersede();
        }
        try {
            borrow_span.FinishWith([](auto& fact) { fact.context.outcome = 1U; });
            const auto transfer = mmltk::common::types::take_monotonic_identity(admitted->next_transfer);
            transfer_.emplace(Transfer{.source = admitted,
                                       .pending = std::move(*pending_),
                                       .publication = {.capability = CapabilityOf(*arena),
                                                       .timeline_ready = native::detail::workspace_timeline_ready(transfer),
                                                       .presentation_revision = mmltk::common::types::take_monotonic_identity(next_publication_),
                                                       .transfer_sequence = transfer},
                                       .plane = admitted->workspace->plane(frame.extent.width, frame.extent.height),
                                       .physical_revision = admitted->workspace->revision()});
            diagnostics_.Emit([&] {
                auto fact = Fact(VisualDiagnosticOperation::PresentationSourceReadSubmitted, *transfer_, 0U);
                fact.value = 0U;
                fact.context.publication = {};
                fact.context.transfer = {};
                return fact;
            });
            SamplePixels(*transfer_, admitted->stream);
            ready_span_.emplace(
                diagnostics_,
                [&] {
                    return visual_diagnostic_boundary(Fact(VisualDiagnosticOperation::PresentationReadySyncStarted, *transfer_, 0U),
                                                      VisualDiagnosticOperation::PresentationReadySyncCompleted);
                },
                transfer_->pending.link);
            admitted->timeline->SignalReady(reinterpret_cast<cudaStream_t>(admitted->stream.native_handle()), transfer_->publication.timeline_ready);
            stage_ = Stage::ReadyPending;
            EnqueueCompletion(admitted->stream, completion_);
            static_cast<void>(wait("ready_completion_pending", arena, &observed, admitted));
            pending_.reset();
        } catch (...) {
            static_cast<void>(SettlePhysicalWork());
            throw;
        }
        return {};
    }
    PresentationNativeOutcome Publish() {
        auto& source = *transfer_->source;
        source.transfer = std::move(transfer_);
        transfer_.reset();
        ReleaseRead();
        auto& transfer = *source.transfer;
        const auto& frame = transfer.pending.submitted.observation.frame;
        // A completed offer consumes no external read. Acquisition owns the
        // read; only its submitted Vulkan release can arm native completion.
        static_cast<void>(channel_.take_capacity_wake());
        native::detail::publish_workspace_frame_signal(
            source.signal.mapping(), transfer.publication.timeline_ready, transfer.publication.transfer_sequence, native::WorkspacePresentationLayer::Primary,
            {presentation_source_session(frame.source.kind), frame.revision}, transfer.publication.presentation_revision, frame.extent.width,
            frame.extent.height, transfer.physical_revision, transfer.pending.metadata);
        source.workspace->CancelWrite();
        source.exposed = true;
        if (!mmltk::common::io::signal_event_fd(source.edge.get())) throw std::runtime_error("native source frame publication failed");
        Diagnose(VisualDiagnosticOperation::PresentationFrameEdge, transfer, transfer.publication.timeline_ready);
        stage_ = Stage::Idle;
        if (candidate_ && source.arena() == candidate_->id) {
            if (active_) RetireArena(active_, VisualDiagnosticOperation::PresentationActiveWithdrawal);
            active_ = std::move(candidate_);
            DiagnoseArena(VisualDiagnosticOperation::PresentationReplacement, *active_, 1U);
        }
        PresentationNativeOutcome outcome{.progress = PresentationNativeProgress::Published,
                                          .submitted = transfer.pending.submitted,
                                          .publication = transfer.publication,
                                          .diagnostic_link = transfer.pending.link};
        return outcome;
    }
    void ObserveSourceTransition(const abi::Record& record) {
        if (record.opcode == abi::Opcode::BindingRetired) {
            const native::WorkspaceSurfaceImportId id{record.id_high, record.id_low};
            for (auto* arena : {active_.get(), candidate_.get()})
                if (arena && arena->id == id) arena->binding_retired = true;
            return;
        }
        const auto found = std::ranges::find_if(
            sources_, [&](const auto& source) { return source->description.id_high == record.id_high && source->description.id_low == record.id_low; });
        if (found == sources_.end()) throw std::runtime_error("workspace acquisition source is unavailable");
        auto& source = **found;
        if (!source.transfer || source.release_submitted) throw std::runtime_error("workspace source transition is duplicate or unavailable");
        auto& transfer = *source.transfer;
        const auto& frame = transfer.pending.submitted.observation.frame;
        if (record.offset != transfer.publication.transfer_sequence || record.presentation_revision != transfer.publication.presentation_revision ||
            record.stride != presentation_source_session(frame.source.kind) || record.size != frame.revision ||
            !source.workspace->Acquired(transfer.physical_revision))
            throw std::runtime_error("workspace acquisition does not own the exact completed publication");
        if (record.opcode == abi::Opcode::Acquired) {
            if (source.acquired) throw std::runtime_error("workspace acquisition is duplicate");
            source.acquired = true;
            return;
        }
        if (record.opcode != abi::Opcode::ReleaseSubmitted || !source.acquired) throw std::runtime_error("workspace release submission has no acquisition");
        source.release_span.emplace(
            diagnostics_,
            [&] {
                return visual_diagnostic_boundary(Fact(VisualDiagnosticOperation::PresentationReleaseWaitStarted, transfer, 0U),
                                                  VisualDiagnosticOperation::PresentationReleaseWaitCompleted);
            },
            transfer.pending.link);
        source.context.Bind();
        source.timeline->WaitForRelease(reinterpret_cast<cudaStream_t>(source.stream.native_handle()), transfer.publication.timeline_ready + 1U);
        source.release_submitted = true;
        SamplePixels(transfer, source.stream);
        EnqueueCompletion(source.stream, source.completion);
    }
    void SamplePixels(Transfer& transfer, gpu::ImageStream& stream) noexcept {
        auto& source = *transfer.source;
        if (!diagnostics_.pixel_probes_enabled() || source.probe_failed) return;
        source.probe_source_copy_pending = false;
        try {
            if (!source.pixels) {
                source.pixels = gpu::PinnedHostBuffer::ForCurrentDevice();
                source.pixels->ensure_bytes(2U * kPixelSampleCount * sizeof(std::uint32_t));
                void* memory = nullptr;
                if (cudaMalloc(&memory, kPixelSampleCount * sizeof(std::uint32_t)) != cudaSuccess) throw std::runtime_error("probe allocation failed");
                source.pixel_device.reset(static_cast<std::uint32_t*>(memory));
            }
            const auto plane = transfer.plane;
            std::array<std::uint32_t, 2U * kPixelSampleCount> coordinates{};
            const auto coordinate = [](std::size_t index, std::uint32_t size) {
                const std::array points{0U, std::min(191U, size - 1U), std::min(383U, size - 1U), (size - 1U) / 2U, size - 1U};
                return points[index];
            };
            for (std::size_t index = 0U; index != kPixelSampleCount; ++index) {
                coordinates[2U * index] = coordinate(index % 5U, plane.descriptor.width);
                coordinates[2U * index + 1U] = coordinate(index / 5U, plane.descriptor.height);
            }
            source.pixel_coordinates = coordinates;
            services::RuntimeDiagnosticSpan probe_span(
                diagnostics_,
                [&] {
                    return visual_diagnostic_boundary(Fact(VisualDiagnosticOperation::PresentationPixelProbeStarted, transfer, 0U),
                                                      VisualDiagnosticOperation::PresentationPixelProbeSubmitted);
                },
                transfer.pending.link);
            if (mmltk::backend::imaging::raster::probe_rgba({reinterpret_cast<const std::uint8_t*>(plane.data), plane.descriptor.pitch_bytes,
                                                             static_cast<int>(plane.descriptor.width), static_cast<int>(plane.descriptor.height)},
                                                            source.pixel_device.get(), coordinates, stream.native_handle()) != cudaSuccess ||
                cudaMemcpyAsync(source.pixels->data(), source.pixel_device.get(), kPixelSampleCount * sizeof(std::uint32_t), cudaMemcpyDeviceToHost,
                                reinterpret_cast<cudaStream_t>(stream.native_handle())) != cudaSuccess)
                throw std::runtime_error("probe submission failed");
            source.probe_pending = true;
            // Observe the source bytes through the transfer path independently
            // of the kernel's device scratch. Partial submission retains both
            // host regions until the existing stream completion settles them.
            auto* copied = static_cast<std::uint32_t*>(source.pixels->data()) + kPixelSampleCount;
            for (std::size_t index = 0U; index != kPixelSampleCount; ++index) {
                const auto offset = static_cast<std::size_t>(coordinates[2U * index + 1U]) * plane.descriptor.pitch_bytes +
                                    static_cast<std::size_t>(coordinates[2U * index]) * sizeof(std::uint32_t);
                if (cudaMemcpyAsync(copied + index, reinterpret_cast<const void*>(plane.data + offset), sizeof(std::uint32_t), cudaMemcpyDeviceToHost,
                                    reinterpret_cast<cudaStream_t>(stream.native_handle())) != cudaSuccess)
                    throw std::runtime_error("source-copy probe submission failed");
            }
            source.probe_source_copy_pending = true;
            probe_span.FinishWith([](auto& fact) { fact.context.outcome = 1U; });
        } catch (...) { source.probe_failed = true; }
    }
    void ReportPixels(Transfer& transfer, VisualDiagnosticOperation operation = VisualDiagnosticOperation::PresentationPixel) {
        auto& source = *transfer.source;
        if (!source.probe_pending) return;
        source.probe_pending = false;
        const bool copied = std::exchange(source.probe_source_copy_pending, false);
        if (!diagnostics_.pixel_probes_enabled()) return;
        const auto* values = static_cast<const std::uint32_t*>(source.pixels->data());
        const std::array operations{operation, operation == VisualDiagnosticOperation::PresentationPixelAfterRelease
                                                   ? VisualDiagnosticOperation::PresentationPixelSourceCopyAfterRelease
                                                   : VisualDiagnosticOperation::PresentationPixelSourceCopyBeforeReady};
        std::array<VisualDiagnosticFact, kPixelSampleCount> facts;
        for (std::size_t probe = 0U; probe != (copied ? operations.size() : 1U); ++probe) {
            for (std::size_t index = 0U; index != facts.size(); ++index) {
                facts[index] = Fact(operations[probe], transfer, 0U);
                facts[index].context.pixel = {.sample_index = static_cast<std::uint32_t>(index),
                                              .sample_x = source.pixel_coordinates[2U * index],
                                              .sample_y = source.pixel_coordinates[2U * index + 1U],
                                              .sample_rgba = values[probe * kPixelSampleCount + index]};
            }
            diagnostics_.WriteBatch(facts);
        }
    }
    VisualDiagnosticFact Fact(VisualDiagnosticOperation operation, const Transfer& transfer, std::uint64_t outcome) const noexcept {
        auto fact = presentation_diagnostic_fact(operation,
                                                 {.submitted = transfer.pending.submitted, .publication = transfer.publication, .link = transfer.pending.link},
                                                 settings_.device, outcome);
        fact.context.workspace = native::workspace_source_diagnostic(transfer.source->description);
        fact.context.workspace.native_process_id = static_cast<std::uint64_t>(::getpid());
        fact.context.workspace.workspace_plane = transfer.plane.data;
        return fact;
    }
    void DiagnoseSource(VisualDiagnosticOperation operation, const AdmittedSource& source, std::uint64_t outcome) const noexcept {
        diagnostics_.Emit([&] {
            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Presentation,
                                        .operation = operation,
                                        .device = settings_.device,
                                        .generation = source.generation,
                                        .context = {.capacity_width = source.description.width,
                                                    .capacity_height = source.description.height,
                                                    .surface_high = source.description.id_high,
                                                    .surface_low = source.description.id_low,
                                                    .outcome = outcome,
                                                    .workspace = native::workspace_source_diagnostic(source.description)}};
        });
    }
    void DiagnoseArena(VisualDiagnosticOperation operation, const SampleArena& arena, std::uint64_t outcome) const noexcept {
        diagnostics_.Emit([&] {
            auto fact = presentation_diagnostic_fact(operation, {.submitted = arena.submitted, .publication = {.capability = CapabilityOf(arena)}},
                                                     settings_.device, outcome);
            if (operation == VisualDiagnosticOperation::PresentationImportOutcome) fact.value = outcome;
            return fact;
        });
    }
    void Diagnose(VisualDiagnosticOperation operation, const Transfer& transfer, std::uint64_t outcome) const noexcept {
        if (!diagnostics_.valid()) return;
        std::array<char, 16U> fingerprint{};
        diagnostics_.Emit([&] {
            auto fact = Fact(operation, transfer, outcome);
            if (operation == VisualDiagnosticOperation::PresentationFrameEdge) {
                std::uint64_t hash = 0xcbf29ce484222325ULL;
                for (const auto byte : transfer.pending.metadata) hash = (hash ^ std::to_integer<std::uint8_t>(byte)) * 0x100000001b3ULL;
                fact.context.metadata_bytes = transfer.pending.metadata.size();
                fingerprint.fill('0');
                std::array<char, 16U> digits{};
                const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), hash, 16);
                const auto length = static_cast<std::size_t>(converted.ptr - digits.data());
                std::copy_n(digits.begin(), length, fingerprint.end() - static_cast<std::ptrdiff_t>(length));
                fact.context.metadata_fingerprint = {fingerprint.data(), fingerprint.size()};
            }
            return fact;
        });
    }
    static constexpr std::size_t kSourceCapacity = 6U;
    VisualDeviceSettings settings_;
    PresentationNativeConfiguration configuration_;
    bool pending_supersession_acceptance_ = configuration_.pending_supersession_acceptance;
    VisualDiagnosticSink diagnostics_;
    std::optional<services::RuntimeDiagnosticSpan<VisualDiagnosticSink, VisualDiagnosticFact>> ready_span_;
    gpu::DeviceExecution execution_;
    native::WorkspaceSurfaceImportChannel channel_;
    gpu::DeviceContext context_;
    std::shared_ptr<ScopedFd> wake_;
    Completion completion_;
    std::vector<std::unique_ptr<AdmittedSource>> sources_;
    std::unique_ptr<SampleArena> active_, candidate_;
    std::array<std::unique_ptr<SampleArena>, 2> retiring_;
    std::optional<Pending> pending_;
    std::optional<Transfer> transfer_;
    std::unique_ptr<gpu::ImageProductReadCompletion> source_read_;
    std::uint64_t next_generation_ = 1U, next_publication_ = 1U;
    Stage stage_ = Stage::Idle;
    friend struct test_support::NativePresentationWriterTestAccess;
};
}  // namespace
std::unique_ptr<PresentationNativeWriter> test_support::NativePresentationWriterTestAccess::Create(gpu::DeviceContext context,
                                                                                                   PresentationNativeConfiguration configuration,
                                                                                                   std::shared_ptr<gpu::ImageWorkspace> workspace,
                                                                                                   VisualDiagnosticSink diagnostics) {
    const VisualDeviceSettings settings{context.device(), workspace->layout().width, workspace->layout().height};
    auto result = std::make_unique<NativePresentationWriter>(settings, std::move(configuration), diagnostics, gpu::DeviceExecution{}, std::move(context));
    abi::Record description{};
    description.id_low = workspace->identity();
    auto source = std::make_unique<AdmittedSource>(description, result->context_, result->wake_->get(), diagnostics);
    source->AdoptWorkspace(std::move(workspace), result->wake_);
    result->sources_.push_back(std::move(source));
    return result;
}
void test_support::NativePresentationWriterTestAccess::RetireSource(PresentationNativeWriter& writer) {
    auto& native = dynamic_cast<NativePresentationWriter&>(writer);
    for (auto& source : native.sources_) source->retirement_requested = true;
    static_cast<void>(native.Pump(0U));
}
PresentationNativeWriterFactory make_native_presentation_writer_factory(VisualDeviceSettings settings, PresentationNativeConfiguration configuration,
                                                                        VisualDiagnosticSink diagnostics) {
    if (!settings.valid() || configuration.import_socket.empty()) throw contracts::InvalidIntentError("Presentation configuration is invalid");
    return [settings, configuration = std::move(configuration), diagnostics, execution = resolve_visual_device_execution(settings)] {
        return std::make_unique<NativePresentationWriter>(settings, configuration, diagnostics, execution);
    };
}
}  // namespace mmltk::controller
