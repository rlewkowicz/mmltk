#include "src/controller/presentation/detail/visual_runtime_owner.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include "src/frameworks/gpu/image_failure.h"

namespace mmltk::controller {

mmltk::frameworks::gpu::BorrowedImageProductReadView borrow_matching_visual_product(const VisualFrame& frame,
                                                                                 const detail::VisualRuntimeOwner& owner) {
    return borrow_matching_visual_product(frame, owner.Borrow());
}

}  // namespace mmltk::controller

namespace mmltk::controller::detail {

using mmltk::frameworks::gpu::combine_image_failures;

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
        if (active_outcome_ == ActiveOutcome::Running) {
            active_outcome_ = ActiveOutcome::Cancelled;
            stop = active_stop_;
        }
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
bool VisualRuntimeOwner::TryCompleteActiveWork() noexcept {
    std::scoped_lock lock(mutex_);
    if (!active_stop_.stop_possible() || active_outcome_ == ActiveOutcome::Cancelled) return false;
    active_outcome_ = ActiveOutcome::Completed;
    return true;
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
        if (active_outcome_ == ActiveOutcome::Running) {
            active_outcome_ = ActiveOutcome::Cancelled;
            stop = active_stop_;
        }
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
    bool observed = false;
    for (;;) {
        Runtime::CompletedOutput completed;
        mmltk::frameworks::gpu::ImageProductPool::Availability available;
        std::stop_token stop;
        {
            std::scoped_lock lock(mutex_);
            if (!observed) Observe(ActivityStage::BorrowLocked);
            observed = true;
            if (!runtime_) return {};
            available = runtime_->ObserveOutputAvailability();
            completed = runtime_->Completed();
            if (!completed.valid() && (stopping_ || !active_stop_.stop_possible())) return {};
            stop = active_stop_.get_token();
        }
        if (completed.valid()) return completed.Borrow();
        if (!available.Wait(stop)) return {};
    }
}
void VisualRuntimeOwner::NotifyReaders() const {
    mmltk::frameworks::gpu::ImageProductPool::Availability available;
    {
        std::scoped_lock lock(mutex_);
        if (runtime_) available = runtime_->ObserveOutputAvailability();
    }
    available.Notify();
}
VisualRuntimeOwner::Runtime* VisualRuntimeOwner::RuntimeForWork(std::stop_token worker_stop, std::stop_token operation_stop) {
    const auto stopped = [&] { return worker_stop.stop_requested() || operation_stop.stop_requested(); };
    if (stopped()) return nullptr;
    auto& destination = replacement_active_ ? replacement_ : runtime_;
    if (!destination) {
        if (replacement_active_) RestorePolicy();
        auto created = factory_(product_revision_sequence_);
        if (!created) throw std::runtime_error("visual runtime factory returned no runtime");
        {
            std::scoped_lock lock(mutex_);
            if (!stopping_ && !stopped()) destination = std::move(created);
        }
        if (created) {
            if (auto failure = RetireOwned(std::move(created))) std::rethrow_exception(failure);
            return nullptr;
        }
    }
    if (stopped()) return nullptr;
    if (!execution_policy_) {
        if (const auto* execution = destination->execution())
            execution_policy_.emplace(mmltk::common::system::ExecutionPolicyRequest{
                execution->placement.cpus, {}, 0, execution->placement.numa_node, -10, false});
    }
    if (stopped()) return nullptr;
    destination->BeginWork();
    return stopped() ? nullptr : destination.get();
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
        active_outcome_ = ActiveOutcome::Running;
        active_discrete_ = discrete;
        operation_stop = active_stop_.get_token();
    }
    Observe(ActivityStage::WorkSelected, ordered_work ? 1U : (latest_work ? 2U : (continuation_work ? 3U : 0U)));
    if (ordered_work && (worker_stop.stop_requested() || operation_stop.stop_requested())) {
        notification = std::move(ordered_work->cancellation);
    } else if (worker_stop.stop_requested() || operation_stop.stop_requested()) {
        notification = {};
    } else if (ordered_work) {
        if (ordered_work->reconstruct) {
            StagedReplacement replacement(*this);
            try {
                if (auto* runtime = RuntimeForWork(worker_stop, operation_stop);
                    runtime && !worker_stop.stop_requested() && !operation_stop.stop_requested())
                    notification = ordered_work->run(*runtime, operation_stop);
                const bool completed = TryCompleteActiveWork();
                const bool promote = completed && replacement_ && replacement_->Completed().valid();
                if (!completed) notification = std::move(ordered_work->cancellation);
                Observe(ActivityStage::StagedCompletionLatched, completed ? 1U : 0U);
                if (auto failure = replacement.Finish(promote)) {
                    ReportFailure(failure);
                    return;
                }
            } catch (...) {
                auto failure = std::current_exception();
                if (auto custody = Runtime::UnsafeConstruction(failure)) {
                    failure = custody->failure();
                    std::scoped_lock lock(mutex_);
                    retained_.emplace<Runtime::UnsafeCustody>(std::move(*custody));
                }
                failure = combine_image_failures(failure, replacement.Finish(false));
                ReportFailure(failure);
                return;
            }
        } else {
            if (auto* runtime = RuntimeForWork(worker_stop, operation_stop);
                runtime && !worker_stop.stop_requested() && !operation_stop.stop_requested())
                notification = ordered_work->run(*runtime, operation_stop);
            else
                notification = std::move(ordered_work->cancellation);
        }
    } else if (latest_work) {
        if (auto* runtime = RuntimeForWork(worker_stop, operation_stop);
            runtime && !worker_stop.stop_requested() && !operation_stop.stop_requested())
            notification = (*latest_work)(*runtime, operation_stop);
    } else {
        if (auto* runtime = RuntimeForWork(worker_stop, operation_stop);
            runtime && !worker_stop.stop_requested() && !operation_stop.stop_requested()) {
            if (continuation_dispatched_) continuation_dispatched_();
            if (!worker_stop.stop_requested() && !operation_stop.stop_requested())
                notification = continuation_(*runtime, operation_stop);
        }
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
    NotifyReaders();
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
    auto reported = construction_custody ? construction_custody->failure() : failure;
    std::unique_ptr<Runtime> retired;
    {
        std::scoped_lock lock(mutex_);
        retired = std::move(runtime_);
        if (construction_custody) retained_.emplace<Runtime::UnsafeCustody>(std::move(*construction_custody));
        runtime_retirement_blocked_ = true;
    }
    reported = combine_image_failures(reported, RetireOwned(std::move(retired)));
    ReportFailure(reported);
}

void VisualRuntimeOwner::ReportFailure(std::exception_ptr failure) noexcept {
    {
        std::scoped_lock lock(mutex_);
        ordered_.clear();
        latest_.reset();
        continuation_state_.store(0U, std::memory_order_release);
        discrete_active_ = false;
        terminal_barrier_active_ = false;
        active_stop_ = std::stop_source{std::nostopstate};
        active_discrete_ = false;
        runtime_retirement_blocked_ = stopping_ || retained_.index() != 0U || execution_policy_.has_value();
        if (continuation_ && !runtime_retirement_blocked_)
            continuation_state_.store(kContinuationEnabled, std::memory_order_release);
    }
    NotifyReaders();
    if (!failures_) return;
    try {
        failures_(failure);
    } catch (...) { failures_ = {}; }
}

void VisualRuntimeOwner::RestorePolicy() {
    if (execution_policy_) {
        execution_policy_->Restore();
        execution_policy_.reset();
    }
}

std::exception_ptr VisualRuntimeOwner::RetireOwned(std::unique_ptr<Runtime> retired) noexcept {
    Observe(ActivityStage::RetirementStarted);
    std::exception_ptr failure;
    bool safe = true;
    if (retired) {
        auto retirement = retired->Retire();
        failure = retirement.failure;
        safe = retirement.safe_to_destroy;
        if (!safe) {
            std::scoped_lock lock(mutex_);
            retained_.emplace<Runtime::UnsafeCustody>(std::move(retirement.custody));
        }
        retired.reset();
    }
    try {
        RestorePolicy();
    } catch (...) { failure = combine_image_failures(failure, std::current_exception()); }
    {
        std::scoped_lock lock(mutex_);
        runtime_retirement_blocked_ = stopping_ || retained_.index() != 0U || execution_policy_.has_value();
    }
    Observe(ActivityStage::RetirementCompleted, safe ? 1U : 0U);
    return failure;
}

VisualRuntimeOwner::StagedReplacement::StagedReplacement(VisualRuntimeOwner& owner) : owner_(&owner) {
    if (owner.replacement_active_) throw std::logic_error("visual runtime replacement is already active");
    owner.replacement_active_ = true;
}
VisualRuntimeOwner::StagedReplacement::~StagedReplacement() {
    if (owner_) {
        auto* owner = owner_;
        if (auto failure = Finish(false)) owner->ReportFailure(failure);
    }
}
std::exception_ptr VisualRuntimeOwner::StagedReplacement::Finish(bool promote) noexcept {
    if (!owner_) return {};
    return std::exchange(owner_, nullptr)->FinishRuntimeReplacement(promote);
}
std::exception_ptr VisualRuntimeOwner::FinishRuntimeReplacement(bool promote) noexcept {
    std::unique_ptr<Runtime> retired;
    {
        std::scoped_lock lock(mutex_);
        if (promote) {
            retired = std::move(runtime_);
            runtime_ = std::move(replacement_);
        } else {
            retired = std::move(replacement_);
        }
        replacement_active_ = false;
    }
    return RetireOwned(std::move(retired));
}
void VisualRuntimeOwner::RetireRuntime() {
    std::unique_ptr<Runtime> retired;
    std::unique_ptr<Runtime> replacement;
    {
        std::scoped_lock lock(mutex_);
        retired = std::move(runtime_);
        replacement = std::move(replacement_);
        replacement_active_ = false;
        runtime_retirement_blocked_ = true;
    }
    std::exception_ptr failure;
    if (replacement) failure = RetireOwned(std::move(replacement));
    failure = combine_image_failures(failure, RetireOwned(std::move(retired)));
    if (failure) std::rethrow_exception(failure);
}

}  // namespace mmltk::controller::detail
