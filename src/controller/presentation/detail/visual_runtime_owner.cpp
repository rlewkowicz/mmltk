#include "src/controller/presentation/detail/visual_runtime_owner.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mmltk::controller::detail {

VisualRuntimeOwner::VisualRuntimeOwner(RuntimeFactory factory, FailureSink failures, ActivityObservation activity)
    : factory_(std::move(factory)),
      failures_(std::move(failures)),
      activity_(std::move(activity)),
      worker_([this](const std::stop_token stop) { Run(stop); }, [this](const std::exception_ptr failure) { Failed(failure); },
              [this] { RetireRuntime(); }) {
    if (!factory_) throw std::invalid_argument("visual runtime factory is unavailable");
}
VisualRuntimeOwner::~VisualRuntimeOwner() {
    StopAndWait();
    RetainedRuntime retained;
    {
        std::scoped_lock lock(mutex_);
        retained = std::move(retained_);
        retained_.emplace<std::monostate>();
    }
}

bool VisualRuntimeOwner::SubmitDiscrete(Work work, Notification cancellation, const bool reconstruct) {
    if (!work) throw std::invalid_argument("visual discrete work is empty");
    {
        std::scoped_lock lock(mutex_);
        const std::size_t required = 1U + static_cast<std::size_t>(latest_.has_value());
        if (stopping_ || runtime_retirement_blocked_ || discrete_active_ || ordered_.size() > 32U - required) return false;
        FlushLatest();
        ordered_.push_back(ScheduledWork{
            .run = std::move(work),
            .cancellation = std::move(cancellation),
            .dispatched = {},
            .stop = std::stop_source{},
            .discrete = true,
            .reconstruct = reconstruct,
        });
        discrete_active_ = true;
    }
    worker_.Wake();
    return true;
}
bool VisualRuntimeOwner::SubmitOrdered(Work work) {
    if (!work) throw std::invalid_argument("visual ordered work is empty");
    {
        std::scoped_lock lock(mutex_);
        const std::size_t required = 1U + static_cast<std::size_t>(latest_.has_value());
        if (stopping_ || runtime_retirement_blocked_ || ordered_.size() > 32U - required) return false;
        FlushLatest();
        ordered_.push_back(ScheduledWork{
            .run = std::move(work),
            .cancellation = {},
            .dispatched = {},
            .stop = std::stop_source{},
        });
    }
    worker_.Wake();
    return true;
}
void VisualRuntimeOwner::FlushLatest() {
    if (!latest_) return;
    ordered_.push_back(ScheduledWork{
        .run = std::move(*latest_),
        .cancellation = {},
        .dispatched = {},
        .stop = std::stop_source{},
    });
    latest_.reset();
}
bool VisualRuntimeOwner::SubmitTerminalBarrier(Work work) {
    if (!work) throw std::invalid_argument("visual terminal barrier work is empty");
    {
        std::scoped_lock lock(mutex_);
        if (stopping_ || runtime_retirement_blocked_ || terminal_barrier_active_) return false;
        latest_.reset();
        continuation_state_.store(0U, std::memory_order_release);
        ordered_.push_back(ScheduledWork{
            .run = std::move(work),
            .cancellation = {},
            .dispatched = {},
            .stop = std::stop_source{},
            .terminal_barrier = true,
        });
        terminal_barrier_active_ = true;
    }
    worker_.Wake();
    return true;
}
bool VisualRuntimeOwner::SubmitLatest(Work work) {
    if (!work) throw std::invalid_argument("visual latest work is empty");
    {
        std::scoped_lock lock(mutex_);
        if (stopping_ || runtime_retirement_blocked_) return false;
        latest_ = std::move(work);
    }
    worker_.Wake();
    return true;
}
void VisualRuntimeOwner::RegisterContinuation(Work work, DispatchObservation dispatched) {
    if (!work) throw std::invalid_argument("visual continuation work is empty");
    std::scoped_lock lock(mutex_);
    if (continuation_ || stopping_ || terminal_barrier_active_ || runtime_retirement_blocked_)
        throw std::logic_error("visual continuation registration is unavailable");
    continuation_ = std::move(work);
    continuation_dispatched_ = std::move(dispatched);
    continuation_state_.store(kContinuationEnabled, std::memory_order_release);
}
bool VisualRuntimeOwner::NotifyContinuation() noexcept {
    const auto state = continuation_state_.fetch_or(kContinuationPending, std::memory_order_acq_rel);
    if ((state & kContinuationEnabled) == 0U) return false;
    if ((state & kContinuationPending) == 0U) worker_.Wake();
    return true;
}
bool VisualRuntimeOwner::RequestActiveStop() noexcept {
    std::stop_source stop{std::nostopstate};
    Notification cancellation;
    {
        std::scoped_lock lock(mutex_);
        stop = active_stop_;
        if (!active_discrete_) {
            const auto queued = std::ranges::find_if(ordered_, [](const auto& item) { return item.discrete; });
            if (queued != ordered_.end()) {
                cancellation = std::move(queued->cancellation);
                ordered_.erase(queued);
                discrete_active_ = false;
            }
        }
    }
    static_cast<void>(stop.request_stop());
    const bool cancelled_queued = static_cast<bool>(cancellation);
    if (cancellation) cancellation();
    return cancelled_queued;
}
void VisualRuntimeOwner::RequestStop() noexcept {
    std::stop_source stop{std::nostopstate};
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        ordered_.clear();
        latest_.reset();
        continuation_state_.store(0U, std::memory_order_release);
        runtime_retirement_blocked_ = true;
        discrete_active_ = false;
        terminal_barrier_active_ = false;
        stop = active_stop_;
    }
    static_cast<void>(stop.request_stop());
    Observe(ActivityStage::StopRequested);
    worker_.RequestStop();
}
void VisualRuntimeOwner::StopAndWait() noexcept {
    RequestStop();
    Observe(ActivityStage::JoinStarted);
    worker_.WaitStopped();
    Observe(ActivityStage::JoinCompleted);
}
bool VisualRuntimeOwner::stopped() const noexcept { return worker_.stopped(); }
bool VisualRuntimeOwner::busy() const noexcept {
    std::scoped_lock lock(mutex_);
    return discrete_active_;
}
mmltk::frameworks::gpu::BorrowedImageProductReadView VisualRuntimeOwner::Borrow() const {
    std::scoped_lock lock(mutex_);
    Observe(ActivityStage::BorrowLocked);
    auto current = runtime_ ? runtime_->Borrow() : mmltk::frameworks::gpu::BorrowedImageProductReadView{};
    if (current.valid() || !replacement_fallback_) return current;
    return replacement_fallback_->Borrow();
}
VisualRuntimeOwner::Runtime& VisualRuntimeOwner::RuntimeForWork() {
    std::scoped_lock lock(mutex_);
    if (!runtime_) {
        runtime_ = factory_();
        if (!runtime_) throw std::runtime_error("visual runtime factory returned no runtime");
        runtime_->SetProductRevisionSequence(product_revision_sequence_);
    }
    if (!execution_policy_) {
        if (const auto* execution = runtime_->execution())
            execution_policy_.emplace(mmltk::common::system::ExecutionPolicyRequest{
                execution->placement.cpus, {}, 0, execution->placement.numa_node, -10, false});
    }
    runtime_->BeginWork();
    return *runtime_;
}
void VisualRuntimeOwner::Observe(const ActivityStage stage, const std::uint64_t value) const noexcept {
    if (activity_) activity_(stage, value);
}
void VisualRuntimeOwner::Run(const std::stop_token worker_stop) {
    Observe(ActivityStage::CycleEntered);
    std::optional<ScheduledWork> ordered_work;
    std::optional<Work> latest_work;
    bool continuation_work = false;
    Notification notification;
    bool discrete = false;
    bool terminal_barrier = false;
    std::stop_token operation_stop;
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) return;
        if (!ordered_.empty()) {
            ordered_work.emplace(std::move(ordered_.front()));
            ordered_.pop_front();
            discrete = ordered_work->discrete;
            terminal_barrier = ordered_work->terminal_barrier;
        } else if (latest_) {
            latest_work = std::move(latest_);
            latest_.reset();
        } else if ((continuation_state_.fetch_and(static_cast<std::uint8_t>(~kContinuationPending), std::memory_order_acq_rel) &
                    (kContinuationEnabled | kContinuationPending)) == (kContinuationEnabled | kContinuationPending)) {
            continuation_work = true;
        } else {
            return;
        }
        active_stop_ = ordered_work ? ordered_work->stop : std::stop_source{};
        active_discrete_ = discrete;
        operation_stop = active_stop_.get_token();
    }
    Observe(ActivityStage::WorkSelected, ordered_work ? 1U : (latest_work ? 2U : (continuation_work ? 3U : 0U)));
    if (ordered_work && (worker_stop.stop_requested() || operation_stop.stop_requested())) {
        notification = std::move(ordered_work->cancellation);
    } else if (worker_stop.stop_requested() || operation_stop.stop_requested()) {
        notification = {};
    } else if (ordered_work) {
        if (ordered_work->reconstruct) BeginRuntimeReplacement();
        notification = ordered_work->run(RuntimeForWork(), operation_stop);
        if (ordered_work->reconstruct) CompleteRuntimeReplacement();
    } else if (latest_work) {
        notification = (*latest_work)(RuntimeForWork(), operation_stop);
    } else {
        if (continuation_dispatched_) continuation_dispatched_();
        notification = continuation_(RuntimeForWork(), operation_stop);
    }
    Observe(ActivityStage::WorkCompleted);
    bool wake_again = false;
    {
        std::scoped_lock lock(mutex_);
        if (discrete) discrete_active_ = false;
        if (terminal_barrier) {
            terminal_barrier_active_ = false;
            if (continuation_ && !stopping_) continuation_state_.store(kContinuationEnabled, std::memory_order_release);
        }
        active_stop_ = std::stop_source{std::nostopstate};
        active_discrete_ = false;
        wake_again =
            !ordered_.empty() || latest_.has_value() || (continuation_state_.load(std::memory_order_acquire) & kContinuationPending) != 0U;
    }
    Observe(ActivityStage::CycleFinalized, wake_again ? 1U : 0U);
    if (notification) {
        Observe(ActivityStage::NotificationStarted);
        notification();
        Observe(ActivityStage::NotificationCompleted);
    }
    if (wake_again) {
        Observe(ActivityStage::RewakeStarted);
        worker_.Wake();
        Observe(ActivityStage::RewakeCompleted);
    }
    Observe(ActivityStage::CycleExited);
}
void VisualRuntimeOwner::Failed(const std::exception_ptr failure) noexcept {
    auto construction_custody = Runtime::UnsafeConstruction(failure);
    const auto construction_failure = construction_custody ? construction_custody->failure() : std::exception_ptr{};
    std::unique_ptr<Runtime> retired;
    {
        std::scoped_lock lock(mutex_);
        if (preserve_runtime_on_failure_)
            preserve_runtime_on_failure_ = false;
        else
            retired = std::move(runtime_);
        if (replacement_fallback_) runtime_ = std::move(replacement_fallback_);
        ordered_.clear();
        latest_.reset();
        continuation_state_.store(0U, std::memory_order_release);
        runtime_retirement_blocked_ = true;
        discrete_active_ = false;
        terminal_barrier_active_ = false;
        active_stop_ = std::stop_source{std::nostopstate};
        active_discrete_ = false;
    }
    std::exception_ptr retirement_failure;
    Runtime::UnsafeCustody retirement_custody;
    bool unsafe_retirement = false;
    if (retired) {
        auto retirement = retired->Retire();
        retirement_failure = retirement.failure;
        unsafe_retirement = !retirement.safe_to_destroy;
        if (unsafe_retirement) retirement_custody = std::move(retirement.custody);
        retired.reset();
    }
    if (execution_policy_) {
        try {
            execution_policy_->Restore();
            execution_policy_.reset();
        } catch (...) { retirement_failure = std::current_exception(); }
    }
    {
        std::scoped_lock lock(mutex_);
        if (construction_custody)
            retained_.emplace<Runtime::UnsafeCustody>(std::move(*construction_custody));
        else if (unsafe_retirement)
            retained_.emplace<Runtime::UnsafeCustody>(std::move(retirement_custody));
        continuation_state_.store(0U, std::memory_order_release);
        runtime_retirement_blocked_ = retained_.index() != 0U || execution_policy_.has_value();
        if (continuation_ && !runtime_retirement_blocked_ && !stopping_)
            continuation_state_.store(kContinuationEnabled, std::memory_order_release);
    }
    if (!failures_) return;
    try {
        failures_(retirement_failure ? retirement_failure : (construction_failure ? construction_failure : failure));
    } catch (...) { failures_ = {}; }
}

