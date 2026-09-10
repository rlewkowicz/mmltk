#include "src/controller/presentation/presentation_system.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <utility>

#include "src/common/io/scoped_fd.h"
#include "src/common/io/event_fd.h"
#include "src/controller/presentation/detail/workspace_frame_signal.h"
#include "src/common/types/generation.h"
#include "src/controller/presentation/detail/workspace_surface_import_channel.h"
#include "src/frameworks/gpu/exported_image_buffer.h"
#include "src/frameworks/gpu/external_graphics_timeline.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"

import mmltk.backend.imaging.raster;

namespace mmltk::controller {
namespace {

namespace gpu = mmltk::frameworks::gpu;
namespace native = mmltk::controller::presentation;
namespace raster = mmltk::backend::imaging::raster;

struct PixelDeviceDeleter final {
    void operator()(std::uint32_t* pointer) const noexcept {
        if (pointer != nullptr) static_cast<void>(cudaFree(pointer));
    }
};

struct NativeAllocation final {
    struct LatestFrame final {
        PresentationSubmittedSource submitted{};
        std::uint64_t presentation_revision = 0U;
        std::uint64_t transfer_sequence = 0U;
        contracts::DiagnosticLink diagnostic_link{};
    };

    std::unique_ptr<gpu::ExportedImageBuffer> buffer;
    native::WorkspaceSurfaceImportId import_id{};
    std::uint64_t generation = 0U;
    PresentationSubmittedSource submitted{};
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::size_t pitch = 0U;
    std::optional<gpu::ExternalGraphicsTimeline> timeline;
    mmltk::common::io::ScopedFd frame_edge;
    native::WorkspaceSurfaceFrameSignal frame_signal;
    std::optional<LatestFrame> latest;
    std::uint64_t next_transfer_sequence = 1U;
};

class NativePresentationWriter final : public PresentationNativeWriter {
   public:
    NativePresentationWriter(const VisualDeviceSettings settings, PresentationNativeConfiguration configuration,
                             const VisualDiagnosticSink diagnostics, const gpu::DeviceExecution& execution)
        : settings_(settings),
          configuration_(std::move(configuration)),
          diagnostics_(diagnostics),
          channel_(configuration_.import_socket, diagnostics_),
          context_(settings.device, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, settings.numa_node, execution),
          stream_(context_) {
        completion_fd_.reset(::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK));
        if (completion_fd_.get() < 0) throw std::runtime_error("presentation completion eventfd creation failed");
        completion_.fd = copy_completion_.fd = completion_fd_.get();
    }

    ~NativePresentationWriter() override {
        // Unsettled CUDA work must never outlive callback storage or release
        // a borrowed producer into concurrent reuse during destruction.
        if (!SettlePhysicalWork()) std::terminate();
        if (pixel_samples_) {
            try {
                context_.Bind();
                pixel_device_.reset();
                static_cast<void>(pixel_samples_->ReleaseSettled());
            } catch (...) { std::terminate(); }
        }
    }

    void Submit(const PresentationSubmittedSource submitted, const VisualSourceReader& source) override {
        context_.Bind();
        if (!submitted.valid() || source.source != submitted.observation.frame.source || !source.borrow || pending_frame_)
            throw std::invalid_argument("presentation native submission is invalid");
        pending_frame_ = PendingFrame{
            .submitted = submitted,
            .reader = std::addressof(source),
            .diagnostic_link = diagnostics_.valid() ? services::DiagnosticSpanIds::Next() : contracts::DiagnosticLink{},
        };
        EnsureTargetForPending();
    }

    PresentationNativeOutcome Pump(const std::uint64_t current_selection_generation) override {
        context_.Bind();
        channel_.pump();
        if (const auto error = channel_.terminal_error()) throw std::runtime_error(*error);
        while (auto retired = channel_.take_retirement()) {
            if (retiring_ && retiring_->import_id == retired->id && retiring_->generation == retired->generation &&
                !ReleaseAllocation(retiring_))
                throw std::runtime_error("presentation retiring allocation release failed");
        }
        while (auto outcome = channel_.take_outcome())
            AcceptImport(std::move(*outcome));
        const auto capacity_wake = channel_.take_capacity_wake();
        DrainCompletion();
        EnsureTargetForPending();
        auto result = CompleteTransfer();
        if (result.progress != PresentationNativeProgress::Published) result = TryIssue(current_selection_generation);
        if (result.progress != PresentationNativeProgress::Published && capacity_wake && active_ && active_->import_id == *capacity_wake &&
            active_->latest)
            reoffer_pending_ = true;
        if (application_peer_connected_.load(std::memory_order_acquire) && reoffer_pending_ && !pending_frame_ && !transfer_target_ &&
            !release_wait_pending_ && active_ && active_->latest && channel_.claimable(active_->import_id)) {
            reoffer_pending_ = false;
            static_cast<void>(OfferLatest(*active_, reinterpret_cast<cudaStream_t>(stream_.native_handle())));
        }
        result.capability = result.progress == PresentationNativeProgress::Published ? result.publication.capability : CurrentCapability();
        return result;
    }

