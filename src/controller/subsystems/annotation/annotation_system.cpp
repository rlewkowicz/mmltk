#include "src/controller/subsystems/annotation/annotation_system.h"
#include "src/controller/presentation/detail/visual_runtime_owner.h"
#include "src/frameworks/gpu/system_image_runtime.h"

#include <mutex>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

#include "src/common/types/generation.h"

namespace mmltk::controller {
namespace {

[[nodiscard]] AnnotationAlgorithm& annotation_algorithm(mmltk::frameworks::gpu::SystemImageRuntime& runtime) {
    auto* const algorithm = dynamic_cast<AnnotationAlgorithm*>(runtime.model());
    if (algorithm == nullptr) throw std::runtime_error("Annotation document runtime is unavailable");
    return *algorithm;
}

void require_annotation_ui(const contracts::AnnotationUiState& ui) {
    if (!ui.valid()) throw contracts::UnavailableError("Annotation document state is invalid");
}

}  // namespace

class AnnotationSystem::Impl final {
    struct InputSlot final {
        AnnotationInputBatch batch;
        bool occupied = false;
    };

   public:
    Impl(const VisualDeviceSettings settings, VisualRuntimeFactory factory, ExactVisualDocumentBorrower borrow_source,
         SystemEventSink<event_type> events, const VisualDiagnosticSink diagnostics)
        : settings_(settings),
          borrow_source_(std::move(borrow_source)),
          events_(std::move(events)),
          diagnostics_(diagnostics),
          worker_(std::move(factory), [this](const std::exception_ptr failure) { Failed(failure); }) {
        if (!settings_.valid() || !borrow_source_) throw contracts::InvalidIntentError("Annotation device settings are invalid");
        worker_.RegisterOrderedDrain([this](auto& runtime, auto stop) { return DrainInput(runtime, stop); });
        worker_.RegisterContinuation(
            [this](auto& runtime, auto stop) {
                RenderPending(runtime, stop, false);
                return detail::VisualRuntimeOwner::Notification{};
            },
            {}, true);
    }

    [[nodiscard]] AnnotationSnapshot Open(const AnnotationOpen request) {
        {
            std::scoped_lock lock(mutex_);
            RequireIdle();
        }
        auto source = borrow_source_(request.source);
        if (!source.valid()) throw contracts::InvalidIntentError("Annotation source image is unavailable");
        const auto descriptor = source.pixels.plane(0U).plane().descriptor;
        if (descriptor.width > settings_.maximum_width || descriptor.height > settings_.maximum_height)
            throw contracts::InvalidIntentError("Annotation source exceeds the configured device bounds");
        AnnotationSnapshot admitted;
        {
            std::scoped_lock lock(mutex_);
            RequireIdle();
            const auto prior = state_;
            Admit();
            if (!worker_.SubmitDiscrete(
                    [this, request, source = std::move(source)](
                        mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                        const std::stop_token stop) mutable -> detail::VisualRuntimeOwner::Notification {
                        RenderPending(runtime, stop, true);
                        if (stop.stop_requested()) return [this] { Cancelled(); };
                        const auto crop = request.original_content ? request.source.content : VisualRegion{};
                        contracts::AnnotationSceneContent scene;
                        try {
                            scene = materialize_visual_document(*source.document, request.source.extent, crop);
                        } catch (const contracts::InvalidIntentError& error) {
                            // Reject before touching receiver storage or the existing editor.
                            // GPU/copy/render failures still use aggregate retirement below.
                            return [this, detail = std::string(error.what())] { Rejected(detail, true, std::nullopt); };
                        }
                        const auto paths = runtime.CopyInputFrom(std::move(source.pixels));
                        diagnostics_.Emit([&] {
                            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Annotation,
                                                        .operation = VisualDiagnosticOperation::CopyCompleted,
                                                        .device = settings_.device,
                                                        .copy_path = paths[0U]};
                        });
                        const auto input = runtime.BorrowInput();
                        const auto source_plane = input.plane(0U).plane();
                        auto result = annotation_algorithm(runtime).Open(source_plane, std::move(scene), crop);
                        if (result.outcome == AnnotationOperationOutcome::Rejected) throw contracts::InvalidIntentError(result.detail);
                        require_annotation_ui(result.ui);
                        runtime.Publish(result.ui.scene.frame_width, result.ui.scene.frame_height,
                                        [&](const auto clean, const auto semantic, const auto stream) {
                                            annotation_algorithm(runtime).Render(source_plane, clean, semantic, stream);
                                        });
                        const VisualExtent extent{result.ui.scene.frame_width, result.ui.scene.frame_height};
                        clean_revision_ = runtime.OutputFacts().revision;
                        auto frame = Frame(runtime, extent);
                        if (result.outcome == AnnotationOperationOutcome::Rejected)
                            return [this, ui = std::move(result.ui), detail = std::move(result.detail), frame]() mutable {
                                Rejected(std::move(detail), true, std::move(ui), frame);
                            };
                        return [this, ui = std::move(result.ui), frame]() mutable {
                            Settled([&](auto& state) {
                                state.ready = true;
                                state.input_document_epoch = mmltk::common::types::advance_monotonic_identity(state.input_document_epoch);
                                state.ui = std::move(ui);
                                state.frame = frame;
                            });
                            diagnostics_.Emit([&] {
                                return VisualDiagnosticFact{
                                    .system = contracts::DiagnosticOwner::Annotation,
                                    .operation = VisualDiagnosticOperation::DocumentOpened,
                                    .device = settings_.device,
                                    .generation = frame.revision,
                                    .context = {.capacity_width = frame.extent.width,
                                                .capacity_height = frame.extent.height,
                                                .source = visual_diagnostic_source({.frame = frame})},
                                };
                            });
                        };
                    },
                    [this] { Cancelled(); })) {
                state_ = prior;
                throw contracts::BusyError("Annotation is busy");
            }
            admitted = state_;
        }
        // CLEANUP-IGNORE: Open and Save return their independently admitted typed snapshots after system-owned checks.
        return admitted;
    }

