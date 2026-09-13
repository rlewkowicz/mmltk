#include "src/controller/presentation/workspace_input.h"
#include "src/controller/subsystems/annotation/annotation_system.h"
#include "src/controller/subsystems/annotation/detail/annotation_document.h"
#include "src/controller/subsystems/annotation/detail/annotation_render_state.h"
#include "src/controller/presentation/detail/visual_runtime_owner.h"
#include "src/frameworks/gpu/system_image_worker.h"

#include <algorithm>
#include <functional>
#include <limits>
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
    using Command = std::variant<AnnotationOpen, AnnotationEditRequest, AnnotationSave>;
    struct PeerBoundary final {};
    using InputRecord = std::variant<WorkspaceMouse, Command, PeerBoundary>;
    using Completion = std::move_only_function<void()>;

   public:
    Impl(const VisualDeviceSettings settings, VisualRuntimeFactory factory, ExactVisualDocumentBorrower borrow_source,
         SystemEventSink<event_type> events, const VisualDiagnosticSink diagnostics)
        : settings_(settings),
          borrow_source_(std::move(borrow_source)),
          events_(std::move(events)),
          diagnostics_(diagnostics),
          renderer_(std::move(factory), [this](std::exception_ptr failure) { RendererFailed(failure); }),
          input_worker_([this](std::stop_token stop) { Reduce(stop); }, [this](std::exception_ptr failure) { Failed(failure); },
                        [this] {
                            if (input_policy_) {
                                input_policy_->Restore();
                                input_policy_.reset();
                            }
                        }) {
        if (!settings_.valid() || !borrow_source_) throw contracts::InvalidIntentError("Annotation device settings are invalid");
        renderer_.RegisterContinuation(
            [this](Runtime& runtime, std::stop_token stop) {
                RenderPending(runtime, stop);
                return detail::VisualRuntimeOwner::Notification{};
            },
            {}, true, detail::VisualRuntimeOwner::ContinuationCancellation::PreserveOrderedInput);
    }
    ~Impl() { Shutdown(); }

    [[nodiscard]] AnnotationSnapshot Open(AnnotationOpen request) {
        if (!request.source.valid()) throw contracts::InvalidIntentError("Annotation source image is unavailable");
        return Admit(Command{std::move(request)});
    }
    [[nodiscard]] AnnotationSnapshot Edit(AnnotationEditRequest request) { return Admit(Command{std::move(request)}); }
    [[nodiscard]] AnnotationSnapshot Save(AnnotationSave request) {
        if (request.destination.empty()) throw contracts::InvalidIntentError("Annotation save destination is unavailable");
        return Admit(Command{std::move(request)});
    }
    void SetInputPeer(std::uint64_t epoch) {
        {
            std::scoped_lock lock(mutex_);
            if (input_epoch_ != epoch) input_.Push(PeerBoundary{});
            input_epoch_ = epoch;
        }
        input_worker_.Wake();
    }
    void Input(WorkspaceMouse mouse) {
        {
            std::scoped_lock lock(mutex_);
            if (stopping_) throw contracts::UnavailableError("Annotation input worker is unavailable");
            if (!mouse.valid() || mouse.source != PresentationSourceKind::Annotation || mouse.peer_epoch != input_epoch_)
                throw contracts::InvalidIntentError("Annotation mouse ownership is invalid");
            input_.Push(std::move(mouse));
        }
        input_worker_.Wake();
    }
    void PeerClosed() noexcept {
        try { SetInputPeer(0U); }
        catch (...) {
            {
                std::scoped_lock lock(mutex_);
                stopping_ = true;
            }
            input_worker_.RequestStop();
            renderer_.RequestStop();
        }
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
    [[nodiscard]] std::optional<AnnotationImageMetadata> ImageSnapshot(const VisualFrame& frame) const {
        std::scoped_lock lock(mutex_);
        if (state_.frame != frame) return std::nullopt;
        return image_;
    }
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
            committed = state_.frame;
        }
        return borrow_matching_visual_product(committed, renderer_);
    }

   private:
    [[nodiscard]] AnnotationSnapshot Admit(Command command) {
        AnnotationSnapshot admitted;
        {
            std::scoped_lock lock(mutex_);
            if (stopping_) throw contracts::UnavailableError("Annotation input worker is unavailable");
            input_.Push(std::move(command));
            ++pending_commands_;
            if (!state_.busy) state_.cancellation_requested = false;
            state_.busy = true;
            AdvanceUi();
            admitted = state_;
        }
        input_worker_.Wake();
        return admitted;
    }
    void AdvanceUi() {
        state_.revision = mmltk::common::types::advance_monotonic_identity(state_.revision);
        state_.ui_revision = state_.revision;
    }
    void Post(Completion completion) {
        {
            std::scoped_lock lock(mutex_);
            completion_ = std::move(completion);
        }
        input_worker_.Wake();
    }
    void Reduce(std::stop_token stop) {
        while (!stop.stop_requested()) {
            Completion completion;
            std::optional<InputRecord> record;
            {
                std::scoped_lock lock(mutex_);
                if (stopping_) return;
                if (completion_) completion = std::move(completion_);
                else if (gpu_continuation_) return;
                else record = input_.Pop();
            }
            if (completion) { completion(); continue; }
            if (!record) return;
            std::visit([this](auto value) {
                using Value = decltype(value);
                if constexpr (std::same_as<Value, WorkspaceMouse>) ReduceMouse(value);
                else if constexpr (std::same_as<Value, Command>) Execute(std::move(value));
                else {
                    const bool preview_changed = CancelGesture();
                    bool ready;
                    {
                        std::scoped_lock lock(mutex_);
                        ready = state_.ready;
                    }
                    if (preview_changed && ready) QueueRender();
                }
            }, std::move(*record));
        }
    }
    [[nodiscard]] bool CancelGesture() noexcept {
        pointer_.reset();
        right_pressed_ = false;
        return document_.PeerClosed();
    }
    void ReduceMouse(const WorkspaceMouse& mouse) {
        bool stale, ready;
        {
            std::scoped_lock lock(mutex_);
            stale = mouse.document_epoch != state_.input_document_epoch;
            ready = state_.ready;
        }
        if (stale || mouse.kind == WorkspaceMouseKind::Cancel) {
            // A retained record can outlive its document while Open completes.
            // Consume it as cancellation, without changing replacement UI facts.
            if (CancelGesture() && ready) QueueRender();
            return;
        }
        if (mouse.button == WorkspaceMouseButton::Right) {
            if (mouse.kind == WorkspaceMouseKind::Press) right_pressed_ = true;
            if (mouse.kind == WorkspaceMouseKind::Release) right_pressed_ = false;
        }
        // Right-button movement belongs to the local viewport pan. Its native
        // record is consumed without moving an overlapping left-button edit.
        if (mouse.kind == WorkspaceMouseKind::Motion && right_pressed_) return;
        const bool begin = mouse.kind == WorkspaceMouseKind::Press && mouse.button == WorkspaceMouseButton::Left;
        const bool end = mouse.kind == WorkspaceMouseKind::Release && mouse.button == WorkspaceMouseButton::Left;
        if (!begin && !end && mouse.kind != WorkspaceMouseKind::Motion) return;
        if (begin) {
            if (!mouse.point || !document_.ui().scene.frame_ready) {
                Reject("Annotation image coordinates are unavailable", false);
                return;
            }
            if (document_.PeerClosed()) QueueRender();
            pointer_.emplace();
            pointer_->interaction_id = next_interaction_ = mmltk::common::types::advance_monotonic_identity(next_interaction_);
            pointer_->sequence = 1U;
            pointer_->phase = contracts::AnnotationPointerPhase::Begin;
        } else {
            if (!pointer_) return;
            ++pointer_->sequence;
            pointer_->phase = end ? contracts::AnnotationPointerPhase::End : contracts::AnnotationPointerPhase::Update;
        }
        if (mouse.point) pointer_->point = {mouse.point->x, mouse.point->y};
        pointer_->brush_radius = mouse.brush_radius;
        auto pointer = *pointer_;
        if (!document_.ResolveTarget(pointer)) {
            if (CancelGesture() && ready) QueueRender();
            Reject("Annotation gesture target is unavailable", false);
            return;
        }
        pointer_ = pointer;
        const auto prior = document_.ui().scene_revision;
        auto result = document_.Pointer(pointer);
        if (end) pointer_.reset();
        if (result.outcome != document::DocumentOutcome::Applied) Reject(std::move(result.detail), false);
        if (prior != document_.ui().scene_revision) InstallUi(false);
        if (result.render_changed) QueueRender();
        if (result.outcome == document::DocumentOutcome::Applied && end &&
            document_.ui().editor.tool == contracts::AnnotationTool::ColorSample &&
            document_.ToolAvailable(contracts::AnnotationTool::ColorSample, pointer.target.object)) Sample(pointer);
    }
    [[nodiscard]] bool CancelRequested() const {
        std::scoped_lock lock(mutex_);
        return state_.cancellation_requested || stopping_;
    }
    void Execute(Command command) {
        if (CancelRequested()) {
            InstallUi(true);
            return;
        }
        std::visit(
            [this](auto request) {
                using Request = decltype(request);
                if constexpr (std::same_as<Request, AnnotationOpen>)
                    OpenSource(request);
                else {
                    const auto result = [&] {
                        if constexpr (std::same_as<Request, AnnotationSave>)
                            return document_.Save(request.destination);
                        else {
                            pointer_.reset();
                            return document_.Edit(request.edit);
                        }
                    }();
                    if (result.outcome == document::DocumentOutcome::Applied)
                        InstallUi(true);
                    else
                        Reject(result.detail, true);
                    if (result.render_changed) QueueRender();
                    if constexpr (std::same_as<Request, AnnotationEditRequest>) {
                        if (result.outcome == document::DocumentOutcome::Applied)
                            diagnostics_.Emit([&] {
                                return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Annotation,
                                                            .operation = VisualDiagnosticOperation::DocumentEdited,
                                                            .device = settings_.device,
                                                            .generation = document_.ui().document_revision};
                            });
                    }
                }
            },
            std::move(command));
    }
    void OpenSource(AnnotationOpen request) {
        // Source borrowing and materialization can wait on another producer and
        // therefore belong to GPU preparation, never to the receive callback.
        {
            std::scoped_lock lock(mutex_);
            gpu_continuation_ = true;
        }
        if (!renderer_.SubmitDiscrete(
                [this, request](Runtime& runtime, std::stop_token stop) -> detail::VisualRuntimeOwner::Notification {
                    if (stop.stop_requested() || CancelRequested()) return [this] { Post([this] { FinishCancelled(); }); };
                    auto source = borrow_source_(request.source);
                    if (!source.valid()) return [this] { Post([this] { FinishRejected("Annotation source image is unavailable"); }); };
                    const auto descriptor = source.pixels.plane(0U).plane().descriptor;
                    if (descriptor.width > settings_.maximum_width || descriptor.height > settings_.maximum_height)
                        return [this] { Post([this] { FinishRejected("Annotation source exceeds the configured device bounds"); }); };
                    const auto crop = request.original_content ? request.source.content : VisualRegion{};
                    contracts::AnnotationSceneContent scene;
                    try {
                        scene = materialize_visual_document(*source.document, request.source.extent, crop);
                    } catch (const contracts::InvalidIntentError& error) {
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
                    diagnostics_.Emit([&] {
                        return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Annotation,
                                                    .operation = VisualDiagnosticOperation::CopyCompleted,
                                                    .device = settings_.device,
                                                    .copy_path = paths[0U]};
                    });
                    const auto input = runtime.BorrowInput();
                    annotation_algorithm(runtime).Open(input.plane(0U).plane(), crop);
                    std::optional<mmltk::common::system::ExecutionPolicyRequest> policy;
                    if (const auto* execution = runtime.execution())
                        policy = mmltk::common::system::ExecutionPolicyRequest{execution->placement.cpus,      "annot-input", 0U,
                                                                               execution->placement.numa_node, -10,           false};
                    // Copy completion has committed receiver storage. A later Stop must
                    // not leave that source paired with the old editable document.
                    return [this, scene = std::move(scene), policy = std::move(policy)]() mutable {
                        Post([this, scene = std::move(scene), policy = std::move(policy)]() mutable {
                            if (policy && !input_policy_) input_policy_.emplace(*policy);
                            static_cast<void>(CancelGesture());
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
                            diagnostics_.Emit([&] {
                                return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Annotation,
                                                            .operation = VisualDiagnosticOperation::DocumentOpened,
                                                            .device = settings_.device,
                                                            .generation = document_.ui().document_revision};
                            });
                        });
                    };
                },
                [this] { Post([this] { FinishCancelled(); }); }))
            FinishRejected("Annotation renderer is unavailable");
    }
    void FinishCancelled() {
        {
            std::scoped_lock lock(mutex_);
            gpu_continuation_ = false;
        }
        InstallUi(true);
    }
    void FinishRejected(std::string detail) {
        {
            std::scoped_lock lock(mutex_);
            gpu_continuation_ = false;
        }
        Reject(std::move(detail), true);
    }
    void FinishSample(AnnotationPointer pointer, std::optional<contracts::AnnotationColor> color) {
        const bool cancelled = CancelRequested();
        {
            std::scoped_lock lock(mutex_);
            gpu_continuation_ = sampling_ = false;
            state_.busy = pending_commands_ != 0U || sampling_;
            state_.cancellation_requested = false;
        }
        if (cancelled || !color) { InstallUi(false); return; }
        if (!document_.ResolveTarget(pointer)) { Reject("The sampled annotation target no longer exists", false); return; }
        const auto object = document_.ui().scene.objects.at(*pointer.target.object);
        auto supported = object.sup;
        supported.center = *color;
        supported.sampling = true;
        auto selected = document_.Edit({.value = AnnotationObjectEdit{*pointer.target.object}});
        const bool selection_changed = selected.render_changed;
        auto result = selected.outcome == document::DocumentOutcome::Applied
                          ? document_.Edit({.value = AnnotationMaskColorsEdit{supported, object.nosup}})
                          : std::move(selected);
        result.render_changed = result.render_changed || selection_changed;
        if (result.outcome != document::DocumentOutcome::Applied) Reject(std::move(result.detail), false);
        else InstallUi(false);
        if (result.render_changed) QueueRender();
    }
    void Sample(AnnotationPointer pointer) {
        {
            std::scoped_lock lock(mutex_);
            gpu_continuation_ = sampling_ = true;
            state_.busy = true;
        }
        InstallUi(false);
        if (!renderer_.SubmitOrdered([this, pointer](Runtime& runtime, std::stop_token stop) -> detail::VisualRuntimeOwner::Notification {
                std::optional<contracts::AnnotationColor> color;
                if (!stop.stop_requested() && !CancelRequested()) color = annotation_algorithm(runtime).Sample(pointer.point);
                return [this, pointer, color] { Post([this, pointer, color] { FinishSample(pointer, color); }); };
            })) throw contracts::UnavailableError("Annotation color sampling is unavailable");
    }
    void InstallUi(bool settle) {
        AnnotationSnapshot installed;
        {
            std::scoped_lock lock(mutex_);
            state_.ui = document_.ui();
            if (settle) {
                if (pending_commands_) --pending_commands_;
                state_.busy = pending_commands_ != 0U || sampling_;
                state_.cancellation_requested = false;
            }
            AdvanceUi();
            installed = state_;
        }
        Publish(AnnotationChanged{std::move(installed)});
    }
    void DiagnoseRender(VisualDiagnosticOperation operation, const AnnotationRenderState& description,
                        const Runtime::CompletedOutput* baseline = nullptr, std::uint64_t revision = 0U) const noexcept {
        diagnostics_.Emit([&] {
            return VisualDiagnosticFact{
                .system = contracts::DiagnosticOwner::Annotation,
                .operation = operation,
                .device = settings_.device,
                .generation = description.generation,
                .value = description.scene_revision,
                .detail = description.document_epoch,
                .context = {.capacity_width = description.scene ? description.scene->frame_width : 0U,
                            .capacity_height = description.scene ? description.scene->frame_height : 0U,
                            .frame_revision = revision,
                            .condition = baseline && baseline->valid() ? 1U : 0U,
                            .source = {.source_session = presentation_source_session(PresentationSourceKind::Annotation),
                                       .source_instance = 1U,
                                       .source_revision = revision}}};
        });
    }
    void QueueRender() {
        document_.CaptureRender(scratch_render_);
        scratch_render_.generation = render_generation_ = mmltk::common::types::advance_monotonic_identity(render_generation_);
        {
            std::scoped_lock lock(mutex_);
            scratch_render_.document_epoch = state_.input_document_epoch;
        }
        {
            std::scoped_lock lock(render_mutex_);
            std::swap(scratch_render_, pending_description_);
            pending_render_ = true;
            DiagnoseRender(VisualDiagnosticOperation::AnnotationRenderQueued, pending_description_);
        }
        if (!renderer_.NotifyContinuation()) throw contracts::UnavailableError("Annotation renderer is unavailable");
    }
    void RenderPending(Runtime& runtime, std::stop_token stop) {
        if (stop.stop_requested()) return;
        {
            std::scoped_lock lock(render_mutex_);
            if (!pending_render_) {
                renderer_.SetOutputRetry(false);
                return;
            }
        }
        if (!pending_baseline_.valid()) pending_baseline_ = runtime.Completed();
        renderer_.SetOutputRetry(true);
        if (diagnostics_.valid()) {
            std::scoped_lock lock(render_mutex_);
            DiagnoseRender(VisualDiagnosticOperation::AnnotationOutputAcquireStarted, pending_description_, &pending_baseline_);
        }
        auto output = runtime.TryAcquireOutput(pending_baseline_, mmltk::frameworks::gpu::ImagePlanePreservation::Clean);
        if (!output.valid()) {
            if (diagnostics_.valid()) {
                std::scoped_lock lock(render_mutex_);
                DiagnoseRender(VisualDiagnosticOperation::AnnotationOutputUnavailable, pending_description_, &pending_baseline_);
            }
            return;
        }
        renderer_.SetOutputRetry(false);
        {
            std::scoped_lock lock(render_mutex_);
            if (!pending_render_) return;
            std::swap(active_description_, pending_description_);
            pending_render_ = false;
        }
        const auto& description = active_description_;
        DiagnoseRender(VisualDiagnosticOperation::AnnotationOutputAcquired, description, &pending_baseline_);
        const VisualExtent extent{description.scene->frame_width, description.scene->frame_height};
        const bool fresh_source = clean_epoch_ != description.document_epoch;
        const auto input = fresh_source ? runtime.BorrowInput() : mmltk::frameworks::gpu::BorrowedImageProductReadView{};
        runtime.Publish(output, extent.width, extent.height, [&](auto clean, auto semantic, auto stream) {
            annotation_algorithm(runtime).Render(
                description, fresh_source ? input.plane(0U).plane() : mmltk::frameworks::gpu::ImagePlaneView{}, clean, semantic, stream);
        });
        runtime.CommitOutput(std::move(output));
        if (fresh_source) {
            clean_epoch_ = description.document_epoch;
            clean_revision_ = runtime.OutputFacts().revision;
        }
        auto frame = visual_frame({PresentationSourceKind::Annotation, 1U}, extent, runtime.OutputFacts().revision);
        frame.clean_revision = clean_revision_;
        std::optional<AnnotationRenderedFacts> evidence;
        if (diagnostics_.valid())
            evidence = AnnotationRenderedFacts{description.generation, description.document_epoch,
                                               description.scene_revision, description.editor};
        AnnotationFrameState rendered;
        {
            std::scoped_lock lock(mutex_);
            if (!state_.ready) return;
            state_.frame = frame;
            image_ = {frame, std::move(evidence)};
            state_.revision = mmltk::common::types::advance_monotonic_identity(state_.revision);
            rendered = {state_.revision, state_.ui_revision, frame};
        }
        Publish(AnnotationFrameChanged{rendered});
        DiagnoseRender(VisualDiagnosticOperation::AnnotationRenderPublished, description, &pending_baseline_, frame.revision);
    }
    void Reject(std::string detail, bool settle) {
        AnnotationSnapshot failed;
        {
            std::scoped_lock lock(mutex_);
            state_.ui = document_.ui();
            if (settle) {
                if (pending_commands_) --pending_commands_;
                state_.busy = pending_commands_ != 0U || sampling_;
                state_.cancellation_requested = false;
            }
            AdvanceUi();
            failed = state_;
        }
        Publish(AnnotationFailed{std::move(failed), std::move(detail)});
    }
    void RendererFailed(std::exception_ptr failure) noexcept {
        pending_baseline_ = {};
        clean_epoch_ = clean_revision_ = 0U;
        {
            std::scoped_lock lock(render_mutex_);
            pending_render_ = false;
        }
        try {
            Post([this, failure] { Failed(failure); });
        } catch (...) { std::terminate(); }
    }
    void Failed(std::exception_ptr failure) noexcept {
        auto detail = visual_failure_detail(failure, "Annotation worker failed");
        AnnotationSnapshot failed;
        static_cast<void>(CancelGesture());
        {
            std::scoped_lock lock(mutex_);
            input_.Clear();
            pending_commands_ = 0U;
            gpu_continuation_ = sampling_ = false;
            state_.ready = false;
            state_.frame = {};
            image_.diagnostics.reset();
            state_.busy = state_.cancellation_requested = false;
            state_.ui = document_.ui();
            AdvanceUi();
            failed = state_;
        }
        report_visual_worker_failure(diagnostics_, contracts::DiagnosticOwner::Annotation, settings_.device, detail);
        Publish(AnnotationFailed{std::move(failed), std::move(detail)});
    }
    template <class Event>
    void Publish(Event event) noexcept {
        publish_visual_event_noexcept(events_, event_type{std::move(event)});
    }

    VisualDeviceSettings settings_;
    ExactVisualDocumentBorrower borrow_source_;
    SystemEventSink<event_type> events_;
    VisualDiagnosticSink diagnostics_;
    mutable std::mutex mutex_;
    AnnotationSnapshot state_;
    AnnotationImageMetadata image_;
    WorkspaceInputQueue<InputRecord> input_;
    Completion completion_;
    std::uint64_t input_epoch_ = 0U;
    std::uint64_t next_interaction_ = 0U;
    std::size_t pending_commands_ = 0U;
    std::optional<AnnotationPointer> pointer_;
    bool right_pressed_ = false;
    bool gpu_continuation_ = false, sampling_ = false, stopping_ = false;
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
void AnnotationSystem::Input(WorkspaceMouse mouse) { impl_->Input(std::move(mouse)); }
void AnnotationSystem::SetInputPeer(std::uint64_t epoch) { impl_->SetInputPeer(epoch); }
AnnotationSnapshot AnnotationSystem::Edit(AnnotationEditRequest request) { return impl_->Edit(std::move(request)); }
AnnotationSnapshot AnnotationSystem::Save(AnnotationSave request) { return impl_->Save(std::move(request)); }
AnnotationSnapshot AnnotationSystem::Stop() noexcept { return impl_->Stop(); }
void AnnotationSystem::PeerClosed() noexcept { impl_->PeerClosed(); }
void AnnotationSystem::Shutdown() noexcept { impl_->Shutdown(); }
bool AnnotationSystem::stopped() const noexcept { return impl_->stopped(); }
// CLEANUP-IGNORE: These direct methods expose Annotation's sealed owner; Live and Upscale retain independent system ownership.
AnnotationSnapshot AnnotationSystem::snapshot() const { return impl_->snapshot(); }
std::optional<AnnotationImageMetadata> AnnotationSystem::ImageSnapshot(const VisualFrame& frame) const { return impl_->ImageSnapshot(frame); }
VisualSourceObservation AnnotationSystem::ObserveSource() const { return impl_->ObserveSource(); }
mmltk::frameworks::gpu::BorrowedImageProductReadView AnnotationSystem::BorrowFrame() const { return impl_->BorrowFrame(); }
mmltk::frameworks::gpu::BorrowedImageWorkspace AnnotationSystem::BorrowWorkspace() const { return impl_->renderer_.BorrowWorkspace(); }
mmltk::frameworks::gpu::ImageWorkspaceObservation AnnotationSystem::ObserveWorkspace() const { return impl_->renderer_.ObserveWorkspace(); }
void AnnotationSystem::RequestWorkspace(VisualWorkspaceRequest request) { impl_->renderer_.RequestWorkspace(std::move(request)); }
}  // namespace mmltk::controller