    int poll_fd() const noexcept override { return channel_.poll_fd(); }
    int completion_fd() const noexcept override { return completion_fd_.get(); }

    bool wants_write() const noexcept override { return channel_.wants_write(); }

    void SetApplicationPeerConnected(const bool connected) noexcept override {
        application_peer_connected_.store(connected, std::memory_order_release);
    }

    void SetExpectedBrowserProcessGroup(const pid_t process_group) override { channel_.set_expected_process_group(process_group); }

    Retirement BrowserPeerLost() noexcept override {
        if (ready_span_) ready_span_->Finish(contracts::DiagnosticSpanOutcome::Exception);
        if (release_span_) release_span_->Finish(contracts::DiagnosticSpanOutcome::Exception);
        if (terminal_retired_) return {.all_released = terminal_release_succeeded_, .safe_to_destroy = !NeedsSettlement()};
        bool context_bound = true;
        try {
            context_.Bind();
        } catch (...) { context_bound = false; }
        static_cast<void>(channel_.consume_peer_loss());
        channel_.reset_peer();
        browser_terminal_ = true;
        terminal_release_succeeded_ = RetirePhysical() && context_bound;
        return {.all_released = terminal_release_succeeded_, .safe_to_destroy = !NeedsSettlement()};
    }

   private:
    struct PendingFrame final {
        PresentationSubmittedSource submitted{};
        const VisualSourceReader* reader = nullptr;
        contracts::DiagnosticLink diagnostic_link{};
    };

    // Stable callback storage outlives every submitted operation. CUDA callbacks
    // only publish completion and wake the owning worker; it releases custody.
    struct Completion final {
        int fd = -1;
        std::atomic<bool> ready{false};
        cudaError_t status = cudaSuccess;
    };

    [[nodiscard]] bool EnqueueCompletion(cudaStream_t stream, Completion& signal) noexcept {
        // Notification consumption does not prove callback return: the callback
        // still accesses its record and descriptor after publishing ready.
        // Only stream settlement discharges this obligation, including when
        // callback submission reports a failure from earlier asynchronous work.
        completion_settlement_pending_ = true;
        // Unlike a host function, this callback also wakes on device failure.
        return cudaStreamAddCallback(
                   stream,
                   [](cudaStream_t, const cudaError_t status, void* context) {
                       auto& completion = *static_cast<Completion*>(context);
                       completion.status = status;
                       completion.ready.store(true, std::memory_order_release);
                       static_cast<void>(mmltk::common::io::signal_event_fd(completion.fd));
                   },
                   std::addressof(signal), 0U) == cudaSuccess;
    }

    void DrainCompletion() {
        std::uint64_t wake = 0U;
        ssize_t consumed = -1;
        do {
            consumed = ::read(completion_fd_.get(), &wake, sizeof(wake));
        } while (consumed < 0 && errno == EINTR);
        if (consumed < 0 && errno != EAGAIN) throw std::runtime_error("presentation completion wake failed");
        if (copy_completion_.ready.exchange(false, std::memory_order_acq_rel)) {
            if (copy_completion_.status != cudaSuccess) throw std::runtime_error("presentation asynchronous source copy failed");
            ReleaseSourceRead();
            context_.Bind();
        }
        if (!completion_.ready.exchange(false, std::memory_order_acq_rel)) return;
        if (completion_.status != cudaSuccess) throw std::runtime_error("presentation asynchronous graphics work failed");
        if (transfer_target_) {
            transfer_complete_ = true;
        } else if (release_wait_pending_) {
            release_wait_pending_ = false;
            if (release_span_) {
                release_span_->FinishWith([](auto& fact) { fact.detail = fact.context.outcome = 1U; });
                release_span_.reset();
            }
        }
    }

    [[nodiscard]] bool NeedsSettlement() const noexcept {
        return source_read_ || source_completion_ || pixel_work_pending_ || transfer_target_ || release_wait_pending_ ||
               completion_settlement_pending_;
    }

    [[nodiscard]] bool SettlePhysicalWork() noexcept {
        if (!NeedsSettlement()) return true;
        // A dead browser cannot be asked to advance its semaphore. Retain this
        // writer (including callback storage) if that external wait is unsettled.
        if (browser_terminal_ && release_wait_pending_) {
            try {
                context_.Bind();
            } catch (...) { return false; }
            if (cudaStreamQuery(reinterpret_cast<cudaStream_t>(stream_.native_handle())) != cudaSuccess) return false;
        }
        const auto settled = stream_.Settle();
        if (!settled.completion_reached) {
            if (source_read_) source_read_->Quarantine();
            if (source_completion_) source_completion_->Quarantine();
            return false;
        }
        ReleaseSourceRead();
        pixel_work_pending_ = false;
        transfer_target_ = nullptr;
        transfer_copy_completed_ = false;
        release_wait_pending_ = false;
        completion_settlement_pending_ = false;
        return true;
    }

    void ReleaseSourceRead() noexcept {
        if (source_completion_) source_completion_->Complete();
        source_completion_.reset();
        source_read_.reset();
    }

