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

#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/product_revision_sequence.h"
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
        StagedCompletionLatched,
    };
    using Runtime = mmltk::frameworks::gpu::SystemImageRuntime;
    using RuntimeFactory = VisualRuntimeFactory;
    using Notification = std::move_only_function<void()>;
    using DispatchObservation = std::move_only_function<void() noexcept>;
    using Work = std::move_only_function<Notification(Runtime&, std::stop_token)>;
    using FailureSink = std::function<void(std::exception_ptr)>;
    using ActivityObservation = std::move_only_function<void(ActivityStage, std::uint64_t) const noexcept>;

    // Claim the active operation's terminal boundary before committing producer
    // state. A winning stop rejects completion; a winning completion makes later
    // stops inert for this operation. Staged work also claims it on return.
    [[nodiscard]] bool TryCompleteActiveWork() noexcept;

    VisualRuntimeOwner(RuntimeFactory, FailureSink, ActivityObservation = {});
    ~VisualRuntimeOwner();
    VisualRuntimeOwner(const VisualRuntimeOwner&) = delete;
    VisualRuntimeOwner& operator=(const VisualRuntimeOwner&) = delete;
    [[nodiscard]] bool SubmitDiscrete(Work, Notification cancellation = {}, bool reconstruct = false);
    [[nodiscard]] bool SubmitOrdered(Work);
    void RegisterOrderedDrain(Work);
    [[nodiscard]] bool NotifyOrderedDrain();
    [[nodiscard]] bool SubmitTerminalBarrier(Work);
    bool SubmitLatest(Work);
    enum class ContinuationCancellation : std::uint8_t { Cancel, PreserveOrderedInput };
    void RegisterContinuation(Work, DispatchObservation = {}, bool wake_on_output_available = false,
                              ContinuationCancellation = ContinuationCancellation::Cancel);
    [[nodiscard]] bool NotifyContinuation() noexcept;
    // Arm before testing output writability; disarm clears only availability retries.
    void SetOutputRetry(bool armed) noexcept;
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
        bool ordered_drain = false;
        bool discrete = false;
        bool terminal_barrier = false;
        bool reconstruct = false;
    };
    using RetainedRuntime = std::variant<std::monostate, Runtime::UnsafeCustody>;
    class StagedReplacement final {
       public:
        explicit StagedReplacement(VisualRuntimeOwner&);
        ~StagedReplacement();
        StagedReplacement(const StagedReplacement&) = delete;
        StagedReplacement& operator=(const StagedReplacement&) = delete;
        [[nodiscard]] std::exception_ptr Finish(bool promote) noexcept;

       private:
        VisualRuntimeOwner* owner_;
    };
    void Run(std::stop_token);
    void Failed(std::exception_ptr) noexcept;
    void ReportFailure(std::exception_ptr) noexcept;
    [[nodiscard]] std::exception_ptr FinishRuntimeReplacement(bool) noexcept;
    [[nodiscard]] std::exception_ptr RetireOwned(std::unique_ptr<Runtime>) noexcept;
    void RestorePolicy();
    void NotifyReaders() const;
    void RetireRuntime();
    void FlushLatest();
    void Observe(ActivityStage, std::uint64_t value = 0U) const noexcept;
    [[nodiscard]] Runtime* RuntimeForWork(std::stop_token worker_stop, std::stop_token operation_stop);

    struct OutputWake;
    std::shared_ptr<OutputWake> output_wake_;
    RuntimeFactory factory_;
    FailureSink failures_;
    ActivityObservation activity_;
    mutable std::mutex mutex_;
    std::deque<ScheduledWork> ordered_;
    std::optional<Work> latest_;
    Work ordered_drain_;
    std::stop_source drain_stop_{std::nostopstate};
    bool drain_queued_ = false;
    Work continuation_;
    DispatchObservation continuation_dispatched_;
    ContinuationCancellation continuation_cancellation_ = ContinuationCancellation::Cancel;
    static constexpr std::uint8_t kContinuationEnabled = 1U;
    static constexpr std::uint8_t kContinuationPending = 2U;
    static constexpr std::uint8_t kOutputRetryPending = 4U;
    std::atomic<std::uint8_t> continuation_state_{0U};
    std::unique_ptr<Runtime> runtime_;
    std::unique_ptr<Runtime> replacement_;
    bool replacement_active_ = false;
    const std::shared_ptr<mmltk::frameworks::gpu::ImageProductRevisionSequence> product_revision_sequence_{
        std::make_shared<mmltk::frameworks::gpu::ImageProductRevisionSequence>()};
    std::optional<mmltk::common::system::ScopedExecutionPolicy> execution_policy_;
    RetainedRuntime retained_;
    std::stop_source active_stop_{std::nostopstate};
    enum class ActiveOutcome : std::uint8_t { Running, Cancelled, Completed };
    ActiveOutcome active_outcome_ = ActiveOutcome::Running;
    bool discrete_active_ = false;
    bool active_discrete_ = false;
    bool active_preserves_input_ = false;
    bool terminal_barrier_active_ = false;
    bool runtime_retirement_blocked_ = false;
    bool stopping_ = false;
    mmltk::frameworks::gpu::SystemImageWorker worker_;
};

}  // namespace mmltk::controller::detail

namespace mmltk::controller {

[[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView borrow_matching_visual_product(const VisualFrame&,
                                                                                                  const detail::VisualRuntimeOwner&);

}  // namespace mmltk::controller