    void SetInputPeer(const std::uint64_t epoch, SystemEventSink<AnnotationInputProgress> progress) {
        bool ready = false;
        {
            std::scoped_lock lock(mutex_);
            input_epoch_ = epoch;
            admitted_sequence_ = 0U;
            consumed_sequence_ = 0U;
            input_progress_ = std::move(progress);
            ready = !terminal_barrier_;
        }
        if (ready) InputReady();
    }
    void InputReady() {
        SystemEventSink<AnnotationInputProgress> progress;
        AnnotationInputProgress value;
        {
            std::scoped_lock lock(mutex_);
            value = {input_epoch_, consumed_sequence_};
            progress = input_progress_;
        }
        if (progress) progress(value);
    }

    void Input(AnnotationInputBatch&& batch) {
        std::scoped_lock lock(mutex_);
        if (state_.busy) throw contracts::InvalidIntentError("Annotation command barrier is unsettled");
        RequireInputReady();
        if (terminal_barrier_) throw contracts::UnavailableError("Annotation input is resetting");
        if (batch.epoch == 0U || batch.epoch != input_epoch_ || batch.document_epoch != state_.input_document_epoch ||
            batch.sequence == 0U || batch.sequence != admitted_sequence_ + 1U || batch.samples.empty())
            throw contracts::InvalidIntentError("Annotation input epoch or sequence is invalid");
        for (const auto& pointer : batch.samples) {
            if (!pointer.valid() || pointer.point.x < 0.0F || pointer.point.y < 0.0F ||
                pointer.point.x > static_cast<float>(state_.frame.extent.width) ||
                pointer.point.y > static_cast<float>(state_.frame.extent.height))
                throw contracts::InvalidIntentError("Annotation batch contains invalid pointer input");
        }
        auto slot = std::ranges::find(input_slots_, false, &InputSlot::occupied);
        if (slot == input_slots_.end()) throw contracts::InvalidIntentError("Annotation input exceeded its two consumption credits");
        slot->batch = std::move(batch);
        slot->occupied = true;
        if (!worker_.NotifyOrderedDrain()) {
            slot->occupied = false;
            throw contracts::UnavailableError("Annotation input worker is unavailable");
        }
        admitted_sequence_ = slot->batch.sequence;
    }

    void PeerClosed() noexcept {
        {
            std::scoped_lock lock(mutex_);
            if (terminal_barrier_ || !state_.ready || !state_.frame.valid() || !state_.ui.valid()) return;
            terminal_barrier_ = true;
        }
        if (worker_.SubmitTerminalBarrier([this](mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                                 const std::stop_token stop) -> detail::VisualRuntimeOwner::Notification {
                require_annotation_ui(annotation_algorithm(runtime).Ui());
                annotation_algorithm(runtime).PeerClosed();
                render_pending_ = true;
                RenderPending(runtime, stop, true);
                return [this] {
                    {
                        std::scoped_lock lock(mutex_);
                        terminal_barrier_ = false;
                    }
                    InputReady();
                };
            }))
            return;
        std::scoped_lock lock(mutex_);
        terminal_barrier_ = false;
    }