    [[nodiscard]] bool ReleaseAllocation(std::unique_ptr<NativeAllocation>& allocation) noexcept {
        if (!allocation) return true;
        bool released = true;
        if (allocation->timeline && allocation->timeline->Release() != cudaSuccess) released = false;
        allocation->timeline.reset();
        if (allocation->buffer && allocation->buffer->Release() != cudaSuccess) released = false;
        DiagnoseAllocation(VisualDiagnosticOperation::PresentationRetirement, *allocation, released ? 1U : 0U);
        allocation.reset();
        return released;
    }

    [[nodiscard]] bool RetirePhysical() noexcept {
        if (!browser_terminal_ || terminal_retired_) return false;
        if (!SettlePhysicalWork()) return false;
        // Releasing terminal source storage can destroy its context on this
        // thread. Restore the receiver before retiring exported resources.
        try {
            context_.Bind();
        } catch (...) { return false; }
        terminal_retired_ = true;
        const bool candidate_released = ReleaseAllocation(candidate_);
        const bool retiring_released = ReleaseAllocation(retiring_);
        const bool active_released = ReleaseAllocation(active_);
        pending_frame_.reset();
        return candidate_released && retiring_released && active_released;
    }

    [[nodiscard]] std::size_t AlignedPitch(const std::uint32_t width) const {
        const std::size_t row = static_cast<std::size_t>(width) * 4U;
        const std::size_t alignment = configuration_.pitch_alignment;
        if (alignment == 0U || row > std::numeric_limits<std::size_t>::max() - (alignment - 1U))
            throw std::overflow_error("presentation pitch is out of range");
        return ((row + alignment - 1U) / alignment) * alignment;
    }

    void BeginImport(const VisualExtent extent, std::size_t required_pitch = 0U, std::size_t required_size = 0U) {
        if (candidate_) return;
        const std::uint32_t width = active_ ? std::max(active_->width, extent.width) : extent.width;
        const std::uint32_t height = active_ ? std::max(active_->height, extent.height) : extent.height;
        const std::size_t pitch = required_pitch == 0U ? AlignedPitch(width) : required_pitch;
        if (pitch < static_cast<std::size_t>(width) * 4U) throw std::invalid_argument("presentation import pitch is too small");
        if (height > std::numeric_limits<std::size_t>::max() / pitch) throw std::overflow_error("presentation allocation is out of range");
        auto allocation = std::make_unique<gpu::ExportedImageBuffer>();
        std::string error;
        if (!allocation->allocate(settings_.device, width, height, pitch,
                                  std::max(std::max(configuration_.minimum_allocation_bytes, required_size), pitch * height), &error))
            throw std::runtime_error(error);
        mmltk::common::io::ScopedFd descriptor{allocation->export_descriptor(&error)};
        if (descriptor.get() < 0) throw std::runtime_error(error);
        const std::uint64_t generation = mmltk::common::types::take_monotonic_identity(next_generation_);
        const auto id = native::WorkspaceSurfaceImportId::generate();
        mmltk::common::io::ScopedFd frame_edge{::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK)};
        if (frame_edge.get() < 0) throw std::runtime_error("presentation frame eventfd creation failed");
        auto frame_signal = native::WorkspaceSurfaceFrameSignal::create();
        if (diagnostics_.valid())
            diagnostics_.Emit([&] {
                return VisualDiagnosticFact{
                    .system = contracts::DiagnosticOwner::Presentation,
                    .operation = VisualDiagnosticOperation::PresentationAllocationCreated,
                    .device = settings_.device,
                    .generation = generation,
                    .context = {.capacity_width = width,
                                .capacity_height = height,
                                .surface_high = id.high,
                                .surface_low = id.low,
                                .selection_generation = pending_frame_ ? pending_frame_->submitted.selection_generation : 0U,
                                .frame_revision = pending_frame_ ? pending_frame_->submitted.observation.frame.revision : 0U,
                                .condition = static_cast<std::uint64_t>(PresentationCapabilityCondition::Unavailable),
                                .outcome = 1U,
                                .source = pending_frame_ ? visual_diagnostic_source(pending_frame_->submitted.observation)
                                                         : contracts::DiagnosticSource{},
                                .allocation = {.allocation_generation = generation},
                                .link = pending_frame_ ? pending_frame_->diagnostic_link : contracts::DiagnosticLink{}}};
            });
        if (!channel_.admit(id, generation, width, height, pitch, allocation->allocation_size(), std::move(descriptor), frame_edge.get(),
                            frame_signal.descriptor(), pending_frame_ ? pending_frame_->submitted.selection_generation : 0U,
                            pending_frame_ ? pending_frame_->submitted.observation.frame.revision : 0U))
            throw std::runtime_error("Firefox surface import admission failed");
        candidate_ = std::make_unique<NativeAllocation>(NativeAllocation{
            .buffer = std::move(allocation),
            .import_id = id,
            .generation = generation,
            .submitted = pending_frame_ ? pending_frame_->submitted : PresentationSubmittedSource{},
            .width = width,
            .height = height,
            .pitch = pitch,
            .timeline = {},
            .frame_edge = std::move(frame_edge),
            .frame_signal = std::move(frame_signal),
            .latest = {},
        });
    }

