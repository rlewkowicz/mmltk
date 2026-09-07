#include "src/controller/subsystems/live/live_system.h"
#include "src/controller/presentation/detail/visual_runtime_owner.h"
#include "src/frameworks/gpu/system_image_worker.h"

#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "src/controller/contracts/compute.h"

namespace mmltk::controller {
class LiveSystem::Impl final {
   public:
    Impl(const VisualDeviceSettings settings, VisualRuntimeFactory factory, SystemEventSink<event_type> events,
         VisualDiagnosticSink diagnostics)
        : settings_(settings),
          events_(std::move(events)),
          diagnostics_(diagnostics),
          worker_(std::move(factory), [this](const std::exception_ptr failure) {
              auto detail = visual_failure_detail(failure, "Live GPU worker failed");
              LiveSnapshot settled;
              {
                  std::scoped_lock lock(mutex_);
                  if (!state_.running) return;
                  state_.running = false;
                  state_.cancellation_requested = false;
                  state_.frame = {};
                  AdvanceRevision();
                  settled = state_;
              }
              report_visual_worker_failure(diagnostics_, VisualSystemKind::Live, settings_.device, detail);
              Publish(event_type{LiveFailed{std::move(settled), std::move(detail)}});
          }) {
        if (!settings_.valid()) throw contracts::InvalidIntentError("Live device settings are invalid");
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
                                         const std::stop_token worker_stop) -> detail::VisualRuntimeOwner::Notification {
                    auto* const algorithm = dynamic_cast<LiveAlgorithm*>(runtime.model());
                    if (algorithm == nullptr) throw std::runtime_error("Live capture data plane is unavailable");
                    algorithm->Start(request);
                    std::mutex cadence_mutex;
                    std::condition_variable_any cadence_ready;
                    std::unique_lock cadence_lock(cadence_mutex);
                    while (!worker_stop.stop_requested()) {
                        bool captured = false;
                        runtime.Publish(request.extent.width, request.extent.height,
                                        [algorithm, worker_stop, &captured](const auto target, const auto, const auto stream) {
                                            captured = algorithm->Capture(target, stream, worker_stop);
                                        });
                        if (!captured) continue;
                        {
                            std::scoped_lock lock(mutex_);
                            if (state_.completed_frames == std::numeric_limits<std::uint64_t>::max())
                                throw std::overflow_error("Live completed-frame counter exhausted");
                            ++state_.completed_frames;
                            AdvanceRevision();
                            state_.frame = {
                                .source =
                                    {
                                        .kind = PresentationSourceKind::Live,
                                        .instance = 1U,
                                    },
                                .extent = request.extent,
                                .revision = runtime.output().revision(),
                            };
                        }
                        PublishFrame();
                        diagnostics_({
                            .system = VisualSystemKind::Live,
                            .operation = VisualDiagnosticOperation::FrameCompleted,
                            .device = settings_.device,
                            .generation = runtime.output().revision(),
                        });
                        cadence_ready.wait_for(cadence_lock, worker_stop, cadence, [] { return false; });
                    }
                    return [this, algorithm] { Settle(algorithm); };
                },
                [this] { Settle(nullptr); })) {
            throw contracts::BusyError("Live is already running");
        }
        state_.running = true;
        state_.cancellation_requested = false;
        state_.revision = *admitted_revision;
        return state_;
    }
    LiveSnapshot Stop() noexcept {
        bool request_stop = false;
        LiveSnapshot result;
        {
            std::scoped_lock lock(mutex_);
            if (state_.running && !state_.cancellation_requested) {
                state_.cancellation_requested = true;
                AdvanceRevision();
                request_stop = true;
            }
            result = state_;
        }
        if (request_stop) worker_.RequestActiveStop();
        return result;
    }
    void Shutdown() noexcept {
        static_cast<void>(Stop());
        worker_.StopAndWait();
    }
    bool stopped() const noexcept { return worker_.stopped(); }
    LiveSnapshot snapshot() const {
        std::scoped_lock lock(mutex_);
        return state_;
    }
    void Publish(event_type event) noexcept { publish_visual_event_noexcept(events_, std::move(event)); }
    void PublishFrame() noexcept { Publish(event_type{LiveFrameCompleted{snapshot()}}); }
    void Settle(LiveAlgorithm* algorithm) noexcept {
        if (algorithm != nullptr) algorithm->Stop();
        LiveSnapshot settled;
        {
            std::scoped_lock lock(mutex_);
            if (!state_.running) return;
            state_.running = false;
            state_.cancellation_requested = false;
            try {
                AdvanceRevision();
            } catch (...) { return; }
            settled = state_;
        }
        Publish(event_type{LiveChanged{std::move(settled)}});
    }
    void AdvanceRevision() {
        const auto next = contracts::next_compute_generation(state_.revision);
        if (!next) throw std::overflow_error("Live observation revision exhausted");
        state_.revision = *next;
    }

   private:
    friend class LiveSystem;

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
    detail::VisualRuntimeOwner worker_;
};

LiveSystem::LiveSystem(const VisualDeviceSettings settings, VisualRuntimeFactory factory, SystemEventSink<event_type> events,
                       VisualDiagnosticSink diagnostics)
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
mmltk::frameworks::gpu::BorrowedImageProductReadView LiveSystem::BorrowFrame() const { return impl_->BorrowFrame(); }

}  // namespace mmltk::controller