    [[nodiscard]] AnnotationSnapshot Edit(AnnotationEditRequest request) {
        {
            std::scoped_lock lock(mutex_);
            RequireIdle();
        }
        return SubmitDiscrete([edit = std::move(request.edit)](AnnotationAlgorithm& algorithm) mutable { return algorithm.Edit(edit); },
                              VisualDiagnosticOperation::DocumentEdited);
    }

    [[nodiscard]] AnnotationSnapshot Save(AnnotationSave request) {
        {
            std::scoped_lock lock(mutex_);
            RequireIdle();
        }
        if (request.destination.empty())
            // CLEANUP-IGNORE: Save validates a destination while Open validates a borrowed source before separate
            // admission paths.
            throw contracts::InvalidIntentError("Annotation save destination is unavailable");
        AnnotationSnapshot admitted;
        {
            std::scoped_lock lock(mutex_);
            RequireReady();
            const auto prior = state_;
            Admit();
            if (!worker_.SubmitDiscrete(
                    [this, request = std::move(request)](mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                                         const std::stop_token stop) mutable -> detail::VisualRuntimeOwner::Notification {
                        if (stop.stop_requested()) return [this] { Cancelled(); };
                        require_annotation_ui(annotation_algorithm(runtime).Ui());
                        RenderPending(runtime, stop, true);
                        if (stop.stop_requested()) return [this] { Cancelled(); };
                        auto result = annotation_algorithm(runtime).Save(request.destination);
                        return CompleteOperation(std::move(result));
                    },
                    [this] { Cancelled(); })) {
                state_ = prior;
                throw contracts::BusyError("Annotation is busy");
            }
            admitted = state_;
        }
        return admitted;
    }

    // CLEANUP-IGNORE: Each sealed visual system cancels its own typed snapshot while VisualRuntimeOwner owns worker
    // stop.
    [[nodiscard]] AnnotationSnapshot Stop() noexcept {
        // CLEANUP-IGNORE: Cancellation updates the Annotation snapshot before forwarding stop to its shared worker
        // owner.
        bool accepted = false;
        {
            std::scoped_lock lock(mutex_);
            if (state_.busy && !state_.cancellation_requested) {
                state_.cancellation_requested = true;
                AdvanceRevision();
                state_.ui_revision = state_.revision;
                accepted = true;
            }
        }
        if (accepted) worker_.RequestActiveStop();
        return snapshot();
    }
    // CLEANUP-IGNORE: This sealed Annotation facade tail has domain-specific state and locking despite sharing
    // conventional shutdown, snapshot, and borrow method shapes with Explore.
    void Shutdown() noexcept { worker_.StopAndWait(); }
    [[nodiscard]] bool stopped() const noexcept { return worker_.stopped(); }
    [[nodiscard]] AnnotationSnapshot snapshot() const {
        std::scoped_lock lock(mutex_);
        return state_;
    }
    [[nodiscard]] VisualSourceObservation ObserveSource() const {
        std::scoped_lock lock(mutex_);
        return visual_source::Observe(state_);
    }
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const {
        VisualFrame committed;
        {
            std::scoped_lock lock(mutex_);
            if (!state_.ready) return {};
            committed = state_.frame;
        }
        return borrow_matching_visual_product(committed, worker_);
    }

