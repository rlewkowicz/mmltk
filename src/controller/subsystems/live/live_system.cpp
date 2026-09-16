#include "src/controller/presentation/workspace_input.h"
#include "src/controller/subsystems/live/live_system.h"
#include "src/controller/presentation/detail/visual_runtime_owner.h"
#include "src/frameworks/gpu/image_failure.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include "src/controller/contracts/compute.h"
namespace mmltk::controller {
class LiveSystem::Impl final {
    WorkspaceInput input_;

   public:
    Impl(const VisualDeviceSettings settings, VisualRuntimeFactory factory, SystemEventSink<event_type> events, VisualDiagnosticSink diagnostics)
        : settings_(settings), events_(std::move(events)), diagnostics_(diagnostics), worker_(std::move(factory), [this](const std::exception_ptr failure) {
              pending_output_ = {};
              const auto reported = mmltk::frameworks::gpu::combine_image_failures(failure, worker_.FinishDeferredRetirement());
              auto detail = visual_failure_detail(reported, "Live GPU worker failed");
              LiveSnapshot settled;
              {
                  std::scoped_lock lock(mutex_);
                  if (!state_.running) return;
                  state_.running = false;
                  state_.cancellation_requested = false;
                  state_.frame = {};
                  if (const auto next = contracts::next_compute_generation(state_.revision)) state_.revision = *next;
                  settled = state_;
              }
              settled_.notify_all();
              report_visual_worker_failure(diagnostics_, contracts::DiagnosticOwner::Live, settings_.device, detail);
              Publish(event_type{LiveFailed{std::move(settled), std::move(detail)}});
          }) {
        if (!settings_.valid()) throw contracts::InvalidIntentError("Live device settings are invalid");
        worker_.RegisterContinuation([this](auto& runtime, const std::stop_token stop) { return Capture(runtime, stop); }, {}, true);
    }
    LiveSnapshot Start(const LiveStart request) {
        if (!request.extent.valid() || request.extent.width > settings_.maximum_width || request.extent.height > settings_.maximum_height ||
            request.frames_per_second == 0U || request.frames_per_second > 240U)
            throw contracts::InvalidIntentError("Live capture settings are invalid");
        const auto cadence = std::chrono::microseconds{1'000'000U / request.frames_per_second};
        std::scoped_lock state_lock(mutex_);
        if (state_.running) throw contracts::BusyError("Live is already running");
        const auto admitted_revision = contracts::next_compute_generation(state_.revision);
        if (!admitted_revision) throw contracts::FailedError("Live observation revision exhausted");
        if (!worker_.SubmitDiscrete(
                [this, request, cadence](mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                         const std::stop_token) -> detail::VisualRuntimeOwner::Notification {
                    auto* const algorithm = dynamic_cast<LiveAlgorithm*>(runtime.model());
                    if (algorithm == nullptr) throw std::runtime_error("Live capture data plane is unavailable");
                    algorithm->SetOutputAvailableSink([this] { static_cast<void>(worker_.NotifyContinuation()); });
                    algorithm->Start(request);
                    request_ = request;
                    cadence_ = cadence;
                    next_capture_ = {};
                    static_cast<void>(worker_.NotifyContinuation());
                    return {};
                },
                [this] { Settle(nullptr); })) {
            throw contracts::BusyError("Live is already running");
        }
        state_.running = true;
        run_stop_ = std::stop_source{};
        state_.cancellation_requested = false;
        state_.revision = *admitted_revision;
        return state_;
    }
    LiveSnapshot Stop() noexcept {
        bool request_stop = false;
        std::stop_source stop{std::nostopstate};
        LiveSnapshot result;
        {
            std::scoped_lock lock(mutex_);
            if (state_.running && !state_.cancellation_requested) {
                state_.cancellation_requested = true;
                if (const auto next = contracts::next_compute_generation(state_.revision)) state_.revision = *next;
                request_stop = true;
                stop = run_stop_;
            }
            result = state_;
        }
        if (request_stop) {
            static_cast<void>(stop.request_stop());
            if (!worker_.RequestActiveStop()) static_cast<void>(worker_.NotifyContinuation());
        }
        return result;
    }
    void Shutdown() noexcept {
        static_cast<void>(Stop());
        {
            std::unique_lock lock(mutex_);
            settled_.wait(lock, [this] { return !state_.running; });
        }
        worker_.StopAndWait();
    }
    bool stopped() const noexcept { return worker_.stopped(); }
    [[nodiscard]] std::optional<VisualImageMetadata> ImageSnapshot(const VisualFrame& frame) const {
        std::scoped_lock lock(mutex_);
        if (state_.frame != frame) return std::nullopt;
        return LiveSystem::visual_source::ImageOf(state_);
    }
    LiveSnapshot snapshot() const {
        std::scoped_lock lock(mutex_);
        return state_;
    }
    void Publish(event_type event) noexcept { publish_visual_event_noexcept(events_, std::move(event)); }
    void PublishFrame() noexcept { Publish(event_type{LiveFrameCompleted{snapshot()}}); }
    void Settle(LiveAlgorithm* algorithm) noexcept {
        pending_output_ = {};
        if (algorithm != nullptr) algorithm->Stop();
        LiveSnapshot settled;
        {
            std::scoped_lock lock(mutex_);
            if (!state_.running) return;
            state_.running = false;
            state_.cancellation_requested = false;
            if (const auto next = contracts::next_compute_generation(state_.revision)) state_.revision = *next;
            settled = state_;
        }
        settled_.notify_all();
        Publish(event_type{LiveChanged{std::move(settled)}});
    }
    void AdvanceRevision() {
        const auto next = contracts::next_compute_generation(state_.revision);
        if (!next) throw std::overflow_error("Live observation revision exhausted");
        state_.revision = *next;
    }