    [[nodiscard]] static PresentationTargetCapacity CapacityOf(const NativeAllocation& allocation) noexcept {
        return {
            .width = allocation.width,
            .height = allocation.height,
            .pitch = allocation.pitch,
            .bytes = allocation.buffer ? allocation.buffer->allocation_size() : 0U,
        };
    }

    [[nodiscard]] static PresentationCapability CapabilityOf(const NativeAllocation& allocation) noexcept {
        return {
            .surface_high = allocation.import_id.high,
            .surface_low = allocation.import_id.low,
            .extent = {allocation.width, allocation.height},
            .generation = allocation.generation,
            .condition = allocation.timeline ? PresentationCapabilityCondition::Ready : PresentationCapabilityCondition::Admitted,
        };
    }

    [[nodiscard]] PresentationCapability CurrentCapability() const noexcept {
        const NativeAllocation* allocation = candidate_ && channel_.claimable(candidate_->import_id) ? candidate_.get() : active_.get();
        return allocation && channel_.claimable(allocation->import_id) ? CapabilityOf(*allocation) : PresentationCapability{};
    }

    [[nodiscard]] static bool Contains(const NativeAllocation* allocation, const VisualExtent extent) noexcept {
        return allocation != nullptr && CapacityOf(*allocation).contains(extent);
    }

    void WithdrawStaleCandidate() {
        // Once admitted, the page owns the opportunity to make its one claim.
        // Finish that handoff before withdrawing an obsolete allocation.
        if (!candidate_ || !candidate_->timeline || retiring_) return;
        const auto withdrawal = channel_.withdraw(candidate_->import_id);
        DiagnoseAllocation(VisualDiagnosticOperation::PresentationCandidateWithdrawal, *candidate_,
                           static_cast<std::uint64_t>(withdrawal.progress));
        switch (withdrawal.progress) {
            case native::WorkspaceSurfaceWithdrawalProgress::Submitted:
            case native::WorkspaceSurfaceWithdrawalProgress::Retained:
            case native::WorkspaceSurfaceWithdrawalProgress::Pending:
                retiring_ = std::move(candidate_);
                return;
            case native::WorkspaceSurfaceWithdrawalProgress::Retired:
                if (!ReleaseAllocation(candidate_)) throw std::runtime_error("presentation stale candidate release failed");
                return;
            case native::WorkspaceSurfaceWithdrawalProgress::Capacity:
                throw std::runtime_error("presentation stale candidate withdrawal capacity");
            case native::WorkspaceSurfaceWithdrawalProgress::Invalid:
                throw std::runtime_error("presentation stale candidate withdrawal failed");
        }
    }

    void EnsureTargetForPending() {
        if (!pending_frame_) return;
        if (!channel_.connected()) return;
        const auto extent = pending_frame_->submitted.observation.frame.extent;
        if (Contains(active_.get(), extent) || Contains(candidate_.get(), extent)) return;
        if (candidate_) WithdrawStaleCandidate();
        if (!candidate_) {
            // The rapid hardware scenario requests Upscale after drawing Detail,
            // so its retained image exists before this one-shot growth probe.
            // Initial gallery measurement can grow before its first capture.
            // The undersized candidate must finish its real page claim before
            // the ordinary stale-candidate path can retire it.
            if (pending_supersession_acceptance_ && active_ && !retiring_ &&
                pending_frame_->submitted.observation.frame.source.kind == PresentationSourceKind::Upscale) {
                pending_supersession_acceptance_ = false;
                BeginImport({active_->width, active_->height});
            } else {
                BeginImport(extent);
            }
        }
    }