   private:
    [[nodiscard]] detail::VisualRuntimeOwner::Notification DrainInput(mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                                                      const std::stop_token stop) {
        std::inplace_vector<InputSlot*, kAnnotationInputAdmissionSlots> frontier;
        {
            std::scoped_lock lock(mutex_);
            for (auto& slot : input_slots_)
                if (slot.occupied) frontier.push_back(&slot);
            std::ranges::sort(frontier, {}, [](const auto* slot) { return slot->batch.sequence; });
        }
        auto& algorithm = annotation_algorithm(runtime);
        std::inplace_vector<std::string, kAnnotationInputBatchCapacity * kAnnotationInputAdmissionSlots> rejections;
        bool ui_changed = false;
        for (auto* storage : frontier) {
            for (const auto& pointer : storage->batch.samples) {
                auto result = algorithm.Pointer(pointer);
                ui_changed = ui_changed || result.ui_changed;
                if (result.outcome == AnnotationOperationOutcome::Rejected) rejections.push_back(std::move(result.detail));
            }
            SystemEventSink<AnnotationInputProgress> progress;
            AnnotationInputProgress consumed;
            {
                std::scoped_lock lock(mutex_);
                consumed = {storage->batch.epoch, storage->batch.sequence};
                storage->occupied = false;
                if (consumed.epoch == input_epoch_) {
                    consumed_sequence_ = consumed.consumed_sequence;
                    progress = input_progress_;
                }
            }
            // Return exact CPU credits before any GPU publication or wait.
            if (progress) progress(consumed);
        }
        if (ui_changed) {
            const auto& ui = algorithm.Ui();
            require_annotation_ui(ui);
            pending_ui_ = ui;
        }
        render_pending_ = render_pending_ || !frontier.empty();
        RenderPending(runtime, stop, false);
        for (auto& rejection : rejections)
            Rejected(std::move(rejection), false, std::nullopt);
        return {};
    }
    void RenderPending(mmltk::frameworks::gpu::SystemImageRuntime& runtime, const std::stop_token stop, const bool wait) {
        if (!render_pending_) {
            worker_.SetOutputRetry(false);
            return;
        }
        VisualExtent extent;
        {
            std::scoped_lock lock(mutex_);
            extent = state_.frame.extent;
        }
        // Retain the baseline across a failed try: releasing it generates an availability
        // notification, which must not turn capacity pressure into a self-waking loop.
        if (!pending_baseline_.valid()) pending_baseline_ = runtime.Completed();
        // Arm before observing writability: a receiver release racing the try
        // either makes reservation succeed or leaves a coalesced retry pending.
        worker_.SetOutputRetry(!wait);
        auto output = wait
                          ? runtime.AcquireOutput(stop, std::move(pending_baseline_), mmltk::frameworks::gpu::ImagePlanePreservation::Clean)
                          : runtime.TryAcquireOutput(pending_baseline_, mmltk::frameworks::gpu::ImagePlanePreservation::Clean);
        if (!output.valid()) {
            if (wait && stop.stop_requested()) {
                bool cancelled_command = false;
                {
                    std::scoped_lock lock(mutex_);
                    cancelled_command = state_.busy && state_.cancellation_requested && !terminal_barrier_;
                }
                if (cancelled_command) {
                    // Re-arm first, then request one retry to cover a receiver
                    // release during the disarmed blocking wait. An unsuccessful
                    // continuation retains its baseline and waits for availability.
                    worker_.SetOutputRetry(true);
                    static_cast<void>(worker_.NotifyContinuation());
                }
            }
            return;
        }
        worker_.SetOutputRetry(false);
        runtime.Publish(output, extent.width, extent.height, [&](const auto clean, const auto semantic, const auto stream) {
            annotation_algorithm(runtime).Render({}, clean, semantic, stream);
        });
        runtime.CommitOutput(std::move(output));
        render_pending_ = false;
        InstallInteraction(std::move(pending_ui_), Frame(runtime, extent));
        pending_ui_.reset();
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification CompleteOperation(AnnotationOperationResult result,
                                                                             const VisualFrame frame = {}) {
        require_annotation_ui(result.ui);
        return [this, result = std::move(result), frame]() mutable {
            if (result.outcome == AnnotationOperationOutcome::Rejected) {
                Rejected(std::move(result.detail), true, std::move(result.ui));
                return;
            }
            Settled([&](auto& state) {
                state.ui = std::move(result.ui);
                if (frame.valid()) state.frame = frame;
            });
        };
    }

    template <class Operation>
    [[nodiscard]] AnnotationSnapshot SubmitDiscrete(Operation operation, const VisualDiagnosticOperation diagnostic) {
        AnnotationSnapshot admitted;
        {
            std::scoped_lock lock(mutex_);
            RequireReady();
            const auto prior = state_;
            const auto extent = state_.frame.extent;
            Admit();
            if (!worker_.SubmitDiscrete(
                    [this, operation = std::move(operation), diagnostic, extent](
                        mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                        const std::stop_token stop) mutable -> detail::VisualRuntimeOwner::Notification {
                        require_annotation_ui(annotation_algorithm(runtime).Ui());
                        RenderPending(runtime, stop, true);
                        const auto input = runtime.BorrowInput();
                        if (!input.valid()) throw contracts::UnavailableError("Annotation source storage is unavailable");
                        if (stop.stop_requested()) return [this] { Cancelled(); };
                        auto output =
                            runtime.AcquireOutput(stop, runtime.Completed(), mmltk::frameworks::gpu::ImagePlanePreservation::Clean);
                        if (!output.valid()) return [this] { Cancelled(); };
                        auto result = operation(annotation_algorithm(runtime));
                        require_annotation_ui(result.ui);
                        if (result.outcome == AnnotationOperationOutcome::Rejected) return CompleteOperation(std::move(result));
                        runtime.Publish(output, extent.width, extent.height, [&](const auto clean, const auto semantic, const auto stream) {
                            annotation_algorithm(runtime).Render({}, clean, semantic, stream);
                        });
                        runtime.CommitOutput(std::move(output));
                        diagnostics_.Emit([&] {
                            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Annotation,
                                                        .operation = diagnostic,
                                                        .device = settings_.device,
                                                        .generation = result.ui.document_revision};
                        });
                        auto frame = Frame(runtime, extent);
                        return CompleteOperation(std::move(result), frame);
                    },
                    [this] { Cancelled(); })) {
                state_ = prior;
                throw contracts::BusyError("Annotation is busy");
            }
            admitted = state_;
        }
        return admitted;
    }

