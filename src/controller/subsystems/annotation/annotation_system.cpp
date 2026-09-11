#include "src/controller/subsystems/annotation/annotation_system.h"
#include "src/controller/subsystems/annotation/detail/annotation_document.h"
#include "src/controller/subsystems/annotation/detail/annotation_render_state.h"
#include "src/controller/presentation/detail/visual_runtime_owner.h"
#include "src/frameworks/gpu/system_image_worker.h"

#include <algorithm>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "src/common/types/generation.h"

namespace mmltk::controller {
namespace {
namespace document = subsystems::annotation;
using Runtime = mmltk::frameworks::gpu::SystemImageRuntime;

[[nodiscard]] AnnotationAlgorithm& annotation_algorithm(Runtime& runtime) {
    auto* algorithm = dynamic_cast<AnnotationAlgorithm*>(runtime.model());
    if (!algorithm) throw std::runtime_error("Annotation renderer is unavailable");
    return *algorithm;
}
}  // namespace

class AnnotationSystem::Impl final {
    friend class AnnotationSystem;
    struct InputSlot final {
        AnnotationInputBatch batch;
        std::size_t next = 0U;
        bool occupied = false;
    };
    using Command = std::variant<AnnotationOpen, AnnotationEditRequest, AnnotationSave>;
    using Completion = std::move_only_function<void()>;

   public:
    Impl(const VisualDeviceSettings settings, VisualRuntimeFactory factory, ExactVisualDocumentBorrower borrow_source,
         SystemEventSink<event_type> events, const VisualDiagnosticSink diagnostics)
        : settings_(settings), borrow_source_(std::move(borrow_source)), events_(std::move(events)), diagnostics_(diagnostics),
          renderer_(std::move(factory), [this](std::exception_ptr failure) { RendererFailed(failure); }),
          input_worker_([this](std::stop_token stop) { Reduce(stop); }, [this](std::exception_ptr failure) { Failed(failure); },
                        [this] { if (input_policy_) { input_policy_->Restore(); input_policy_.reset(); } }) {
        if (!settings_.valid() || !borrow_source_) throw contracts::InvalidIntentError("Annotation device settings are invalid");
        renderer_.RegisterContinuation([this](Runtime& runtime, std::stop_token stop) {
            RenderPending(runtime, stop);
            return detail::VisualRuntimeOwner::Notification{};
        }, {}, true, detail::VisualRuntimeOwner::ContinuationCancellation::PreserveOrderedInput);
    }
    ~Impl() { Shutdown(); }