    void AcceptImport(native::WorkspaceSurfaceImportOutcome outcome) {
        if (diagnostics_.valid())
            diagnostics_.Emit([&] {
                return VisualDiagnosticFact{
                    .system = contracts::DiagnosticOwner::Presentation,
                    .operation = VisualDiagnosticOperation::PresentationImportOutcome,
                    .device = settings_.device,
                    .generation = candidate_ && candidate_->import_id == outcome.id ? candidate_->generation : 0U,
                    .value = outcome.imported ? 1U : 0U,
                    .detail = static_cast<std::uint64_t>(outcome.failure),
                    .context = {.capacity_width = candidate_ && candidate_->import_id == outcome.id ? candidate_->width : 0U,
                                .capacity_height = candidate_ && candidate_->import_id == outcome.id ? candidate_->height : 0U,
                                .surface_high = outcome.id.high,
                                .surface_low = outcome.id.low,
                                .selection_generation =
                                    candidate_ && candidate_->import_id == outcome.id ? candidate_->submitted.selection_generation : 0U,
                                .frame_revision = candidate_ && candidate_->import_id == outcome.id
                                                      ? candidate_->submitted.observation.frame.revision
                                                      : 0U,
                                .condition = static_cast<std::uint64_t>(outcome.imported ? PresentationCapabilityCondition::Ready
                                                                                         : PresentationCapabilityCondition::Unavailable),
                                .outcome = outcome.imported ? 0U : static_cast<std::uint64_t>(outcome.failure),
                                .source = candidate_ && candidate_->import_id == outcome.id
                                              ? visual_diagnostic_source(candidate_->submitted.observation)
                                              : contracts::DiagnosticSource{},
                                .allocation = {.allocation_generation =
                                                   candidate_ && candidate_->import_id == outcome.id ? candidate_->generation : 0U}}};
            });
        if (!candidate_ || candidate_->import_id != outcome.id) return;
        if (!outcome.imported) {
            if (outcome.failure == native::workspace_surface_import::FailureCode::Layout &&
                outcome.required_stride <= std::numeric_limits<std::size_t>::max() &&
                outcome.required_size <= std::numeric_limits<std::size_t>::max()) {
                const VisualExtent extent{candidate_->width, candidate_->height};
                if (!ReleaseAllocation(candidate_)) throw std::runtime_error("presentation candidate release failed");
                BeginImport(extent, static_cast<std::size_t>(outcome.required_stride), static_cast<std::size_t>(outcome.required_size));
                return;
            }
            throw std::runtime_error("Firefox surface import failed");
        }
        if (outcome.timeline_descriptor.get() < 0) throw std::runtime_error("Firefox timeline descriptor is unavailable");
        candidate_->timeline.emplace(std::move(outcome.timeline_descriptor));
    }

