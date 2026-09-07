#pragma once

#include <atomic>
#include <functional>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <variant>

#include "src/frameworks/gpu/system_image_worker.h"
#include "src/common/system/execution_policy.h"
#include "src/controller/presentation/visual_runtime.h"
#include "src/controller/presentation/visual_system_types.h"

namespace mmltk::controller::detail {

class VisualRuntimeOwner final {
   public:
    enum class ActivityStage : std::uint8_t {
        CycleEntered = 1U,
        WorkSelected,
        WorkCompleted,
        CycleFinalized,
        NotificationStarted,
        NotificationCompleted,
        RewakeStarted,
        RewakeCompleted,
        CycleExited,
        BorrowLocked,
        StopRequested,
        JoinStarted,
        JoinCompleted,
        RetirementStarted,
        RetirementCompleted,
    };
    using Runtime = mmltk::frameworks::gpu::SystemImageRuntime;
    using RuntimeFactory = VisualRuntimeFactory;
    using Notification = std::move_only_function<void()>;
    using DispatchObservation = std::move_only_function<void() noexcept>;
    using Work = std::move_only_function<Notification(Runtime&, std::stop_token)>;
    using FailureSink = std::function<void(std::exception_ptr)>;
    using ActivityObservation = std::move_only_function<void(ActivityStage, std::uint64_t) const noexcept>;

    VisualRuntimeOwner(RuntimeFactory, FailureSink, ActivityObservation = {});
    ~VisualRuntimeOwner();
    VisualRuntimeOwner(const VisualRuntimeOwner&) = delete;
    VisualRuntimeOwner& operator=(const VisualRuntimeOwner&) = delete;
    [[nodiscard]] bool SubmitDiscrete(Work, Notification cancellation = {}, bool reconstruct = false);
    [[nodiscard]] bool SubmitOrdered(Work);
    [[nodiscard]] bool SubmitTerminalBarrier(Work);
    bool SubmitLatest(Work);
    void RegisterContinuation(Work, DispatchObservation = {});
    [[nodiscard]] bool NotifyContinuation() noexcept;
    bool RequestActiveStop() noexcept;
    void RequestStop() noexcept;
    void StopAndWait() noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView Borrow() const;

   private:
    struct ScheduledWork final {
        Work run;
        Notification cancellation;
        DispatchObservation dispatched;
        std::stop_source stop;
        bool discrete = false;
        bool terminal_barrier = false;
        bool reconstruct = false;
    };
    using RetainedRuntime = std::variant<std::monostate, Runtime::UnsafeCustody>;
    void Run(std::stop_token);
    void Failed(std::exception_ptr) noexcept;
    void BeginRuntimeReplacement();
    void CompleteRuntimeReplacement();
    void RetireRuntime();
    void FlushLatest();
    void Observe(ActivityStage, std::uint64_t value = 0U) const noexcept;
    [[nodiscard]] Runtime& RuntimeForWork();

    RuntimeFactory factory_;
    FailureSink failures_;
    ActivityObservation activity_;
    mutable std::mutex mutex_;
    std::deque<ScheduledWork> ordered_;
    std::optional<Work> latest_;
    Work continuation_;
    DispatchObservation continuation_dispatched_;
    static constexpr std::uint8_t kContinuationEnabled = 1U;
    static constexpr std::uint8_t kContinuationPending = 2U;
    std::atomic<std::uint8_t> continuation_state_{0U};
    std::unique_ptr<Runtime> runtime_;
    std::unique_ptr<Runtime> replacement_fallback_;
    std::shared_ptr<std::atomic<std::uint64_t>> product_revision_sequence_{
        std::make_shared<std::atomic<std::uint64_t>>(1U)};
    std::optional<mmltk::common::system::ScopedExecutionPolicy> execution_policy_;
    RetainedRuntime retained_;
    std::stop_source active_stop_{std::nostopstate};
    bool discrete_active_ = false;
    bool active_discrete_ = false;
    bool terminal_barrier_active_ = false;
    bool runtime_retirement_blocked_ = false;
    bool preserve_runtime_on_failure_ = false;
    bool stopping_ = false;
    mmltk::frameworks::gpu::SystemImageWorker worker_;
};

}  // namespace mmltk::controller::detail

namespace mmltk::controller {

[[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView borrow_matching_visual_product(
    const VisualFrame&, const detail::VisualRuntimeOwner&);

}  // namespace mmltk::controller