    [[nodiscard]] VisualFrame Frame(const mmltk::frameworks::gpu::SystemImageRuntime& runtime, const VisualExtent extent) const {
        auto frame = visual_frame({PresentationSourceKind::Annotation, 1U}, extent, runtime.OutputFacts().revision);
        frame.clean_revision = clean_revision_;
        return frame;
    }
    void RequireIdle() const {
        if (state_.busy) throw contracts::BusyError("Annotation is busy");
    }
    void RequireReady() const {
        RequireInputReady();
        require_annotation_ui(state_.ui);
    }
    void RequireInputReady() const {
        RequireIdle();
        if (!state_.ready || !state_.frame.valid()) throw contracts::UnavailableError("Annotation document is unavailable");
    }
    void Admit() {
        state_.busy = true;
        state_.cancellation_requested = false;
        // CLEANUP-IGNORE: Annotation advances its own snapshot revision when admission changes observable facts.
        AdvanceRevision();
        state_.ui_revision = state_.revision;
    }
    void AdvanceRevision() { state_.revision = mmltk::common::types::advance_monotonic_identity(state_.revision); }
    template <class Install>
    void Settled(Install install) noexcept {
        AnnotationSnapshot settled;
        {
            std::scoped_lock lock(mutex_);
            install(state_);
            state_.busy = false;
            state_.cancellation_requested = false;
            AdvanceRevision();
            state_.ui_revision = state_.revision;
            settled = state_;
        }
        Publish(AnnotationChanged{std::move(settled)});
    }
    void InstallInteraction(std::optional<contracts::AnnotationUiState> ui, const VisualFrame frame) noexcept {
        std::optional<AnnotationSnapshot> changed;
        AnnotationFrameState rendered;
        {
            std::scoped_lock lock(mutex_);
            if (ui && ui->document_revision < state_.ui.document_revision) return;
            state_.frame = frame;
            AdvanceRevision();
            if (ui) {
                state_.ui = std::move(*ui);
                state_.ui_revision = state_.revision;
                changed = state_;
            }
            rendered = {state_.revision, state_.ui_revision, frame};
        }
        if (changed) Publish(AnnotationChanged{std::move(*changed)});
        Publish(AnnotationFrameChanged{rendered});
    }
    void Cancelled() noexcept {
        Settled([](auto&) {});
    }
    void Rejected(std::string detail, const bool settle_busy, std::optional<contracts::AnnotationUiState> ui,
                  const VisualFrame frame = {}) noexcept {
        AnnotationSnapshot failed;
        {
            std::scoped_lock lock(mutex_);
            if (settle_busy) {
                state_.busy = false;
                state_.cancellation_requested = false;
            }
            if (ui) state_.ui = std::move(*ui);
            if (frame.valid()) state_.frame = frame;
            AdvanceRevision();
            state_.ui_revision = state_.revision;
            failed = state_;
        }
        Publish(AnnotationFailed{std::move(failed), std::move(detail)});
    }
    void Failed(const std::exception_ptr failure) noexcept {
        worker_.SetOutputRetry(false);
        pending_baseline_ = {};
        render_pending_ = false;
        auto detail = visual_failure_detail(failure, "Annotation GPU worker failed");
        AnnotationSnapshot failed;
        SystemEventSink<AnnotationInputProgress> progress;
        AnnotationInputProgress rejected;
        {
            std::scoped_lock lock(mutex_);
            if (std::ranges::any_of(input_slots_, [this](const auto& slot) { return slot.occupied && slot.batch.epoch == input_epoch_; })) {
                progress = input_progress_;
                rejected = {input_epoch_, consumed_sequence_, detail};
            }
            if (pending_ui_) state_.ui = std::move(*pending_ui_);
            pending_ui_.reset();
            state_.ready = false;
            state_.frame = {};
            state_.busy = false;
            // CLEANUP-IGNORE: Annotation failure clears its own cancellation fact before publishing its typed event.
            state_.cancellation_requested = false;
            terminal_barrier_ = false;
            for (auto& slot : input_slots_)
                slot.occupied = false;
            // CLEANUP-IGNORE: Annotation owns this state transition; common noexcept event publication is already
            // shared.
            AdvanceRevision();
            state_.ui_revision = state_.revision;
            failed = state_;
        }
        report_visual_worker_failure(diagnostics_, contracts::DiagnosticOwner::Annotation, settings_.device, detail);
        if (progress) progress(std::move(rejected));
        Publish(AnnotationFailed{std::move(failed), std::move(detail)});
    }
    template <class Event>
    void Publish(Event event) noexcept {
        publish_visual_event_noexcept(events_, event_type{std::move(event)});
    }

