#include "src/controller/presentation/presentation_system.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
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
#include "src/controller/presentation/detail/workspace_frame_signal.h"
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
    {
        std::scoped_lock lock(mutex_);
        if (stopped_) return false;
        if (held_) return true;
        if (!armed_) return false;
        armed_ = false;
        held_ = true;
        receipt_ = receipt;
        observe = observer_;
    }
    if (observe) observe(receipt);
    return true;
}
void PresentationAcceptanceGate::ObserveCapacity() {
    std::function<void(Receipt)> observe;
    Receipt receipt;
    {
        std::scoped_lock lock(mutex_);
        if (stopped_ || !held_ || receipt_.capacity_available) return;
        receipt_.capacity_available = true;
        receipt = receipt_;
        observe = observer_;
    }
    if (observe) observe(receipt);
}
void PresentationAcceptanceGate::Stop() noexcept {
    std::function<void()> wake;
    {
        std::scoped_lock lock(mutex_);
        stopped_ = true;
        armed_ = false;
        held_ = false;
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
};
struct AdmittedSource final {
    explicit AdmittedSource(const abi::Record& imported) : description(imported) {}

    [[nodiscard]] native::WorkspaceSurfaceImportId id() const noexcept {
        return {description.id_high, description.id_low};
    }
    [[nodiscard]] native::WorkspaceSurfaceImportId arena() const noexcept {
        return {description.arena_high, description.arena_low};
    }

    const abi::Record description;
    std::shared_ptr<gpu::ImageWorkspace> workspace;
    std::uint64_t generation = 0U;
    const VisualSourceReader* reader = nullptr;
    gpu::ImageWorkspaceObservation product{};
    std::optional<gpu::ExternalGraphicsTimeline> timeline;
    ScopedFd edge;
    native::WorkspaceSurfaceFrameSignal signal;
    std::uint64_t next_transfer = 1U;
    bool withdrawing = false;
};
struct PixelDeviceDeleter final {
    void operator()(std::uint32_t* value) const noexcept {
        if (value) static_cast<void>(cudaFree(value));
    }
};

class NativePresentationWriter final : public PresentationNativeWriter {
   public:
    NativePresentationWriter(VisualDeviceSettings settings, PresentationNativeConfiguration configuration,
                             VisualDiagnosticSink diagnostics, gpu::DeviceExecution execution)
        : settings_(settings), configuration_(std::move(configuration)), diagnostics_(diagnostics), execution_(std::move(execution)),
          channel_(configuration_.import_socket, diagnostics_),
          context_(settings.device, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, settings.numa_node, execution_),
          stream_(context_), wake_(std::make_shared<ScopedFd>(::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK))) {
        if (wake_->get() < 0) throw std::runtime_error("presentation completion eventfd creation failed");
        completion_.fd = wake_->get();
        sources_.reserve(kSourceCapacity);
        if (configuration_.completion_acceptance) {
            configuration_.completion_acceptance->SetWake([wake = wake_] {
                static_cast<void>(mmltk::common::io::signal_event_fd(wake->get()));
            });
        }
    }
    ~NativePresentationWriter() override {
        if (configuration_.completion_acceptance) configuration_.completion_acceptance->SetWake({});
        if (!SettlePhysicalWork()) std::terminate();
        try {
            context_.Bind();
            pixel_device_.reset();
            if (pixels_) static_cast<void>(pixels_->ReleaseSettled());
        } catch (...) { std::terminate(); }
    }
    void Submit(PresentationSubmittedSource submitted, const VisualSourceReader& reader) override {
        if (pending_ && pending_->reuse_publication != 0U) pending_.reset();
        if (!submitted.valid() || reader.source != submitted.observation.frame.source || !reader.observe_workspace ||
            !reader.borrow_workspace || !reader.request_workspace || pending_)
            throw std::invalid_argument("direct workspace submission is invalid");
        pending_ = Pending{.submitted = submitted, .reader = &reader,
                           .link = diagnostics_.valid() ? services::DiagnosticSpanIds::Next() : contracts::DiagnosticLink{}};
    }
    PresentationNativeOutcome Pump(std::uint64_t selection) override {
        context_.Bind();
        channel_.pump();
        if (const auto error = channel_.terminal_error()) throw std::runtime_error(*error);
        while (auto retired = channel_.take_retirement()) {
            std::erase_if(sources_, [&](auto& source) {
                if (source->id() != retired->id || source->generation != retired->generation) return false;
                if (source->timeline && source->timeline->Release() != cudaSuccess)
                    throw std::runtime_error("native source timeline retirement failed");
                DiagnoseSource(VisualDiagnosticOperation::PresentationSourceRetirement, *source, 1U);
                return true;
            });
            if (retiring_ && retiring_->id == retired->id) {
                DiagnoseArena(VisualDiagnosticOperation::PresentationRetirement, *retiring_, 1U);
                retiring_.reset();
            }
        }
        while (auto outcome = channel_.take_outcome()) Accept(std::move(*outcome));
        DrainCompletion();
        if (configuration_.completion_acceptance && transfer_ && stage_ == Stage::ReleasePending &&
            channel_.has_capacity_wake(transfer_->source->arena())) configuration_.completion_acceptance->ObserveCapacity();
        RetireSources();
        // Available remains in the channel while another submission owns the
        // writer. In particular, its socket edge can precede the release callback.
        if (!pending_ && !transfer_ && connected_.load(std::memory_order_acquire) && channel_.connected()) {
            if (const auto arena = channel_.take_capacity_wake();
                arena && last_ && last_->submitted.selection_generation == selection &&
                active_ && *arena == last_->reuse_arena && active_->id == *arena && channel_.claimable(*arena)) {
                pending_ = *last_;
                pending_->requested_owner = 0U;
                if (diagnostics_.valid()) pending_->link = services::DiagnosticSpanIds::Next();
            }
        }
        PresentationNativeOutcome result;
        if (transfer_ && stage_ == Stage::ReadyComplete && connected_.load(std::memory_order_acquire)) result = Publish();
        if (!transfer_ && pending_) result = Prepare(selection);
        result.capability = result.progress == PresentationNativeProgress::Published ? result.publication.capability : Capability();
        return result;
    }
    int poll_fd() const noexcept override { return channel_.poll_fd(); }
    int completion_fd() const noexcept override { return wake_->get(); }
    bool wants_write() const noexcept override { return channel_.wants_write(); }
    void SetApplicationPeerConnected(bool connected) noexcept override { connected_.store(connected, std::memory_order_release); }
    void SetExpectedBrowserProcessGroup(pid_t group) override { channel_.set_expected_process_group(group); }
    Retirement BrowserPeerLost() noexcept override {
        browser_terminal_ = true;
        static_cast<void>(channel_.consume_peer_loss());
        channel_.reset_peer();
        if (!SettlePhysicalWork()) return {.all_released = false, .safe_to_destroy = false};
        bool released = true;
        try {
            context_.Bind();
            for (auto& source : sources_) {
                const bool source_released = !source->timeline || source->timeline->Release() == cudaSuccess;
                released = released && source_released;
                DiagnoseSource(VisualDiagnosticOperation::PresentationSourceRetirement, *source, source_released ? 1U : 0U);
            }
            sources_.clear();
            for (const auto* arena : {active_.get(), candidate_.get(), retiring_.get()})
                if (arena) DiagnoseArena(VisualDiagnosticOperation::PresentationRetirement, *arena, released ? 1U : 0U);
            active_.reset(); candidate_.reset(); retiring_.reset(); pending_.reset(); last_.reset();
        } catch (...) { released = false; }
        return {.all_released = released, .safe_to_destroy = true};
    }

   private:
    struct Pending final {
        PresentationSubmittedSource submitted{};
        const VisualSourceReader* reader = nullptr;
        contracts::DiagnosticLink link{};
        std::uint64_t requested_owner = 0U;
        std::uint64_t reuse_publication = 0U;
        native::WorkspaceSurfaceImportId reuse_arena{};
    };
    enum class Stage : std::uint8_t { Idle, ReadyPending, ReadyComplete, ReleasePending, ReleaseComplete };
    struct Transfer final {
        AdmittedSource* source;
        Pending pending;
        std::uint64_t ready;
        std::uint64_t publication;
        gpu::ImagePlaneView plane;
    };
    // Callback storage is stable until stream settlement, including callback
    // return. Notifications alone never discharge that destruction obligation.
    struct Completion final {
        int fd = -1;
        std::atomic_bool ready{false};
        cudaError_t status = cudaSuccess;
    };
    void EnqueueCompletion() {
        callback_pending_ = true;
        if (cudaStreamAddCallback(Stream(), [](cudaStream_t, cudaError_t status, void* pointer) {
                auto& signal = *static_cast<Completion*>(pointer);
                signal.status = status;
                signal.ready.store(true, std::memory_order_release);
                static_cast<void>(mmltk::common::io::signal_event_fd(signal.fd));
            }, &completion_, 0U) != cudaSuccess)
            throw std::runtime_error("presentation completion submission failed");
    }
    cudaStream_t Stream() const noexcept { return reinterpret_cast<cudaStream_t>(stream_.native_handle()); }
    void ReleaseRead() noexcept {
        if (source_read_) source_read_->Complete();
        source_read_.reset();
    }
    bool SettlePhysicalWork() noexcept {
        // An exported ready edge can already authorize a Vulkan read. A failed
        // CUDA wait submission cannot turn local stream idleness into proof of
        // that external read's completion; retain this complete owner instead.
        if (browser_read_exposed_ && !release_wait_submitted_) return false;
        if (!callback_pending_ && !source_read_) return true;
        if (browser_terminal_ && stage_ == Stage::ReleasePending) {
            try { context_.Bind(); } catch (...) { return false; }
            if (cudaStreamQuery(Stream()) != cudaSuccess) return false;
        }
        const auto settled = stream_.Settle();
        if (!settled.completion_reached) {
            if (source_read_) source_read_->Quarantine();
            return false;
        }
        ReleaseRead();
        browser_read_exposed_ = false;
        release_wait_submitted_ = false;
        if (ready_span_) ready_span_->FinishWith([](auto& fact) { fact.context.outcome = 1U; });
        if (release_span_) release_span_->FinishWith([](auto& fact) { fact.context.outcome = 1U; });
        ready_span_.reset();
        release_span_.reset();
        callback_pending_ = false;
        transfer_.reset();
        stage_ = Stage::Idle;
        return true;
    }
    void DrainCompletion() {
        std::uint64_t edge = 0U;
        ssize_t read;
        do { read = ::read(wake_->get(), &edge, sizeof(edge)); } while (read < 0 && errno == EINTR);
        if (read < 0 && errno != EAGAIN) throw std::runtime_error("presentation completion wake failed");
        if (stage_ == Stage::ReleasePending && transfer_ && configuration_.completion_acceptance &&
            completion_.ready.load(std::memory_order_acquire) &&
            configuration_.completion_acceptance->Hold({transfer_->source->description.id_high, transfer_->source->description.id_low,
                (transfer_->ready + 1U) / 2U, transfer_->publication})) return;
        if (completion_.ready.exchange(false, std::memory_order_acq_rel)) {
            if (completion_.status != cudaSuccess) throw std::runtime_error("presentation asynchronous graphics work failed");
            if (stage_ == Stage::ReadyPending) {
                stage_ = Stage::ReadyComplete;
                if (ready_span_) ready_span_->FinishWith([](auto& fact) { fact.context.outcome = 1U; });
                ready_span_.reset();
                ReportPixels();
            } else if (stage_ == Stage::ReleasePending) {
                stage_ = Stage::ReleaseComplete;
                // Includes optional probe reads and the browser's actual source
                // read. Neither the ready callback nor probe success releases it.
                ReleaseRead();
                browser_read_exposed_ = false;
                release_wait_submitted_ = false;
                context_.Bind();
            }
        }
        if (stage_ == Stage::ReleaseComplete && transfer_) {
            const auto& frame = transfer_->pending.submitted.observation.frame;
            if (!channel_.copy_completed(transfer_->source->id(),
                    {presentation_source_session(frame.source.kind), frame.revision}, transfer_->publication, (transfer_->ready + 1U) / 2U)) return;
            if (release_span_) release_span_->FinishWith([](auto& fact) { fact.context.outcome = 1U; });
            release_span_.reset();
            transfer_.reset();
            stage_ = Stage::Idle;
        }
    }
    static PresentationCapability CapabilityOf(const SampleArena& arena) noexcept {
        return {.surface_high = arena.id.high, .surface_low = arena.id.low, .extent = arena.extent,
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
    SampleArena* EnsureArena(VisualExtent extent) {
        if (candidate_) {
            if (Contains(*candidate_, extent)) return candidate_.get();
            if (candidate_->ready && !retiring_) {
                const auto withdrawal = channel_.withdraw(candidate_->id);
                if (withdrawal.progress == native::WorkspaceSurfaceWithdrawalProgress::Invalid ||
                    withdrawal.progress == native::WorkspaceSurfaceWithdrawalProgress::Capacity)
                    throw std::runtime_error("Firefox candidate arena withdrawal failed");
                DiagnoseArena(VisualDiagnosticOperation::PresentationCandidateWithdrawal, *candidate_, 1U);
                retiring_ = std::move(candidate_);
                RetireSources();
            }
            return nullptr;
        }
        if (active_ && Contains(*active_, extent)) return active_.get();
        if (retiring_) return nullptr;
        auto arena = std::make_unique<SampleArena>();
        arena->id = native::WorkspaceSurfaceImportId::generate();
        arena->generation = mmltk::common::types::take_monotonic_identity(next_generation_);
        arena->submitted = pending_->submitted;
        arena->extent = {active_ ? std::max(active_->extent.width, extent.width) : extent.width,
                         active_ ? std::max(active_->extent.height, extent.height) : extent.height};
        if (pending_supersession_acceptance_ && active_ &&
            pending_->submitted.observation.frame.source.kind == PresentationSourceKind::Upscale) {
            pending_supersession_acceptance_ = false;
            arena->extent = active_->extent;
        }
        DiagnoseArena(VisualDiagnosticOperation::PresentationArenaAdvertised, *arena, 1U);
        if (!channel_.admit_arena(arena->id, arena->generation, arena->extent.width, arena->extent.height,
                                  arena->submitted.selection_generation, arena->submitted.observation.frame.revision)) return nullptr;
        candidate_ = std::move(arena);
        return candidate_.get();
    }
    gpu::ImageWorkspaceLayout Layout(const SampleArena& arena) const {
        const auto& packet = arena.layout;
        gpu::ImageWorkspaceLayout layout{.device_incarnation = packet.device_incarnation, .device = settings_.device,
            .width = packet.width, .height = packet.height, .pitch_bytes = packet.stride, .offset_bytes = packet.offset,
            .required_allocation_bytes = std::max<std::uint64_t>(packet.size, configuration_.minimum_allocation_bytes), .alignment_bytes = packet.alignment, .dedicated = packet.dedicated != 0U};
        std::ranges::copy(packet.device_uuid, layout.device_uuid.begin());
        if (!layout.valid()) throw std::runtime_error("Firefox returned an invalid workspace layout");
        return layout;
    }
    void Request(const VisualSourceReader& reader, const gpu::ImageWorkspaceObservation& observed,
                 const SampleArena& arena, std::uint64_t admitted = 0U) {
        reader.request_workspace({.product_owner = observed.product_owner, .product_revision = observed.product_revision,
            .layout = Layout(arena), .display_execution = execution_, .admitted_allocation = admitted,
            .ready = [weak = std::weak_ptr{wake_}] {
                if (const auto wake = weak.lock()) static_cast<void>(mmltk::common::io::signal_event_fd(wake->get()));
            }});
    }
    void Accept(native::WorkspaceSurfaceImportOutcome outcome) {
        if (candidate_ && candidate_->id == outcome.id) {
            DiagnoseArena(VisualDiagnosticOperation::PresentationImportOutcome, *candidate_, outcome.imported ? 1U : 0U);
            if (!outcome.imported) throw std::runtime_error("Firefox arena admission failed");
            if (outcome.layout.opcode != abi::Opcode::ArenaReady || outcome.layout.width != candidate_->extent.width ||
                outcome.layout.height != candidate_->extent.height) throw std::runtime_error("Firefox arena layout identity mismatch");
            candidate_->layout = outcome.layout;
            static_cast<void>(Layout(*candidate_));
            candidate_->ready = true;
            return;
        }
        for (auto& source : sources_) {
            if (source->id() != outcome.id) continue;
            if (!outcome.imported) throw std::runtime_error("Firefox source admission failed");
            if (source->withdrawing) return;
            if (outcome.timeline_descriptor.get() < 0) throw std::runtime_error("Firefox source timeline is unavailable");
            source->timeline.emplace(std::move(outcome.timeline_descriptor));
            DiagnoseSource(VisualDiagnosticOperation::PresentationSourceReady, *source, 1U);
            const auto* arena = candidate_ && candidate_->id == source->arena() ? candidate_.get() : active_.get();
            if (!arena || arena->id != source->arena()) throw std::runtime_error("Firefox source belongs to a retired arena");
            Request(*source->reader, source->product, *arena, source->workspace->identity());
            return;
        }
        throw std::runtime_error("Firefox returned an unknown source admission");
    }
    void Withdraw(AdmittedSource& source) {
        if (source.withdrawing || (transfer_ && transfer_->source == &source)) return;
        const auto withdrawal = channel_.withdraw(source.id());
        if (withdrawal.progress == native::WorkspaceSurfaceWithdrawalProgress::Invalid ||
            withdrawal.progress == native::WorkspaceSurfaceWithdrawalProgress::Capacity)
            throw std::runtime_error("Firefox source withdrawal failed");
        source.withdrawing = true;
        DiagnoseSource(VisualDiagnosticOperation::PresentationSourceWithdrawal, source, 1U);
    }
    void RetireSources() {
        for (auto& source : sources_)
            if (source->workspace->retired() || (retiring_ && source->arena() == retiring_->id)) Withdraw(*source);
    }
    PresentationNativeOutcome Supersede() {
        const auto submitted = pending_->submitted;
        const bool retry = pending_->reuse_publication != 0U;
        pending_.reset();
        return retry ? PresentationNativeOutcome{} :
                       PresentationNativeOutcome{.progress = PresentationNativeProgress::Superseded, .submitted = submitted};
    }
    PresentationNativeOutcome Prepare(std::uint64_t selection) {
        if (!connected_.load(std::memory_order_acquire) || !channel_.connected()) return {};
        if (pending_->submitted.selection_generation != selection) return Supersede();
        const auto& frame = pending_->submitted.observation.frame;
        // A retry retains the publication's actual arena even if an unrelated
        // candidate was prepared while a newer submission was pending.
        auto* arena = pending_->reuse_publication != 0U ? active_.get() : EnsureArena(frame.extent);
        if (pending_->reuse_publication != 0U &&
            (!arena || arena->id != pending_->reuse_arena || !channel_.claimable(arena->id))) return Supersede();
        if (!arena || !arena->ready) return {};
        const auto observed = pending_->reader->observe_workspace();
        if (observed.product_owner == 0U) return {};
        if (observed.product_revision != frame.revision) return Supersede();
        if (!observed.workspace || observed.workspace->layout() != Layout(*arena)) {
            if (pending_->requested_owner != observed.product_owner) {
                Request(*pending_->reader, observed, *arena);
                pending_->requested_owner = observed.product_owner;
            }
            return {};
        }
        AdmittedSource* admitted = nullptr;
        for (auto& source : sources_)
            if (!source->withdrawing && source->workspace->identity() == observed.workspace->identity() && source->arena() == arena->id)
                admitted = source.get();
        if (!admitted) {
            // A workspace admits one initialized Vulkan source image. A fresh
            // browser/arena must not repeat UNDEFINED ownership setup over an
            // already published raw alias, even when its layout is identical.
            if (observed.workspace->admitted()) {
                Request(*pending_->reader, observed, *arena);
                return {};
            }
            if (sources_.size() == kSourceCapacity) return {};
            const auto id = native::WorkspaceSurfaceImportId::generate();
            auto packet = arena->layout;
            packet.opcode = abi::Opcode::Import;
            packet.descriptors = abi::kImportDescriptorCount;
            packet.id_high = id.high; packet.id_low = id.low;
            packet.arena_high = arena->id.high; packet.arena_low = arena->id.low;
            packet.allocation_identity = observed.workspace->identity();
            packet.size = observed.workspace->allocation_bytes();
            auto source = std::make_unique<AdmittedSource>(packet);
            source->workspace = observed.workspace;
            source->generation = mmltk::common::types::take_monotonic_identity(next_generation_);
            source->reader = pending_->reader;
            source->product = observed;
            source->edge.reset(::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK));
            if (source->edge.get() < 0) throw std::runtime_error("native source eventfd creation failed");
            source->signal = native::WorkspaceSurfaceFrameSignal::create();
            if (!channel_.admit_source(source->description, source->generation, source->workspace->ExportDescriptor(),
                                        source->edge.get(), source->signal.descriptor(), pending_->submitted.selection_generation, frame.revision)) return {};
            sources_.push_back(std::move(source));
            return {};
        }
        if (!admitted->timeline) return {};
        if (!admitted->workspace->admitted()) {
            Request(*pending_->reader, observed, *arena, admitted->workspace->identity());
            return {};
        }
        services::RuntimeDiagnosticSpan borrow_span(diagnostics_, [&] {
            auto fact = presentation_diagnostic_fact(VisualDiagnosticOperation::PresentationSourceBorrowStarted,
                {.submitted = pending_->submitted, .publication = {.capability = CapabilityOf(*arena)}, .link = pending_->link},
                settings_.device);
            fact.context.workspace = native::workspace_source_diagnostic(admitted->description);
            return visual_diagnostic_boundary(fact, VisualDiagnosticOperation::PresentationSourceBorrowCompleted);
        }, pending_->link);
        auto read = pending_->reader->borrow_workspace();
        if (!read.valid()) {
            Request(*pending_->reader, observed, *arena, admitted->workspace->identity());
            return {};
        }
        if (read.identity() != admitted->workspace->identity() || read.revision() != frame.revision) return Supersede();
        borrow_span.FinishWith([](auto& fact) { fact.context.outcome = 1U; });
        stream_.Await(read);
        const auto transfer = mmltk::common::types::take_monotonic_identity(admitted->next_transfer);
        transfer_.emplace(Transfer{admitted, *pending_, native::detail::workspace_timeline_ready(transfer),
            pending_->reuse_publication != 0U ? pending_->reuse_publication :
                mmltk::common::types::take_monotonic_identity(next_publication_), read.plane()});
        source_read_ = std::move(read).TakeCompletion();
        diagnostics_.Emit([&] {
            auto fact = Fact(VisualDiagnosticOperation::PresentationSourceReadSubmitted, *transfer_, 0U);
            fact.value = 0U;
            fact.context.publication = {};
            fact.context.transfer = {};
            return fact;
        });
        try {
            SamplePixels();
            ready_span_.emplace(diagnostics_, [&] {
                return visual_diagnostic_boundary(Fact(VisualDiagnosticOperation::PresentationReadySyncStarted, *transfer_, 0U),
                                                  VisualDiagnosticOperation::PresentationReadySyncCompleted);
            }, transfer_->pending.link);
            admitted->timeline->SignalReady(Stream(), transfer_->ready);
            stage_ = Stage::ReadyPending;
            EnqueueCompletion();
            pending_.reset();
        } catch (...) {
            static_cast<void>(SettlePhysicalWork());
            throw;
        }
        return {};
    }
    PresentationNativeOutcome Publish() {
        auto& transfer = *transfer_;
        auto& source = *transfer.source;
        const auto& frame = transfer.pending.submitted.observation.frame;
        // This offer uses capacity reported before its publication. Any later
        // release-only attempt owns a fresh Available, retained through completion.
        static_cast<void>(channel_.take_capacity_wake());
        native::detail::publish_workspace_frame_signal(source.signal.mapping(), transfer.ready, (transfer.ready + 1U) / 2U,
            native::WorkspacePresentationLayer::Primary, {presentation_source_session(frame.source.kind), frame.revision},
            transfer.publication, frame.extent.width, frame.extent.height);
        browser_read_exposed_ = true;
        if (!mmltk::common::io::signal_event_fd(source.edge.get())) throw std::runtime_error("native source frame publication failed");
        Diagnose(VisualDiagnosticOperation::PresentationFrameEdge, transfer, transfer.ready);
        release_span_.emplace(diagnostics_, [&] {
            return visual_diagnostic_boundary(Fact(VisualDiagnosticOperation::PresentationReleaseWaitStarted, transfer, 0U),
                                              VisualDiagnosticOperation::PresentationReleaseWaitCompleted);
        }, transfer.pending.link);
        source.timeline->WaitForRelease(Stream(), transfer.ready + 1U);
        release_wait_submitted_ = true;
        stage_ = Stage::ReleasePending;
        EnqueueCompletion();
        if (candidate_ && source.arena() == candidate_->id) {
            if (active_) {
                const auto withdrawal = channel_.withdraw(active_->id);
                if (withdrawal.progress == native::WorkspaceSurfaceWithdrawalProgress::Invalid)
                    throw std::runtime_error("Firefox sample arena withdrawal failed");
                DiagnoseArena(VisualDiagnosticOperation::PresentationActiveWithdrawal, *active_, 1U);
                retiring_ = std::move(active_);
                RetireSources();
            }
            active_ = std::move(candidate_);
            DiagnoseArena(VisualDiagnosticOperation::PresentationReplacement, *active_, 1U);
        }
        PresentationNativeOutcome outcome{.progress = PresentationNativeProgress::Published,
            .submitted = transfer.pending.submitted,
            .publication = {.capability = CapabilityOf(*active_), .timeline_ready = transfer.ready,
                            .presentation_revision = transfer.publication, .transfer_sequence = (transfer.ready + 1U) / 2U},
            .diagnostic_link = transfer.pending.link};
        last_ = transfer.pending;
        last_->reuse_publication = transfer.publication;
        last_->reuse_arena = source.arena();
        if (transfer.pending.reuse_publication != 0U) return {};
        return outcome;
    }
    void SamplePixels() noexcept {
        if (!diagnostics_.pixel_probes_enabled() || probe_failed_) return;
        try {
            if (!pixels_) {
                pixels_ = gpu::PinnedHostBuffer::ForCurrentDevice();
                pixels_->ensure_bytes(25U * sizeof(std::uint32_t));
                void* memory = nullptr;
                if (cudaMalloc(&memory, 25U * sizeof(std::uint32_t)) != cudaSuccess) throw std::runtime_error("probe allocation failed");
                pixel_device_.reset(static_cast<std::uint32_t*>(memory));
            }
            const auto plane = transfer_->plane;
            std::array<std::uint32_t, 50U> coordinates{};
            const auto coordinate = [](std::size_t index, std::uint32_t size) {
                const std::array points{0U, std::min(191U, size - 1U), std::min(383U, size - 1U), (size - 1U) / 2U, size - 1U};
                return points[index];
            };
            for (std::size_t index = 0U; index != 25U; ++index) {
                coordinates[2U * index] = coordinate(index % 5U, plane.descriptor.width);
                coordinates[2U * index + 1U] = coordinate(index / 5U, plane.descriptor.height);
            }
            pixel_coordinates_ = coordinates;
            services::RuntimeDiagnosticSpan probe_span(diagnostics_, [&] {
                return visual_diagnostic_boundary(Fact(VisualDiagnosticOperation::PresentationPixelProbeStarted, *transfer_, 0U),
                                                  VisualDiagnosticOperation::PresentationPixelProbeSubmitted);
            }, transfer_->pending.link);
            if (mmltk::backend::imaging::raster::probe_rgba(
                    {reinterpret_cast<const std::uint8_t*>(plane.data), plane.descriptor.pitch_bytes,
                     static_cast<int>(plane.descriptor.width), static_cast<int>(plane.descriptor.height)},
                    pixel_device_.get(), coordinates, stream_.native_handle()) != cudaSuccess ||
                cudaMemcpyAsync(pixels_->data(), pixel_device_.get(), 25U * sizeof(std::uint32_t), cudaMemcpyDeviceToHost, Stream()) != cudaSuccess)
                throw std::runtime_error("probe submission failed");
            probe_pending_ = true;
            probe_span.FinishWith([](auto& fact) { fact.context.outcome = 1U; });
        } catch (...) { probe_failed_ = true; }
    }
    void ReportPixels() {
        if (!probe_pending_) return;
        probe_pending_ = false;
        if (!diagnostics_.pixel_probes_enabled()) return;
        const auto* values = static_cast<const std::uint32_t*>(pixels_->data());
        std::array<VisualDiagnosticFact, 25U> facts;
        for (std::size_t index = 0U; index != facts.size(); ++index) {
            facts[index] = Fact(VisualDiagnosticOperation::PresentationPixel, *transfer_, 0U);
            facts[index].context.pixel = {.sample_index = static_cast<std::uint32_t>(index),
                .sample_x = pixel_coordinates_[2U * index], .sample_y = pixel_coordinates_[2U * index + 1U], .sample_rgba = values[index]};
        }
        diagnostics_.WriteBatch(facts);
    }
    VisualDiagnosticFact Fact(VisualDiagnosticOperation operation, const Transfer& transfer, std::uint64_t outcome) const noexcept {
        const auto* arena = candidate_ && candidate_->id == transfer.source->arena() ? candidate_.get() : active_.get();
        auto fact = presentation_diagnostic_fact(operation,
            {.submitted = transfer.pending.submitted,
             .publication = {.capability = arena ? CapabilityOf(*arena) : PresentationCapability{}, .timeline_ready = transfer.ready,
                             .presentation_revision = transfer.publication, .transfer_sequence = (transfer.ready + 1U) / 2U},
             .link = transfer.pending.link}, settings_.device, outcome);
        fact.context.workspace = native::workspace_source_diagnostic(transfer.source->description);
        return fact;
    }
    void DiagnoseSource(VisualDiagnosticOperation operation, const AdmittedSource& source, std::uint64_t outcome) const noexcept {
        diagnostics_.Emit([&] {
            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Presentation, .operation = operation,
                .device = settings_.device, .generation = source.generation,
                .context = {.capacity_width = source.description.width,
                            .capacity_height = source.description.height,
                            .surface_high = source.description.id_high, .surface_low = source.description.id_low, .outcome = outcome,
                            .workspace = native::workspace_source_diagnostic(source.description)}};
        });
    }
    void DiagnoseArena(VisualDiagnosticOperation operation, const SampleArena& arena, std::uint64_t outcome) const noexcept {
        diagnostics_.Emit([&] {
            auto fact = presentation_diagnostic_fact(operation,
                {.submitted = arena.submitted, .publication = {.capability = CapabilityOf(arena)}}, settings_.device, outcome);
            if (operation == VisualDiagnosticOperation::PresentationImportOutcome) fact.value = outcome;
            return fact;
        });
    }
    void Diagnose(VisualDiagnosticOperation operation, const Transfer& transfer, std::uint64_t outcome) const noexcept {
        if (diagnostics_.valid()) diagnostics_(Fact(operation, transfer, outcome));
    }
    static constexpr std::size_t kSourceCapacity = 24U;
    VisualDeviceSettings settings_;
    PresentationNativeConfiguration configuration_;
    bool pending_supersession_acceptance_ = configuration_.pending_supersession_acceptance;
    bool browser_read_exposed_ = false;
    bool release_wait_submitted_ = false;
    VisualDiagnosticSink diagnostics_;
    std::optional<services::RuntimeDiagnosticSpan<VisualDiagnosticSink, VisualDiagnosticFact>> ready_span_;
    std::optional<services::RuntimeDiagnosticSpan<VisualDiagnosticSink, VisualDiagnosticFact>> release_span_;
    gpu::DeviceExecution execution_;
    native::WorkspaceSurfaceImportChannel channel_;
    gpu::DeviceContext context_;
    gpu::ImageStream stream_;
    std::shared_ptr<ScopedFd> wake_;
    Completion completion_;
    std::vector<std::unique_ptr<AdmittedSource>> sources_;
    std::unique_ptr<SampleArena> active_, candidate_, retiring_;
    std::optional<Pending> pending_, last_;
    std::optional<Transfer> transfer_;
    std::unique_ptr<gpu::ImageProductReadCompletion> source_read_;
    std::unique_ptr<gpu::PinnedHostBuffer> pixels_;
    std::unique_ptr<std::uint32_t, PixelDeviceDeleter> pixel_device_;
    std::array<std::uint32_t, 50U> pixel_coordinates_{};
    std::uint64_t next_generation_ = 1U, next_publication_ = 1U;
    Stage stage_ = Stage::Idle;
    std::atomic_bool connected_{true};
    bool callback_pending_ = false, browser_terminal_ = false, probe_failed_ = false, probe_pending_ = false;
};
}  // namespace

PresentationNativeWriterFactory make_native_presentation_writer_factory(VisualDeviceSettings settings,
        PresentationNativeConfiguration configuration, VisualDiagnosticSink diagnostics) {
    if (!settings.valid() || configuration.import_socket.empty()) throw contracts::InvalidIntentError("Presentation configuration is invalid");
    return [settings, configuration = std::move(configuration), diagnostics, execution = resolve_visual_device_execution(settings)] {
        return std::make_unique<NativePresentationWriter>(settings, configuration, diagnostics, execution);
    };
}
}  // namespace mmltk::controller