   private:
    friend class LiveSystem;
    detail::VisualRuntimeOwner::Notification Capture(mmltk::frameworks::gpu::SystemImageRuntime& runtime, const std::stop_token worker_stop) {
        auto* const algorithm = dynamic_cast<LiveAlgorithm*>(runtime.model());
        if (!algorithm) throw std::runtime_error("Live capture data plane is unavailable");
        std::stop_token run_stop;
        std::stop_source stop_source{std::nostopstate};
        {
            std::scoped_lock lock(mutex_);
            if (!state_.running) return {};
            run_stop = run_stop_.get_token();
            stop_source = run_stop_;
        }
        std::stop_callback stopping(worker_stop, [stop_source]() mutable { static_cast<void>(stop_source.request_stop()); });
        if (run_stop.stop_requested()) {
            worker_.SetOutputRetry(false);
            algorithm->Stop();
            return [this] { Settle(nullptr); };
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < next_capture_) {
            worker_.NotifyContinuationAt(next_capture_);
            return {};
        }
        if (!pending_output_.valid()) pending_output_ = runtime.Completed();
        auto candidate = worker_.TryAcquireOutput(runtime, pending_output_);
        if (!candidate.valid()) return {};
        if (!algorithm->AcquireOutput()) return {};
        bool captured = false;
        runtime.Publish(candidate, request_.extent.width, request_.extent.height,
                        [&](const auto target, const auto, const auto stream) { captured = algorithm->Capture(target, stream, run_stop); });
        if (!captured || run_stop.stop_requested()) {
            static_cast<void>(worker_.NotifyContinuation());
            return {};
        }
        static_cast<void>(runtime.CommitOutput(std::move(candidate)));
        {
            std::scoped_lock lock(mutex_);
            if (state_.completed_frames == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Live completed-frame counter exhausted");
            ++state_.completed_frames;
            AdvanceRevision();
            state_.frame = {.source = {PresentationSourceKind::Live, 1U}, .extent = request_.extent, .revision = runtime.OutputFacts().revision};
        }
        next_capture_ = std::chrono::steady_clock::now() + cadence_;
        worker_.NotifyContinuationAt(next_capture_);
        diagnostics_.Emit([&] {
            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Live,
                                        .operation = VisualDiagnosticOperation::FrameCompleted,
                                        .device = settings_.device,
                                        .generation = runtime.OutputFacts().revision};
        });
        return [this] { PublishFrame(); };
    }
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const {
        VisualFrame committed;
        {
            std::scoped_lock lock(mutex_);
            if (!state_.frame.valid()) return {};
            committed = state_.frame;
        }
        return borrow_matching_visual_product(committed, worker_);
    }
    VisualDeviceSettings settings_;
    SystemEventSink<event_type> events_;
    VisualDiagnosticSink diagnostics_{};
    mutable std::mutex mutex_;
    LiveSnapshot state_;
    std::condition_variable settled_;
    std::stop_source run_stop_;
    LiveStart request_{};
    std::chrono::microseconds cadence_{};
    std::chrono::steady_clock::time_point next_capture_{};
    mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput pending_output_;
    detail::VisualRuntimeOwner worker_;
};
LiveSystem::LiveSystem(const VisualDeviceSettings settings, VisualRuntimeFactory factory, SystemEventSink<event_type> events, VisualDiagnosticSink diagnostics)
    : impl_(std::make_unique<Impl>(settings, std::move(factory), std::move(events), diagnostics)) {}
LiveSystem::~LiveSystem() = default;
LiveSnapshot LiveSystem::Start(const LiveStart request) {
    // CLEANUP-IGNORE: Live facade forwarding preserves its domain-specific Start endpoint.
    return impl_->Start(request);
}
LiveSnapshot LiveSystem::Stop() noexcept {
    // CLEANUP-IGNORE: Live facade forwarding preserves its domain-specific Stop result.
    return impl_->Stop();
}
void LiveSystem::Shutdown() noexcept { impl_->Shutdown(); }
bool LiveSystem::stopped() const noexcept { return impl_->stopped(); }
LiveSnapshot LiveSystem::snapshot() const { return impl_->snapshot(); }
// CLEANUP-IGNORE: Live forwards its sealed source API to its own owner and the existing shared renderer.
std::optional<VisualImageMetadata> LiveSystem::ImageSnapshot(const VisualFrame& frame) const { return impl_->ImageSnapshot(frame); }
mmltk::frameworks::gpu::BorrowedImageProductReadView LiveSystem::BorrowFrame() const { return impl_->BorrowFrame(); }
mmltk::frameworks::gpu::BorrowedImageWorkspace LiveSystem::BorrowWorkspace() const { return impl_->worker_.BorrowWorkspace(); }
mmltk::frameworks::gpu::ImageWorkspaceObservation LiveSystem::ObserveWorkspace() const { return impl_->worker_.ObserveWorkspace(); }
void LiveSystem::RequestWorkspace(VisualWorkspaceRequest request) { impl_->worker_.RequestWorkspace(std::move(request)); }
}  // namespace mmltk::controller
namespace mmltk::controller {
void LiveSystem::Input(WorkspaceMouse mouse) {
    std::scoped_lock lock(impl_->mutex_);
    if (impl_->worker_.stopped()) throw contracts::UnavailableError("Live input is unavailable");
    impl_->input_.Accept(std::move(mouse), PresentationSourceKind::Live);
}
void LiveSystem::SetInputPeer(std::uint64_t epoch) {
    std::scoped_lock lock(impl_->mutex_);
    impl_->input_.SetPeer(epoch);
}
}  // namespace mmltk::controller