    VisualDeviceSettings settings_;
    ExactVisualDocumentBorrower borrow_source_;
    SystemEventSink<event_type> events_;
    VisualDiagnosticSink diagnostics_{};
    mutable std::mutex mutex_;
    AnnotationSnapshot state_;
    std::array<InputSlot, kAnnotationInputAdmissionSlots> input_slots_{};
    std::uint64_t input_epoch_ = 0U;
    std::uint64_t admitted_sequence_ = 0U;
    std::uint64_t consumed_sequence_ = 0U;
    SystemEventSink<AnnotationInputProgress> input_progress_;
    bool terminal_barrier_ = false;
    std::optional<contracts::AnnotationUiState> pending_ui_;
    mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput pending_baseline_;
    bool render_pending_ = false;
    std::uint64_t clean_revision_ = 0U;
    detail::VisualRuntimeOwner worker_;
};

// CLEANUP-IGNORE: The public Annotation facade forwards construction to its one private implementation owner.
AnnotationSystem::AnnotationSystem(const VisualDeviceSettings settings, VisualRuntimeFactory factory,
                                   ExactVisualDocumentBorrower borrow_source, SystemEventSink<event_type> events,
                                   const VisualDiagnosticSink diagnostics)
    : impl_(std::make_unique<Impl>(settings, std::move(factory), std::move(borrow_source), std::move(events), diagnostics)) {}
AnnotationSystem::~AnnotationSystem() = default;
AnnotationSnapshot AnnotationSystem::Open(const AnnotationOpen request) { return impl_->Open(request); }
void AnnotationSystem::Input(AnnotationInputBatch batch) { impl_->Input(std::move(batch)); }
void AnnotationSystem::SetInputPeer(std::uint64_t epoch, SystemEventSink<AnnotationInputProgress> progress) {
    impl_->SetInputPeer(epoch, std::move(progress));
}
AnnotationSnapshot AnnotationSystem::Edit(AnnotationEditRequest request) { return impl_->Edit(std::move(request)); }
AnnotationSnapshot AnnotationSystem::Save(AnnotationSave request) { return impl_->Save(std::move(request)); }
AnnotationSnapshot AnnotationSystem::Stop() noexcept { return impl_->Stop(); }
void AnnotationSystem::PeerClosed() noexcept { impl_->PeerClosed(); }
void AnnotationSystem::Shutdown() noexcept { impl_->Shutdown(); }
bool AnnotationSystem::stopped() const noexcept { return impl_->stopped(); }
AnnotationSnapshot AnnotationSystem::snapshot() const { return impl_->snapshot(); }
VisualSourceObservation AnnotationSystem::ObserveSource() const { return impl_->ObserveSource(); }
mmltk::frameworks::gpu::BorrowedImageProductReadView AnnotationSystem::BorrowFrame() const { return impl_->BorrowFrame(); }

}  // namespace mmltk::controller