void VisualRuntimeOwner::BeginRuntimeReplacement() {
    {
        std::scoped_lock lock(mutex_);
        if (replacement_fallback_) throw std::logic_error("visual runtime replacement is already active");
        replacement_fallback_ = std::move(runtime_);
    }
    if (execution_policy_) {
        execution_policy_->Restore();
        execution_policy_.reset();
    }
}

void VisualRuntimeOwner::CompleteRuntimeReplacement() {
    Runtime* replacement = nullptr;
    {
        std::scoped_lock lock(mutex_);
        if (!replacement_fallback_) return;
        replacement = runtime_.get();
    }
    auto completed = replacement ? replacement->Borrow() : mmltk::frameworks::gpu::BorrowedImageProductReadView{};
    if (!completed.valid()) {
        std::unique_ptr<Runtime> rejected;
        {
            std::scoped_lock lock(mutex_);
            rejected = std::move(runtime_);
            runtime_ = std::move(replacement_fallback_);
            preserve_runtime_on_failure_ = true;
        }
        Observe(ActivityStage::RetirementStarted);
        auto retirement = rejected->Retire();
        rejected.reset();
        if (!retirement.safe_to_destroy) {
            std::scoped_lock lock(mutex_);
            retained_.emplace<Runtime::UnsafeCustody>(std::move(retirement.custody));
            runtime_retirement_blocked_ = true;
        }
        if (execution_policy_) {
            execution_policy_->Restore();
            execution_policy_.reset();
        }
        Observe(ActivityStage::RetirementCompleted, retirement.safe_to_destroy ? 1U : 0U);
        if (retirement.failure) std::rethrow_exception(retirement.failure);
        {
            std::scoped_lock lock(mutex_);
            preserve_runtime_on_failure_ = false;
        }
        return;
    }

    std::unique_ptr<Runtime> retired;
    {
        std::scoped_lock lock(mutex_);
        retired = std::move(replacement_fallback_);
    }
    Observe(ActivityStage::RetirementStarted);
    auto retirement = retired->Retire();
    retired.reset();
    if (!retirement.safe_to_destroy) {
        std::scoped_lock lock(mutex_);
        retained_.emplace<Runtime::UnsafeCustody>(std::move(retirement.custody));
        runtime_retirement_blocked_ = true;
    }
    Observe(ActivityStage::RetirementCompleted, retirement.safe_to_destroy ? 1U : 0U);
    if (retirement.failure) {
        {
            std::scoped_lock lock(mutex_);
            preserve_runtime_on_failure_ = true;
        }
        std::rethrow_exception(retirement.failure);
    }
}

void VisualRuntimeOwner::RetireRuntime() {
    Observe(ActivityStage::RetirementStarted);
    std::unique_ptr<Runtime> retired;
    {
        std::scoped_lock lock(mutex_);
        retired = std::move(runtime_);
    }
    if (!retired) {
        if (execution_policy_) {
            execution_policy_->Restore();
            execution_policy_.reset();
        }
        return;
    }
    auto retirement = retired->Retire();
    retired.reset();
    if (!retirement.safe_to_destroy) {
        std::scoped_lock lock(mutex_);
        retained_.emplace<Runtime::UnsafeCustody>(std::move(retirement.custody));
        runtime_retirement_blocked_ = true;
    }
    if (execution_policy_) {
        execution_policy_->Restore();
        execution_policy_.reset();
    }
    Observe(ActivityStage::RetirementCompleted, retirement.safe_to_destroy ? 1U : 0U);
    if (retirement.failure) std::rethrow_exception(retirement.failure);
}

}  // namespace mmltk::controller::detail