    PresentationNativeOutcome TryIssue(const std::uint64_t current_selection_generation) {
        if (transfer_target_ || release_wait_pending_) return {};
        if (!pending_frame_) return {};
        if (!application_peer_connected_.load(std::memory_order_acquire) || !channel_.connected()) return {};
        if (pending_frame_->submitted.selection_generation != current_selection_generation) {
            const auto superseded = pending_frame_->submitted;
            pending_frame_.reset();
            return {
                .progress = PresentationNativeProgress::Superseded,
                .submitted = superseded,
            };
        }
        const auto submitted = pending_frame_->submitted;
        const auto& observation = submitted.observation;
        const auto& frame = observation.frame;
        NativeAllocation* target = nullptr;
        if (!retiring_ && candidate_ && candidate_->timeline && Contains(candidate_.get(), frame.extent))
            target = candidate_.get();
        else if (!candidate_ && active_ && active_->timeline && Contains(active_.get(), frame.extent))
            target = active_.get();
        if (target == nullptr || !channel_.claimable(target->import_id)) return {};
        auto stream = reinterpret_cast<cudaStream_t>(stream_.native_handle());
        const auto previous_submitted = target->submitted;
        target->submitted = submitted;
        services::RuntimeDiagnosticSpan borrow_span{
            diagnostics_,
            [&] {
                return visual_diagnostic_boundary(AllocationFact(VisualDiagnosticOperation::PresentationSourceBorrowStarted, *target),
                                                  VisualDiagnosticOperation::PresentationSourceBorrowCompleted);
            },
            pending_frame_->diagnostic_link};
        auto source = pending_frame_->reader->borrow();
        borrow_span.FinishWith([](auto& fact) { fact.detail = fact.context.outcome = 1U; });
        if (!visual_product_matches_frame(frame, source)) {
            target->submitted = previous_submitted;
            pending_frame_.reset();
            return {
                .progress = PresentationNativeProgress::Superseded,
                .submitted = submitted,
            };
        }
        source_read_.emplace(std::move(source));
        std::uint64_t presentation_revision = 0U;
        try {
            stream_.Await(*source_read_);
            DiagnoseAllocation(VisualDiagnosticOperation::PresentationSourceWaitSubmitted, *target);
            const auto clean = source_read_->plane(0U).plane();
            auto* destination = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(target->buffer->data()));
            cudaError_t status = cudaMemset2DAsync(destination, target->pitch, 0, target->pitch, target->height, stream);
            if (status == cudaSuccess)
                status =
                    cudaMemcpy2DAsync(destination, target->pitch, reinterpret_cast<const void*>(clean.data), clean.descriptor.pitch_bytes,
                                      clean.descriptor.row_bytes(), clean.descriptor.height, cudaMemcpyDeviceToDevice, stream);
            if (status == cudaSuccess && source_read_->plane_count() == 2U) {
                const auto semantic = source_read_->plane(1U).plane();
                status = static_cast<cudaError_t>(raster::composite_rgba({
                    .base_rgba = raster::pitched_rgba_target(destination, target->pitch, static_cast<int>(frame.extent.width),
                                                             static_cast<int>(frame.extent.height)),
                    .overlay_rgba =
                        {
                            reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(semantic.data)),
                            semantic.descriptor.pitch_bytes,
                            static_cast<int>(frame.extent.width),
                            static_cast<int>(frame.extent.height),
                        },
                    .stream = stream,
                }));
            }
            DiagnoseAllocation(VisualDiagnosticOperation::PresentationSourceCopy, *target, static_cast<std::uint64_t>(status));
            if (status != cudaSuccess) throw std::runtime_error("presentation exported backbuffer copy failed");
            presentation_revision = mmltk::common::types::take_monotonic_identity(next_presentation_revision_);
            target->latest = NativeAllocation::LatestFrame{
                .submitted = submitted,
                .presentation_revision = presentation_revision,
                .diagnostic_link = pending_frame_->diagnostic_link,
            };
            // Unlock thread-affine borrowed mutexes here. Counted read custody
            // survives the asynchronous return and terminal-owner retirement.
            source_completion_.emplace(std::move(*source_read_));
            source_read_.reset();
            static_cast<void>(OfferLatest(*target, stream));
            transfer_is_publication_ = true;
        } catch (...) {
            // Copy or timeline submission may fail after a source read was
            // queued. Keep custody until the receiver reaches completion; an
            // unprovable settlement stays with this writer through retirement.
            static_cast<void>(SettlePhysicalWork());
            throw;
        }
        return {};
    }

    PresentationNativeOutcome CompleteTransfer() {
        if (!transfer_target_ || !transfer_complete_) return {};
        auto* target = transfer_target_;
        const auto submitted = target->latest->submitted;
        const auto ready = native::detail::workspace_timeline_ready(target->latest->transfer_sequence);
        if (!transfer_copy_completed_) {
            ReleaseSourceRead();
            // Releasing a terminal producer may restore its own CUDA context.
            context_.Bind();
            if (pixel_sample_pending_) {
                const auto* samples = static_cast<const std::uint32_t*>(pixel_samples_->data());
                std::array<VisualDiagnosticFact, 25U> facts;
                for (std::size_t index = 0U; index < pixel_coordinates_.size(); ++index) {
                    facts[index] = pixel_fact_;
                    facts[index].context.pixel = {.sample_index = static_cast<std::uint32_t>(index),
                                                  .sample_x = pixel_coordinates_[index][0],
                                                  .sample_y = pixel_coordinates_[index][1],
                                                  .sample_rgba = samples[index]};
                }
                diagnostics_.WriteBatch(facts);
                pixel_sample_pending_ = false;
            }
            pixel_work_pending_ = false;
            if (ready_span_) {
                ready_span_->FinishWith([](auto& fact) { fact.detail = fact.context.outcome = 1U; });
                ready_span_.reset();
            }
            transfer_copy_completed_ = true;
        }
        if (!application_peer_connected_.load(std::memory_order_acquire) || !channel_.claimable(target->import_id)) return {};
        PublishTransfer(*target, ready);
        transfer_target_ = nullptr;
        transfer_complete_ = false;
        transfer_copy_completed_ = false;
        const bool publication = std::exchange(transfer_is_publication_, false);
        if (!publication) return {};
        if (target == candidate_.get()) {
            if (active_) {
                const auto withdrawal = channel_.withdraw(active_->import_id);
                DiagnoseAllocation(VisualDiagnosticOperation::PresentationActiveWithdrawal, *active_,
                                   static_cast<std::uint64_t>(withdrawal.progress));
                if (withdrawal.progress == native::WorkspaceSurfaceWithdrawalProgress::Invalid)
                    throw std::runtime_error("Firefox incumbent surface withdrawal failed");
                retiring_ = std::move(active_);
            }
            active_ = std::move(candidate_);
            DiagnoseAllocation(VisualDiagnosticOperation::PresentationReplacement, *active_);
        }
        PresentationNativeOutcome outcome{
            .progress = PresentationNativeProgress::Published,
            .submitted = submitted,
            .publication =
                {
                    .capability = CapabilityOf(*active_),
                    .timeline_ready = ready,
                    .presentation_revision = active_->latest->presentation_revision,
                    .transfer_sequence = active_->latest->transfer_sequence,
                },
            .diagnostic_link = active_->latest->diagnostic_link,
        };
        pending_frame_.reset();
        reoffer_pending_ = false;
        return outcome;
    }

    [[nodiscard]] std::uint64_t OfferLatest(NativeAllocation& allocation, cudaStream_t stream) {
        if (!allocation.timeline || !allocation.latest) throw std::runtime_error("presentation transfer has no completed frame");
        const std::uint64_t transfer_sequence = mmltk::common::types::take_monotonic_identity(allocation.next_transfer_sequence);
        const std::uint64_t ready = native::detail::workspace_timeline_ready(transfer_sequence);
        allocation.latest->transfer_sequence = transfer_sequence;
        if (diagnostics_.pixel_probes_enabled()) {
            if (!pixel_probe_failed_ && source_completion_ && !EnqueueCompletion(stream, copy_completion_)) pixel_probe_failed_ = true;
            services::RuntimeDiagnosticSpan probe_span{
                diagnostics_,
                [&] {
                    return visual_diagnostic_boundary(AllocationFact(VisualDiagnosticOperation::PresentationPixelProbeStarted, allocation),
                                                      VisualDiagnosticOperation::PresentationPixelProbeSubmitted);
                },
                allocation.latest->diagnostic_link};
            if (SamplePixels(allocation, stream)) {
                pixel_fact_ = AllocationFact(VisualDiagnosticOperation::PresentationPixel, allocation);
                pixel_fact_.context.link = probe_span.link();
                pixel_sample_pending_ = true;
            }
            probe_span.FinishWith([&](auto& fact) { fact.context.outcome = pixel_probe_failed_ ? 0U : 1U; });
        }
        // The stream orders all image work before the exported ready value.
        // The worker publishes its exact metadata when completion wakes it.
        allocation.timeline->SignalReady(stream, ready);
        if (diagnostics_.valid())
            ready_span_.emplace(
                diagnostics_,
                [&] {
                    return visual_diagnostic_boundary(AllocationFact(VisualDiagnosticOperation::PresentationReadySyncStarted, allocation),
                                                      VisualDiagnosticOperation::PresentationReadySyncCompleted);
                },
                allocation.latest->diagnostic_link);
        transfer_target_ = std::addressof(allocation);
        transfer_copy_completed_ = false;
        if (!EnqueueCompletion(stream, completion_)) throw std::runtime_error("presentation completion submission failed");
        return ready;
    }

    void PublishTransfer(NativeAllocation& allocation, const std::uint64_t ready) {
        const auto& latest = *allocation.latest;
        const auto& frame = latest.submitted.observation.frame;
        native::detail::publish_workspace_frame_signal(allocation.frame_signal.mapping(), ready, latest.transfer_sequence,
                                                       native::WorkspacePresentationLayer::Primary,
                                                       {
                                                           .session = presentation_source_session(frame.source.kind),
                                                           .sequence = frame.revision,
                                                       },
                                                       latest.presentation_revision, frame.extent.width, frame.extent.height);
        const std::uint64_t edge = 1U;
        ssize_t written = -1;
        do {
            written = ::write(allocation.frame_edge.get(), &edge, sizeof(edge));
        } while (written < 0 && errno == EINTR);
        if (written != static_cast<ssize_t>(sizeof(edge))) throw std::runtime_error("presentation frame edge publication failed");
        DiagnoseAllocation(VisualDiagnosticOperation::PresentationFrameEdge, allocation, ready);
        if (diagnostics_.valid())
            release_span_.emplace(
                diagnostics_,
                [&] {
                    return visual_diagnostic_boundary(AllocationFact(VisualDiagnosticOperation::PresentationReleaseWaitStarted, allocation),
                                                      VisualDiagnosticOperation::PresentationReleaseWaitCompleted);
                },
                allocation.latest->diagnostic_link);
        auto stream = reinterpret_cast<cudaStream_t>(stream_.native_handle());
        allocation.timeline->WaitForRelease(stream, ready + 1U);
        release_wait_pending_ = true;
        if (!EnqueueCompletion(stream, completion_)) throw std::runtime_error("presentation completion submission failed");
    }

    // Read only receiver-owned storage. One gather and one bounded DMA join
    // an asynchronous receiver completion; no source borrow, additional wait, or
    // per-frame allocation is needed.
    [[nodiscard]] bool SamplePixels(const NativeAllocation& allocation, cudaStream_t stream) noexcept {
        if (!diagnostics_.pixel_probes_enabled() || pixel_probe_failed_) return false;
        try {
            if (!pixel_samples_) {
                pixel_samples_ = gpu::PinnedHostBuffer::ForCurrentDevice();
                pixel_samples_->ensure_bytes(pixel_coordinates_.size() * sizeof(std::uint32_t));
                void* device = nullptr;
                if (cudaMalloc(&device, pixel_coordinates_.size() * sizeof(std::uint32_t)) != cudaSuccess)
                    throw std::runtime_error("pixel probe storage unavailable");
                pixel_device_.reset(static_cast<std::uint32_t*>(device));
            }
            const auto extent = allocation.latest->submitted.observation.frame.extent;
            const auto coordinate = [](std::size_t index, std::uint32_t size) {
                const std::array<std::uint32_t, 5> points{0U, std::min(191U, size - 1U), std::min(383U, size - 1U), (size - 1U) / 2U,
                                                          size - 1U};
                return points[index];
            };
            std::array<std::uint32_t, 50> coordinates{};
            const auto* source = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(allocation.buffer->data()));
            for (std::size_t index = 0U; index < pixel_coordinates_.size(); ++index) {
                const auto x = coordinate(index % 5U, extent.width);
                const auto y = coordinate(index / 5U, extent.height);
                pixel_coordinates_[index] = {x, y};
                coordinates[index * 2U] = x;
                coordinates[index * 2U + 1U] = y;
            }
            // Set before the first enqueue: even a failed later enqueue can
            // leave a kernel, DMA or callback naming this allocation in flight.
            pixel_work_pending_ = true;
            if (raster::probe_rgba({source, allocation.pitch, static_cast<int>(extent.width), static_cast<int>(extent.height)},
                                   pixel_device_.get(), coordinates, reinterpret_cast<std::uintptr_t>(stream)) != cudaSuccess ||
                cudaMemcpyAsync(pixel_samples_->data(), pixel_device_.get(), pixel_coordinates_.size() * sizeof(std::uint32_t),
                                cudaMemcpyDeviceToHost, stream) != cudaSuccess) {
                pixel_probe_failed_ = true;
                return false;
            }
            return true;
        } catch (...) {
            pixel_probe_failed_ = true;
            return false;
        }
    }

    void DiagnoseAllocation(const VisualDiagnosticOperation operation, const NativeAllocation& allocation,
                            const std::uint64_t outcome = 0U) const noexcept {
        if (!diagnostics_.valid()) return;
        diagnostics_(AllocationFact(operation, allocation, outcome));
    }

    [[nodiscard]] VisualDiagnosticFact AllocationFact(const VisualDiagnosticOperation operation, const NativeAllocation& allocation,
                                                      const std::uint64_t outcome = 0U) const noexcept {
        return presentation_diagnostic_fact(operation, AllocationRecord(operation, allocation), settings_.device, outcome);
    }
    [[nodiscard]] PresentationDiagnosticRecord AllocationRecord(const VisualDiagnosticOperation operation,
                                                                const NativeAllocation& allocation) const noexcept {
        const bool copying = operation == VisualDiagnosticOperation::PresentationSourceBorrowStarted ||
                             operation == VisualDiagnosticOperation::PresentationSourceBorrowCompleted ||
                             operation == VisualDiagnosticOperation::PresentationSourceWaitSubmitted ||
                             operation == VisualDiagnosticOperation::PresentationSourceCopy;
        PresentationDiagnosticRecord record{
            .submitted = allocation.submitted,
            .publication = {.capability = CapabilityOf(allocation)},
            .link = copying && pending_frame_ ? pending_frame_->diagnostic_link : contracts::DiagnosticLink{},
        };
        if (!copying && allocation.latest) {
            record.submitted = allocation.latest->submitted;
            record.link = allocation.latest->diagnostic_link;
            record.publication.presentation_revision = allocation.latest->presentation_revision;
            record.publication.transfer_sequence = allocation.latest->transfer_sequence;
            if (record.publication.transfer_sequence != 0U)
                record.publication.timeline_ready = native::detail::workspace_timeline_ready(record.publication.transfer_sequence);
        }
        if (operation == VisualDiagnosticOperation::PresentationRetirement)
            record.publication.capability.condition = PresentationCapabilityCondition::Unavailable;
        return record;
    }

    VisualDeviceSettings settings_;
    PresentationNativeConfiguration configuration_;
    VisualDiagnosticSink diagnostics_{};
    std::optional<services::RuntimeDiagnosticSpan<VisualDiagnosticSink, VisualDiagnosticFact>> ready_span_;
    std::optional<services::RuntimeDiagnosticSpan<VisualDiagnosticSink, VisualDiagnosticFact>> release_span_;
    native::WorkspaceSurfaceImportChannel channel_;
    gpu::DeviceContext context_;
    std::unique_ptr<gpu::PinnedHostBuffer> pixel_samples_;
    std::unique_ptr<std::uint32_t, PixelDeviceDeleter> pixel_device_;
    std::array<std::array<std::uint32_t, 2>, 25> pixel_coordinates_{};
    VisualDiagnosticFact pixel_fact_{};
    bool pixel_probe_failed_ = false;
    bool pixel_work_pending_ = false;
    bool pixel_sample_pending_ = false;
    gpu::ImageStream stream_;
    mmltk::common::io::ScopedFd completion_fd_;
    Completion completion_;
    Completion copy_completion_;
    bool completion_settlement_pending_ = false;
    std::optional<gpu::BorrowedImageProductReadView> source_read_;
    std::optional<gpu::ImageProductReadCompletion> source_completion_;
    NativeAllocation* transfer_target_ = nullptr;
    bool transfer_complete_ = false;
    bool transfer_copy_completed_ = false;
    bool transfer_is_publication_ = false;
    bool reoffer_pending_ = false;
    std::unique_ptr<NativeAllocation> active_;
    std::unique_ptr<NativeAllocation> candidate_;
    std::unique_ptr<NativeAllocation> retiring_;
    std::optional<PendingFrame> pending_frame_;
    std::uint64_t next_generation_ = 1U;
    std::uint64_t next_presentation_revision_ = 1U;
    bool release_wait_pending_ = false;
    std::atomic_bool application_peer_connected_{true};
    bool browser_terminal_ = false;
    bool terminal_retired_ = false;
    bool terminal_release_succeeded_ = true;
    bool pending_supersession_acceptance_ = configuration_.pending_supersession_acceptance;
};

}  // namespace

PresentationNativeWriterFactory make_native_presentation_writer_factory(const VisualDeviceSettings settings,
                                                                        PresentationNativeConfiguration configuration,
                                                                        const VisualDiagnosticSink diagnostics) {
    if (!settings.valid() || configuration.import_socket.empty() || configuration.pitch_alignment == 0U)
        throw contracts::InvalidIntentError("Presentation native configuration is invalid");
    return [settings, execution = resolve_visual_device_execution(settings), configuration = std::move(configuration), diagnostics] {
        return std::make_unique<NativePresentationWriter>(settings, configuration, diagnostics, execution);
    };
}

}  // namespace mmltk::controller