    [[nodiscard]] AnnotationSnapshot Open(AnnotationOpen request) {
        if (!request.source.valid()) throw contracts::InvalidIntentError("Annotation source image is unavailable");
        return Admit(Command{std::move(request)}, false);
    }
    [[nodiscard]] AnnotationSnapshot Edit(AnnotationEditRequest request) { return Admit(Command{std::move(request)}, true); }
    [[nodiscard]] AnnotationSnapshot Save(AnnotationSave request) {
        if (request.destination.empty()) throw contracts::InvalidIntentError("Annotation save destination is unavailable");
        return Admit(Command{std::move(request)}, true);
    }
    void SetInputPeer(std::uint64_t epoch, SystemEventSink<AnnotationInputProgress> progress) {
        {
            std::scoped_lock lock(mutex_);
            if (input_epoch_ && input_epoch_ != epoch && !terminal_barrier_) {
                terminal_barrier_ = true;
                command_before_terminal_ = command_.has_value();
            }
            input_epoch_ = epoch;
            admitted_sequence_ = consumed_sequence_ = 0U;
            input_progress_ = std::move(progress);
            peer_ready_pending_ = true;
        }
        input_worker_.Wake();
    }
    void Input(AnnotationInputBatch batch) {
        {
            std::scoped_lock lock(mutex_);
            RequireReady();
            if (state_.busy) throw contracts::InvalidIntentError("Annotation command barrier is unsettled");
            if (terminal_barrier_) throw contracts::UnavailableError("Annotation input is resetting");
            if (!batch.epoch || batch.epoch != input_epoch_ || batch.document_epoch != state_.input_document_epoch ||
                !batch.sequence || batch.sequence != admitted_sequence_ + 1U || batch.samples.empty())
                throw contracts::InvalidIntentError("Annotation input epoch or sequence is invalid");
            for (const auto& pointer : batch.samples) {
                if (!pointer.valid() || pointer.point.x < 0 || pointer.point.y < 0 ||
                    pointer.point.x > state_.ui.scene.frame_width || pointer.point.y > state_.ui.scene.frame_height)
                    throw contracts::InvalidIntentError("Annotation batch contains invalid pointer input");
            }
            auto slot = std::ranges::find(input_slots_, false, &InputSlot::occupied);
            if (slot == input_slots_.end()) throw contracts::InvalidIntentError("Annotation input exceeded its two consumption credits");
            admitted_sequence_ = batch.sequence;
            slot->batch = std::move(batch);
            slot->next = 0U;
            slot->occupied = true;
        }
        input_worker_.Wake();
    }
    void PeerClosed() noexcept {
        {
            std::scoped_lock lock(mutex_);
            if (!terminal_barrier_) {
                terminal_barrier_ = true;
                command_before_terminal_ = command_.has_value();
            }
            input_progress_ = {};
        }
        input_worker_.Wake();
    }
    [[nodiscard]] AnnotationSnapshot Stop() noexcept {
        AnnotationSnapshot snapshot;
        {
            std::scoped_lock lock(mutex_);
            if (state_.busy && !state_.cancellation_requested) {
                state_.cancellation_requested = true;
                AdvanceUi();
            }
            snapshot = state_;
        }
        input_worker_.Wake();
        return snapshot;
    }
    void Shutdown() noexcept {
        {
            std::scoped_lock lock(mutex_);
            stopping_ = true;
        }
        input_worker_.RequestStop();
        renderer_.RequestStop();
        // GPU completions may still post to the input mailbox until the renderer
        // has joined. All mailbox/document/resource storage outlives both workers.
        renderer_.StopAndWait();
        input_worker_.WaitStopped();
    }
    [[nodiscard]] bool stopped() const noexcept { return input_worker_.stopped() && renderer_.stopped(); }
    [[nodiscard]] AnnotationSnapshot snapshot() const { std::scoped_lock lock(mutex_); return state_; }
    [[nodiscard]] VisualSourceObservation ObserveSource() const { std::scoped_lock lock(mutex_); return visual_source::Observe(state_); }
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const {
        VisualFrame committed;
        { std::scoped_lock lock(mutex_); committed = state_.frame; }
        return borrow_matching_visual_product(committed, renderer_);
    }

