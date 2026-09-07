#include "src/controller/subsystems/annotation/annotation_system.h"
#include "src/controller/presentation/detail/visual_runtime_owner.h"
#include "src/frameworks/gpu/system_image_worker.h"

#include <mutex>
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

}  // namespace

class AnnotationSystem::Impl final {
   public:
    Impl(const VisualDeviceSettings settings, VisualRuntimeFactory factory, ExactVisualDocumentBorrower borrow_source,
         SystemEventSink<event_type> events, const VisualDiagnosticSink diagnostics)
        : settings_(settings),
          borrow_source_(std::move(borrow_source)),
          events_(std::move(events)),
          diagnostics_(diagnostics),
          worker_(std::move(factory), [this](const std::exception_ptr failure) { Failed(failure); }) {
        if (!settings_.valid() || !borrow_source_) throw contracts::InvalidIntentError("Annotation device settings are invalid");
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
                        diagnostics_({.system = VisualSystemKind::Annotation,
                                      .operation = VisualDiagnosticOperation::CopyCompleted,
                                      .device = settings_.device,
                                      .copy_path = paths[0U]});
                        const auto input = runtime.BorrowInput();
                        const auto source_plane = input.plane(0U).plane();
                        auto result = annotation_algorithm(runtime).Open(source_plane, std::move(scene), crop);
                        if (result.outcome == AnnotationOperationOutcome::Rejected) throw contracts::InvalidIntentError(result.detail);
                        runtime.Publish(result.ui.scene.frame_width, result.ui.scene.frame_height,
                                        [&](const auto clean, const auto semantic, const auto stream) {
                                            annotation_algorithm(runtime).Render(source_plane, clean, semantic, stream);
                                        });
                        const VisualExtent extent{result.ui.scene.frame_width, result.ui.scene.frame_height};
                        auto frame = Frame(runtime, extent);
                        if (result.outcome == AnnotationOperationOutcome::Rejected)
                            return [this, ui = std::move(result.ui), detail = std::move(result.detail), frame]() mutable {
                                Rejected(std::move(detail), true, std::move(ui), frame);
                            };
                        return [this, ui = std::move(result.ui), frame]() mutable {
                            Settled([&](auto& state) {
                                state.ready = true;
                                state.ui = std::move(ui);
                                state.frame = frame;
                            });
                            diagnostics_({
                                .system = VisualSystemKind::Annotation,
                                .operation = VisualDiagnosticOperation::DocumentOpened,
                                .device = settings_.device,
                                .generation = frame.revision,
                                .context = {.capacity_width = frame.extent.width, .capacity_height = frame.extent.height},
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

    void Pointer(const AnnotationPointer pointer) {
        if (!pointer.valid()) throw contracts::InvalidIntentError("Annotation pointer input is invalid");
        VisualExtent extent;
        {
            std::scoped_lock lock(mutex_);
            RequireReady();
            if (terminal_barrier_) throw contracts::UnavailableError("Annotation pointer ingress is resetting");
            if (pointer.point.x < 0.0F || pointer.point.y < 0.0F || pointer.point.x > static_cast<float>(state_.frame.extent.width) ||
                pointer.point.y > static_cast<float>(state_.frame.extent.height))
                throw contracts::InvalidIntentError("Annotation pointer is outside the active image");
            extent = state_.frame.extent;
        }
        auto work = [this, pointer, extent](mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                            const std::stop_token stop) -> detail::VisualRuntimeOwner::Notification {
            const auto input = runtime.BorrowInput();
            if (!input.valid()) throw contracts::UnavailableError("Annotation source storage is unavailable");
            const auto source = input.plane(0U).plane();
            if (stop.stop_requested()) return {};
            auto result = annotation_algorithm(runtime).Pointer(pointer);
            runtime.Publish(extent.width, extent.height, [&](const auto clean, const auto semantic, const auto stream) {
                annotation_algorithm(runtime).Render(source, clean, semantic, stream);
            });
            auto frame = Frame(runtime, extent);
            if (result.outcome == AnnotationOperationOutcome::Rejected)
                return [this, ui = std::move(result.ui), detail = std::move(result.detail), frame]() mutable {
                    Rejected(std::move(detail), false, std::move(ui), frame);
                };
            return [this, ui = std::move(result.ui), frame]() mutable { InstallInteraction(std::move(ui), frame); };
        };
        std::scoped_lock admission_lock(mutex_);
        if (terminal_barrier_) throw contracts::UnavailableError("Annotation pointer ingress is resetting");
        if (!worker_.SubmitOrdered(std::move(work))) { throw contracts::BusyError("Annotation pointer queue is full"); }
    }

    void PeerClosed() noexcept {
        VisualExtent extent;
        {
            std::scoped_lock lock(mutex_);
            if (terminal_barrier_ || !state_.ready || !state_.frame.valid() || !state_.ui.valid()) return;
            terminal_barrier_ = true;
            extent = state_.frame.extent;
        }
        if (worker_.SubmitTerminalBarrier([this, extent](mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                                         std::stop_token) -> detail::VisualRuntimeOwner::Notification {
                annotation_algorithm(runtime).PeerClosed();
                const auto input = runtime.BorrowInput();
                if (!input.valid()) throw contracts::UnavailableError("Annotation source storage is unavailable");
                const auto source = input.plane(0U).plane();
                runtime.Publish(extent.width, extent.height, [&](const auto clean, const auto semantic, const auto stream) {
                    annotation_algorithm(runtime).Render(source, clean, semantic, stream);
                });
                const auto frame = Frame(runtime, extent);
                return [this, frame] {
                    AnnotationSnapshot changed;
                    {
                        std::scoped_lock lock(mutex_);
                        terminal_barrier_ = false;
                        state_.frame = frame;
                        AdvanceRevision();
                        changed = state_;
                    }
                    Publish(AnnotationChanged{std::move(changed)});
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
                accepted = true;
            }
        }
        if (accepted) worker_.RequestActiveStop();
        return snapshot();
    }
    void Shutdown() noexcept { worker_.StopAndWait(); }
    [[nodiscard]] bool stopped() const noexcept { return worker_.stopped(); }
    [[nodiscard]] AnnotationSnapshot snapshot() const {
        std::scoped_lock lock(mutex_);
        return state_;
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
    [[nodiscard]] detail::VisualRuntimeOwner::Notification CompleteOperation(AnnotationOperationResult result,
                                                                            const VisualFrame frame = {}) {
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
                        const auto input = runtime.BorrowInput();
                        if (!input.valid()) throw contracts::UnavailableError("Annotation source storage is unavailable");
                        const auto source = input.plane(0U).plane();
                        if (stop.stop_requested()) return [this] { Cancelled(); };
                        auto result = operation(annotation_algorithm(runtime));
                        if (result.outcome == AnnotationOperationOutcome::Rejected)
                            return CompleteOperation(std::move(result));
                        runtime.Publish(extent.width, extent.height, [&](const auto clean, const auto semantic, const auto stream) {
                            annotation_algorithm(runtime).Render(source, clean, semantic, stream);
                        });
                        diagnostics_({.system = VisualSystemKind::Annotation,
                                      .operation = diagnostic,
                                      .device = settings_.device,
                                      .generation = result.ui.document_revision});
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
        return visual_frame({PresentationSourceKind::Annotation, 1U}, extent, runtime.output().revision());
    }
    void RequireIdle() const {
        if (state_.busy) throw contracts::BusyError("Annotation is busy");
    }
    void RequireReady() const {
        RequireIdle();
        if (!state_.ready || !state_.frame.valid() || !state_.ui.valid())
            throw contracts::UnavailableError("Annotation document is unavailable");
    }
    void Admit() {
        state_.busy = true;
        state_.cancellation_requested = false;
        // CLEANUP-IGNORE: Annotation advances its own snapshot revision when admission changes observable facts.
        AdvanceRevision();
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
            settled = state_;
        }
        Publish(AnnotationChanged{std::move(settled)});
    }
    void InstallInteraction(contracts::AnnotationUiState ui, const VisualFrame frame) noexcept {
        AnnotationSnapshot changed;
        {
            std::scoped_lock lock(mutex_);
            if (ui.document_revision < state_.ui.document_revision) return;
            state_.ui = std::move(ui);
            state_.frame = frame;
            AdvanceRevision();
            changed = state_;
        }
        Publish(AnnotationChanged{std::move(changed)});
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
            failed = state_;
        }
        Publish(AnnotationFailed{std::move(failed), std::move(detail)});
    }
    void Failed(const std::exception_ptr failure) noexcept {
        auto detail = visual_failure_detail(failure, "Annotation GPU worker failed");
        AnnotationSnapshot failed;
        {
            std::scoped_lock lock(mutex_);
            state_.ready = false;
            state_.frame = {};
            state_.busy = false;
            // CLEANUP-IGNORE: Annotation failure clears its own cancellation fact before publishing its typed event.
            state_.cancellation_requested = false;
            terminal_barrier_ = false;
            // CLEANUP-IGNORE: Annotation owns this state transition; common noexcept event publication is already
            // shared.
            AdvanceRevision();
            failed = state_;
        }
        report_visual_worker_failure(diagnostics_, VisualSystemKind::Annotation, settings_.device, detail);
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
    bool terminal_barrier_ = false;
    detail::VisualRuntimeOwner worker_;
};

// CLEANUP-IGNORE: The public Annotation facade forwards construction to its one private implementation owner.
AnnotationSystem::AnnotationSystem(const VisualDeviceSettings settings, VisualRuntimeFactory factory,
                                   ExactVisualDocumentBorrower borrow_source, SystemEventSink<event_type> events,
                                   const VisualDiagnosticSink diagnostics)
    : impl_(std::make_unique<Impl>(settings, std::move(factory), std::move(borrow_source), std::move(events), diagnostics)) {}
AnnotationSystem::~AnnotationSystem() = default;
AnnotationSnapshot AnnotationSystem::Open(const AnnotationOpen request) { return impl_->Open(request); }
void AnnotationSystem::Pointer(const AnnotationPointer pointer) { impl_->Pointer(pointer); }
AnnotationSnapshot AnnotationSystem::Edit(AnnotationEditRequest request) { return impl_->Edit(std::move(request)); }
AnnotationSnapshot AnnotationSystem::Save(AnnotationSave request) { return impl_->Save(std::move(request)); }
AnnotationSnapshot AnnotationSystem::Stop() noexcept { return impl_->Stop(); }
void AnnotationSystem::PeerClosed() noexcept { impl_->PeerClosed(); }
void AnnotationSystem::Shutdown() noexcept { impl_->Shutdown(); }
bool AnnotationSystem::stopped() const noexcept { return impl_->stopped(); }
AnnotationSnapshot AnnotationSystem::snapshot() const { return impl_->snapshot(); }
mmltk::frameworks::gpu::BorrowedImageProductReadView AnnotationSystem::BorrowFrame() const { return impl_->BorrowFrame(); }

}  // namespace mmltk::controller