   private:
    [[nodiscard]] AnnotationSnapshot Admit(Command command, bool needs_document) {
        AnnotationSnapshot admitted;
        {
            std::scoped_lock lock(mutex_);
            if (stopping_) throw contracts::UnavailableError("Annotation input worker is unavailable");
            if (state_.busy) throw contracts::BusyError("Annotation is busy");
            if (needs_document) RequireReady();
            command_ = std::move(command);
            state_.busy = true;
            state_.cancellation_requested = false;
            AdvanceUi();
            admitted = state_;
        }
        input_worker_.Wake();
        return admitted;
    }
    void RequireReady() const {
        if (stopping_) throw contracts::UnavailableError("Annotation input worker is unavailable");
        if (!state_.ready) throw contracts::UnavailableError("Annotation document is unavailable");
    }
    void AdvanceUi() {
        state_.revision = mmltk::common::types::advance_monotonic_identity(state_.revision);
        state_.ui_revision = state_.revision;
    }
    void Post(Completion completion) {
        { std::scoped_lock lock(mutex_); completion_ = std::move(completion); }
        input_worker_.Wake();
    }
    void Reduce(std::stop_token stop) {
        while (!stop.stop_requested()) {
            Completion completion;
            std::optional<Command> command;
            InputSlot* slot = nullptr;
            bool close = false;
            {
                std::scoped_lock lock(mutex_);
                if (stopping_) return;
                if (completion_) completion = std::move(completion_);
                else if (gpu_continuation_) return;
                else {
                    // The oldest occupied slot may belong to the peer being
                    // retired. Finish its accepted samples before gesture cleanup.
                    for (auto& candidate : input_slots_)
                        if (candidate.occupied && (!slot || candidate.batch.sequence < slot->batch.sequence)) slot = &candidate;
                    if (!slot && terminal_barrier_ && !command_before_terminal_) close = true;
                    else if (!slot && command_) {
                        command = std::move(command_); command_.reset(); command_before_terminal_ = false;
                    } else if (!slot && terminal_barrier_) close = true;
                }
            }
            if (completion) { completion(); continue; }
            if (slot) { ReduceBatch(*slot); continue; }
            if (command) { Execute(std::move(*command)); continue; }
            if (close) {
                const bool preview = document_.PeerClosed();
                if (preview) QueueRender();
                { std::scoped_lock lock(mutex_); terminal_barrier_ = false; }
                continue;
            }
            InputReady();
            return;
        }
    }
    void ReduceBatch(InputSlot& slot) {
        bool dirty = false;
        bool ui_changed = false;
        while (slot.next < slot.batch.samples.size()) {
            const auto pointer = slot.batch.samples[slot.next++];
            const auto prior = document_.ui().scene_revision;
            auto result = document_.Pointer(pointer);
            dirty = dirty || result.render_changed;
            ui_changed = ui_changed || prior != document_.ui().scene_revision;
            const bool sample = result.outcome == document::DocumentOutcome::Applied &&
                pointer.phase == contracts::AnnotationPointerPhase::End && document_.ui().editor.tool == contracts::AnnotationTool::ColorSample &&
                document_.ToolAvailable(contracts::AnnotationTool::ColorSample, pointer.target.object);
            if (result.outcome != document::DocumentOutcome::Applied) Reject(std::move(result.detail), false);
            if (sample) {
                if (ui_changed) InstallUi(false);
                if (dirty) QueueRender();
                if (slot.next == slot.batch.samples.size()) Consumed(slot);
                Sample(pointer);
                return;
            }
        }
        // Credit release is independent of snapshot copying, output admission,
        // the renderer's stream, and all external consumers.
        Consumed(slot);
        if (ui_changed) InstallUi(false);
        if (dirty) QueueRender();
    }
    void Consumed(InputSlot& slot) {
        AnnotationInputProgress value;
        SystemEventSink<AnnotationInputProgress> progress;
        {
            std::scoped_lock lock(mutex_);
            value = {slot.batch.epoch, slot.batch.sequence};
            slot.occupied = false;
            if (value.epoch == input_epoch_) { consumed_sequence_ = value.consumed_sequence; progress = input_progress_; }
        }
        if (progress) progress(value);
    }
    void InputReady() {
        AnnotationInputProgress value;
        SystemEventSink<AnnotationInputProgress> progress;
        {
            std::scoped_lock lock(mutex_);
            if (terminal_barrier_ || stopping_ || !peer_ready_pending_) return;
            peer_ready_pending_ = false;
            value = {input_epoch_, consumed_sequence_};
            progress = input_progress_;
        }
        if (progress) progress(value);
    }
    [[nodiscard]] bool CancelRequested() const { std::scoped_lock lock(mutex_); return state_.cancellation_requested || stopping_; }
    void Execute(Command command) {
        if (CancelRequested()) { InstallUi(true); return; }
        std::visit([this](auto request) {
            using Request = decltype(request);
            if constexpr (std::same_as<Request, AnnotationOpen>) OpenSource(request);
            else {
                const auto result = [&] {
                    if constexpr (std::same_as<Request, AnnotationSave>) return document_.Save(request.destination);
                    else return document_.Edit(request.edit);
                }();
                if (result.outcome == document::DocumentOutcome::Applied) InstallUi(true);
                else Reject(result.detail, true);
                if (result.render_changed) QueueRender();
                if constexpr (std::same_as<Request, AnnotationEditRequest>) {
                    if (result.outcome == document::DocumentOutcome::Applied)
                        diagnostics_.Emit([&] { return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Annotation,
                            .operation = VisualDiagnosticOperation::DocumentEdited, .device = settings_.device,
                            .generation = document_.ui().document_revision}; });
                }
            }
        }, std::move(command));
    }
    void OpenSource(AnnotationOpen request) {
        // Source borrowing and materialization can wait on another producer and
        // therefore belong to GPU preparation, never to the receive callback.
        { std::scoped_lock lock(mutex_); gpu_continuation_ = true; }
        if (!renderer_.SubmitDiscrete([this, request](Runtime& runtime, std::stop_token stop) -> detail::VisualRuntimeOwner::Notification {
            if (stop.stop_requested() || CancelRequested()) return [this] { Post([this] { FinishCancelled(); }); };
            auto source = borrow_source_(request.source);
            if (!source.valid()) return [this] { Post([this] { FinishRejected("Annotation source image is unavailable"); }); };
            const auto descriptor = source.pixels.plane(0U).plane().descriptor;
            if (descriptor.width > settings_.maximum_width || descriptor.height > settings_.maximum_height)
                return [this] { Post([this] { FinishRejected("Annotation source exceeds the configured device bounds"); }); };
            const auto crop = request.original_content ? request.source.content : VisualRegion{};
            contracts::AnnotationSceneContent scene;
            try { scene = materialize_visual_document(*source.document, request.source.extent, crop); }
            catch (const contracts::InvalidIntentError& error) {
                return [this, detail = std::string(error.what())] { Post([this, detail] { FinishRejected(detail); }); };
            }
            if (stop.stop_requested() || CancelRequested()) return [this] { Post([this] { FinishCancelled(); }); };
            {
                std::scoped_lock lock(render_mutex_);
                pending_render_ = false;
            }
            pending_baseline_ = {};
            renderer_.SetOutputRetry(false);
            const auto paths = runtime.CopyInputFrom(std::move(source.pixels));
            diagnostics_.Emit([&] { return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Annotation,
                .operation = VisualDiagnosticOperation::CopyCompleted, .device = settings_.device, .copy_path = paths[0U]}; });
            const auto input = runtime.BorrowInput();
            annotation_algorithm(runtime).Open(input.plane(0U).plane(), crop);
            std::optional<mmltk::common::system::ExecutionPolicyRequest> policy;
            if (const auto* execution = runtime.execution())
                policy = mmltk::common::system::ExecutionPolicyRequest{execution->placement.cpus, "annot-input", 0U,
                                                                     execution->placement.numa_node, -10, false};
            // Copy completion has committed receiver storage. A later Stop must
            // not leave that source paired with the old editable document.
            return [this, scene = std::move(scene), policy = std::move(policy)]() mutable {
                Post([this, scene = std::move(scene), policy = std::move(policy)]() mutable {
                    if (policy && !input_policy_) input_policy_.emplace(*policy);
                    auto result = document_.Open(std::move(scene));
                    if (result.outcome != document::DocumentOutcome::Applied) throw std::runtime_error(result.detail);
                    {
                        std::scoped_lock lock(mutex_);
                        gpu_continuation_ = false;
                        state_.ready = true;
                        state_.input_document_epoch = mmltk::common::types::advance_monotonic_identity(state_.input_document_epoch);
                    }
                    InstallUi(true);
                    QueueRender();
                    diagnostics_.Emit([&] { return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Annotation,
                        .operation = VisualDiagnosticOperation::DocumentOpened, .device = settings_.device,
                        .generation = document_.ui().document_revision}; });
                });
            };
        }, [this] { Post([this] { FinishCancelled(); }); })) FinishRejected("Annotation renderer is unavailable");
    }
    void FinishCancelled() { { std::scoped_lock lock(mutex_); gpu_continuation_ = false; } InstallUi(true); }
    void FinishRejected(std::string detail) { { std::scoped_lock lock(mutex_); gpu_continuation_ = false; } Reject(std::move(detail), true); }
    void Sample(const AnnotationPointer pointer) {
        { std::scoped_lock lock(mutex_); gpu_continuation_ = true; }
        if (!renderer_.SubmitOrdered([this, pointer](Runtime& runtime, std::stop_token) -> detail::VisualRuntimeOwner::Notification {
            const auto color = annotation_algorithm(runtime).Sample(pointer.point);
            return [this, pointer, color] { Post([this, pointer, color] {
                const auto object = document_.ui().scene.objects.at(*pointer.target.object);
                auto supported = object.sup;
                supported.center = color;
                supported.sampling = true;
                auto selected = document_.Edit({.value = AnnotationObjectEdit{*pointer.target.object}});
                const bool selection_changed = selected.render_changed;
                auto result = selected.outcome == document::DocumentOutcome::Applied
                    ? document_.Edit({.value = AnnotationMaskColorsEdit{supported, object.nosup}}) : std::move(selected);
                result.render_changed = result.render_changed || selection_changed;
                { std::scoped_lock lock(mutex_); gpu_continuation_ = false; }
                if (result.outcome != document::DocumentOutcome::Applied) Reject(std::move(result.detail), false);
                else InstallUi(false);
                if (result.render_changed) QueueRender();
            }); };
        })) throw contracts::UnavailableError("Annotation color sampling is unavailable");
    }
    void InstallUi(bool settle) {
        AnnotationSnapshot installed;
        {
            std::scoped_lock lock(mutex_);
            state_.ui = document_.ui();
            if (settle) { state_.busy = false; state_.cancellation_requested = false; }
            AdvanceUi();
            installed = state_;
        }
        Publish(AnnotationChanged{std::move(installed)});
    }
    void QueueRender() {
        document_.CaptureRender(scratch_render_);
        scratch_render_.generation = render_generation_ = mmltk::common::types::advance_monotonic_identity(render_generation_);
        { std::scoped_lock lock(mutex_); scratch_render_.document_epoch = state_.input_document_epoch; }
        {
            std::scoped_lock lock(render_mutex_);
            std::swap(scratch_render_, pending_description_);
            pending_render_ = true;
        }
        if (!renderer_.NotifyContinuation()) throw contracts::UnavailableError("Annotation renderer is unavailable");
    }
    void RenderPending(Runtime& runtime, std::stop_token stop) {
        if (stop.stop_requested()) return;
        {
            std::scoped_lock lock(render_mutex_);
            if (!pending_render_) { renderer_.SetOutputRetry(false); return; }
        }
        if (!pending_baseline_.valid()) pending_baseline_ = runtime.Completed();
        renderer_.SetOutputRetry(true);
        auto output = runtime.TryAcquireOutput(pending_baseline_, mmltk::frameworks::gpu::ImagePlanePreservation::Clean);
        if (!output.valid()) return;
        renderer_.SetOutputRetry(false);
        {
            std::scoped_lock lock(render_mutex_);
            if (!pending_render_) return;
            std::swap(active_description_, pending_description_);
            pending_render_ = false;
        }
        const auto& description = active_description_;
        const VisualExtent extent{description.scene->frame_width, description.scene->frame_height};
        const bool fresh_source = clean_epoch_ != description.document_epoch;
        const auto input = fresh_source ? runtime.BorrowInput() : mmltk::frameworks::gpu::BorrowedImageProductReadView{};
        runtime.Publish(output, extent.width, extent.height, [&](auto clean, auto semantic, auto stream) {
            annotation_algorithm(runtime).Render(description, fresh_source ? input.plane(0U).plane() : mmltk::frameworks::gpu::ImagePlaneView{},
                                                  clean, semantic, stream);
        });
        runtime.CommitOutput(std::move(output));
        if (fresh_source) { clean_epoch_ = description.document_epoch; clean_revision_ = runtime.OutputFacts().revision; }
        auto frame = visual_frame({PresentationSourceKind::Annotation, 1U}, extent, runtime.OutputFacts().revision);
        frame.clean_revision = clean_revision_;
        std::optional<AnnotationSnapshot> changed;
        AnnotationFrameState rendered;
        {
            std::scoped_lock lock(mutex_);
            // A completed older render remains publishable; logical UI stays at
            // the latest reduction while rendered facts identify these exact pixels.
            if (!state_.ready) return;
            state_.frame = frame;
            const AnnotationRenderedFacts facts{description.generation, description.document_epoch, description.scene_revision};
            const bool facts_changed = state_.rendered.scene_revision != facts.scene_revision ||
                                       state_.rendered.document_epoch != facts.document_epoch;
            state_.rendered = facts;
            state_.revision = mmltk::common::types::advance_monotonic_identity(state_.revision);
            // UI facts are already present for preview-only frames. Preserve
            // the compact progress path instead of resending masks per motion.
            if (facts_changed) changed = state_;
            rendered = {state_.revision, state_.ui_revision, frame, state_.rendered};
        }
        if (changed) Publish(AnnotationChanged{std::move(*changed)});
        Publish(AnnotationFrameChanged{rendered});
    }
    void Reject(std::string detail, bool settle) {
        AnnotationSnapshot failed;
        {
            std::scoped_lock lock(mutex_);
            state_.ui = document_.ui();
            if (settle) { state_.busy = false; state_.cancellation_requested = false; }
            AdvanceUi(); failed = state_;
        }
        Publish(AnnotationFailed{std::move(failed), std::move(detail)});
    }
    void RendererFailed(std::exception_ptr failure) noexcept {
        pending_baseline_ = {};
        clean_epoch_ = clean_revision_ = 0U;
        { std::scoped_lock lock(render_mutex_); pending_render_ = false; }
        try { Post([this, failure] { Failed(failure); }); } catch (...) { std::terminate(); }
    }
    void Failed(std::exception_ptr failure) noexcept {
        auto detail = visual_failure_detail(failure, "Annotation worker failed");
        AnnotationSnapshot failed;
        SystemEventSink<AnnotationInputProgress> progress;
        AnnotationInputProgress rejected;
        document_.PeerClosed();
        {
            std::scoped_lock lock(mutex_);
            if (std::ranges::any_of(input_slots_, [this](const auto& slot) { return slot.occupied && slot.batch.epoch == input_epoch_; }))
                progress = input_progress_;
            rejected = {input_epoch_, consumed_sequence_, detail};
            for (auto& slot : input_slots_) slot.occupied = false;
            command_.reset(); gpu_continuation_ = false; terminal_barrier_ = false;
            state_.ready = false; state_.frame = {}; state_.rendered = {};
            state_.busy = state_.cancellation_requested = false;
            state_.ui = document_.ui();
            AdvanceUi(); failed = state_;
        }
        report_visual_worker_failure(diagnostics_, contracts::DiagnosticOwner::Annotation, settings_.device, detail);
        if (progress) progress(std::move(rejected));
        Publish(AnnotationFailed{std::move(failed), std::move(detail)});
    }
    template<class Event> void Publish(Event event) noexcept { publish_visual_event_noexcept(events_, event_type{std::move(event)}); }

    VisualDeviceSettings settings_;
    ExactVisualDocumentBorrower borrow_source_;
    SystemEventSink<event_type> events_;
    VisualDiagnosticSink diagnostics_;
    mutable std::mutex mutex_;
    AnnotationSnapshot state_;
    std::array<InputSlot, kAnnotationInputAdmissionSlots> input_slots_{};
    std::optional<Command> command_;
    Completion completion_;
    std::uint64_t input_epoch_ = 0U, admitted_sequence_ = 0U, consumed_sequence_ = 0U;
    SystemEventSink<AnnotationInputProgress> input_progress_;
    bool terminal_barrier_ = false, gpu_continuation_ = false, stopping_ = false, peer_ready_pending_ = false;
    bool command_before_terminal_ = false;
    document::AnnotationDocument document_;
    std::optional<mmltk::common::system::ScopedExecutionPolicy> input_policy_;
    std::uint64_t render_generation_ = 0U;
    AnnotationRenderState scratch_render_, pending_description_, active_description_;
    std::mutex render_mutex_;
    bool pending_render_ = false;
    Runtime::CompletedOutput pending_baseline_;
    std::uint64_t clean_epoch_ = 0U, clean_revision_ = 0U;
    detail::VisualRuntimeOwner renderer_;
    mmltk::frameworks::gpu::SystemImageWorker input_worker_;
};

AnnotationSystem::AnnotationSystem(VisualDeviceSettings settings, VisualRuntimeFactory factory, ExactVisualDocumentBorrower borrow_source,
                                   SystemEventSink<event_type> events, VisualDiagnosticSink diagnostics)
    : impl_(std::make_unique<Impl>(settings, std::move(factory), std::move(borrow_source), std::move(events), diagnostics)) {}
AnnotationSystem::~AnnotationSystem() = default;
AnnotationSnapshot AnnotationSystem::Open(AnnotationOpen request) { return impl_->Open(std::move(request)); }
void AnnotationSystem::Input(AnnotationInputBatch batch) { impl_->Input(std::move(batch)); }
void AnnotationSystem::SetInputPeer(std::uint64_t epoch, SystemEventSink<AnnotationInputProgress> progress) { impl_->SetInputPeer(epoch, std::move(progress)); }
AnnotationSnapshot AnnotationSystem::Edit(AnnotationEditRequest request) { return impl_->Edit(std::move(request)); }
AnnotationSnapshot AnnotationSystem::Save(AnnotationSave request) { return impl_->Save(std::move(request)); }
AnnotationSnapshot AnnotationSystem::Stop() noexcept { return impl_->Stop(); }
void AnnotationSystem::PeerClosed() noexcept { impl_->PeerClosed(); }
void AnnotationSystem::Shutdown() noexcept { impl_->Shutdown(); }
bool AnnotationSystem::stopped() const noexcept { return impl_->stopped(); }
AnnotationSnapshot AnnotationSystem::snapshot() const { return impl_->snapshot(); }
VisualSourceObservation AnnotationSystem::ObserveSource() const { return impl_->ObserveSource(); }
mmltk::frameworks::gpu::BorrowedImageProductReadView AnnotationSystem::BorrowFrame() const { return impl_->BorrowFrame(); }
mmltk::frameworks::gpu::BorrowedImageWorkspace AnnotationSystem::BorrowWorkspace() const { return impl_->renderer_.BorrowWorkspace(); }
mmltk::frameworks::gpu::ImageWorkspaceObservation AnnotationSystem::ObserveWorkspace() const { return impl_->renderer_.ObserveWorkspace(); }
void AnnotationSystem::RequestWorkspace(VisualWorkspaceRequest request) { impl_->renderer_.RequestWorkspace(std::move(request)); }
}  // namespace mmltk::controller
