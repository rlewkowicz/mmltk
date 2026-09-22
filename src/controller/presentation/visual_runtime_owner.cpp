#include "src/controller/presentation/visual_runtime_owner.h"
#include <algorithm>
#include <stdexcept>
#include <utility>
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/frameworks/gpu/image_failure.h"
namespace mmltk::controller {
mmltk::frameworks::gpu::BorrowedImageProductReadView borrow_matching_visual_product(const VisualFrame& frame, const detail::VisualRuntimeOwner& owner) {
 return borrow_matching_visual_product(frame, owner.Borrow());
}
}  // namespace mmltk::controller
namespace mmltk::controller::detail {
using mmltk::frameworks::gpu::combine_image_failures;
struct VisualRuntimeOwner::OutputWake final {
 std::mutex mutex;
 VisualRuntimeOwner* owner = nullptr;
 bool retry_armed = false;
};
VisualRuntimeOwner::VisualRuntimeOwner(RuntimeFactory factory, FailureSink failures, ActivityObservation activity)
    : factory_(std::move(factory)),
      failures_(std::move(failures)),
      activity_(std::move(activity)),
      worker_([this](const std::stop_token stop) { Run(stop); }, [this](const std::exception_ptr failure) { Failed(failure); }, [this] { RetireRuntime(); }) {
 if (!factory_) throw std::invalid_argument("visual runtime factory is unavailable");
 output_wake_ = std::make_shared<OutputWake>();
 output_wake_->owner = this;
 retirement_sink_ = std::make_shared<const std::function<void()>>([wake = output_wake_] {
  std::scoped_lock lock(wake->mutex);
  if (wake->owner) {
   wake->owner->retirement_ready_.store(true, std::memory_order_release);
   wake->owner->worker_.Wake();
  }
 });
}
VisualRuntimeOwner::~VisualRuntimeOwner() {
 if (output_wake_) {
  std::scoped_lock lock(output_wake_->mutex);
  output_wake_->retry_armed = false;
  output_wake_->owner = nullptr;
 }
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
void VisualRuntimeOwner::RegisterOrderedDrain(Work work) {
 if (!work) throw std::invalid_argument("visual ordered drain is empty");
 std::scoped_lock lock(mutex_);
 if (ordered_drain_ || stopping_) throw std::logic_error("visual ordered drain registration is unavailable");
 drain_stop_ = std::stop_source{};
 ordered_drain_ = std::move(work);
}
bool VisualRuntimeOwner::NotifyOrderedDrain() {
 {
  std::scoped_lock lock(mutex_);
  if (!ordered_drain_ || stopping_ || runtime_retirement_blocked_ || terminal_barrier_active_) return false;
  if (drain_queued_) return true;
  FlushLatest();
  ordered_.push_back(ScheduledWork{
   .run = {},
   .cancellation = {},
   .dispatched = {},
   .stop = drain_stop_,
   .ordered_drain = true,
   .discrete = false,
   .terminal_barrier = false,
   .reconstruct = false,
  });
  drain_queued_ = true;
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
void VisualRuntimeOwner::RegisterContinuation(Work work, DispatchObservation dispatched, const bool wake_on_output_available, const ContinuationCancellation cancellation) {
 if (!work) throw std::invalid_argument("visual continuation work is empty");
 std::scoped_lock lock(mutex_);
 if (continuation_ || stopping_ || terminal_barrier_active_ || runtime_retirement_blocked_ || (wake_on_output_available && runtime_))
  throw std::logic_error("visual continuation registration is unavailable");
 output_notifications_ = wake_on_output_available;
 continuation_ = std::move(work);
 continuation_dispatched_ = std::move(dispatched);
 continuation_cancellation_ = cancellation;
 continuation_state_.store(kContinuationEnabled, std::memory_order_release);
}
bool VisualRuntimeOwner::NotifyContinuation() noexcept {
 const auto state = continuation_state_.fetch_or(kContinuationPending, std::memory_order_acq_rel);
 if ((state & kContinuationEnabled) == 0U) return false;
 if ((state & kContinuationPending) == 0U) worker_.Wake();
 return true;
}
void VisualRuntimeOwner::NotifyContinuationAt(const std::chrono::steady_clock::time_point deadline) noexcept {
 {
  std::scoped_lock lock(mutex_);
  if (stopping_ || (continuation_state_.load(std::memory_order_acquire) & kContinuationEnabled) == 0U) return;
  continuation_deadline_ = deadline;
 }
 worker_.WakeAt(deadline);
}
void VisualRuntimeOwner::SetOutputRetry(const bool armed) noexcept {
 if (!output_notifications_) return;
 std::scoped_lock lock(output_wake_->mutex);
 output_wake_->retry_armed = armed && (continuation_state_.load(std::memory_order_acquire) & kContinuationEnabled) != 0U;
 if (!output_wake_->retry_armed) continuation_state_.fetch_and(static_cast<std::uint8_t>(~kOutputRetryPending), std::memory_order_acq_rel);
}
VisualRuntimeOwner::Runtime::OutputCandidate VisualRuntimeOwner::TryAcquireOutput(Runtime& runtime, Runtime::CompletedOutput& baseline, mmltk::frameworks::gpu::ImagePlanePreservation preservation) {
 // Arm before observing the pool: a receiver may release the last occupied
 // allocation between the failed reservation and returning to the worker.
 SetOutputRetry(true);
 auto candidate = runtime.TryAcquireOutput(baseline, preservation);
 if (candidate.valid()) SetOutputRetry(false);
 return candidate;
}
void VisualRuntimeOwner::DeferCompletion(Runtime& runtime, Notification completed) {
 if (completion_ || !completed) throw std::logic_error("visual GPU completion custody is unavailable");
 completion_ = std::move(completed);
 completion_runtime_ = &runtime;
 completion_ready_.store(false, std::memory_order_release);
 runtime.NotifyWorkCompletion([wake = std::weak_ptr{output_wake_}] {
  if (const auto gate = wake.lock()) {
   std::scoped_lock lock(gate->mutex);
   if (gate->owner) {
    gate->owner->completion_ready_.store(true, std::memory_order_release);
    gate->owner->worker_.Wake();
   }
  }
 });
}
bool VisualRuntimeOwner::RequestActiveStop() noexcept {
 std::stop_source stop{std::nostopstate};
 Notification cancellation;
 {
  std::scoped_lock lock(mutex_);
  if (!active_preserves_input_ && active_outcome_ == ActiveOutcome::Running) {
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
  drain_queued_ = false;
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
 SetOutputRetry(false);
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
void VisualRuntimeOwner::FinishStoppedRetirement() noexcept {
 if (!worker_.stopped()) std::terminate();
 if (auto failure = FinishDeferredRetirement()) ReportFailure(failure);
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
void VisualRuntimeOwner::SelectOutput(const Runtime::CompletedOutput& product) {
 std::scoped_lock lock(mutex_);
 if (runtime_) runtime_->SelectOutput(product);
}
mmltk::frameworks::gpu::BorrowedImageWorkspace VisualRuntimeOwner::BorrowWorkspace() const {
 std::unique_lock lock(mutex_, std::try_to_lock);
 return lock.owns_lock() && runtime_ ? runtime_->BorrowWorkspace() : mmltk::frameworks::gpu::BorrowedImageWorkspace{};
}
mmltk::frameworks::gpu::ImageWorkspaceObservation VisualRuntimeOwner::ObserveWorkspace() const {
 std::scoped_lock lock(mutex_);
 if (!runtime_) return {};
 return runtime_->ObserveWorkspace();
}
namespace {
void diagnose_workspace_service(const VisualWorkspaceRequest& request, std::string_view reason, const mmltk::frameworks::gpu::ImageWorkspaceObservation& observed = {},
 const mmltk::frameworks::gpu::ImageWorkspaceObservation& candidate = {}) noexcept {
 const auto destination = request.destination.lock();
 if (!request.diagnostics || !destination) return;
 request.diagnostics->sink.Emit([&] {
  VisualDiagnosticFact fact{.system = contracts::DiagnosticOwner::Presentation,
   .operation = VisualDiagnosticOperation::PresentationWorkspaceService,
   .device = destination->layout().device,
   .context = request.diagnostics->context,
   .failure_detail = reason};
  auto& progress = fact.context.workspace_progress;
  progress.requested_product_owner = request.product_owner;
  progress.requested_product_revision = request.product_revision;
  progress.observed_product_owner = observed.product_owner;
  progress.observed_product_revision = observed.product_revision;
  progress.admitted_allocation = destination->identity();
  progress.candidate_allocation = candidate.workspace ? candidate.workspace->identity() : 0U;
  progress.candidate_product_owner = candidate.product_owner;
  if (candidate.workspace) {
   progress.candidate_admitted = candidate.workspace->admitted();
   progress.candidate_write_available = candidate.workspace->WriteAvailable();
  }
  progress.expected_workspace_pitch = destination->layout().pitch_bytes;
  progress.expected_workspace_bytes = destination->layout().required_allocation_bytes;
  progress.expected_device_incarnation = destination->layout().device_incarnation;
  if (observed.workspace) {
   const auto& workspace = *observed.workspace;
   observe_workspace_storage(fact.context, workspace);
   progress.workspace_layout_matches = workspace.layout() == destination->layout();
  }
  return fact;
 });
}
}  // namespace
void VisualRuntimeOwner::RequestWorkspace(VisualWorkspaceRequest request) {
 const auto destination = request.destination.lock();
 if (!request.ready || (!request.detach_only && (request.product_owner == 0U || request.product_revision == 0U))) throw std::invalid_argument("visual workspace request is incomplete");
 if (!destination) {
  request.ready();
  return;
 }
 if (!destination->layout().valid()) throw std::invalid_argument("visual workspace destination is invalid");
 std::optional<VisualWorkspaceRequest> displaced;
 std::stop_source background_stop{std::nostopstate};
 {
  std::scoped_lock lock(mutex_);
  if (stopping_ || runtime_retirement_blocked_) {
   diagnose_workspace_service(request, "request_rejected_stopping");
   request.ready();
   return;
  }
  if (workspace_request_ && workspace_request_->diagnostics && workspace_request_->diagnostics->sink.valid() && workspace_pending_.load(std::memory_order_acquire))
   diagnose_workspace_service(*workspace_request_, "pending_request_replaced");
  diagnose_workspace_service(request, "request_enqueued");
  displaced = std::exchange(workspace_request_, std::move(request));
  workspace_retry_.store(false, std::memory_order_release);
  workspace_pending_.store(true, std::memory_order_release);
  if (active_yields_to_workspace_ && active_outcome_ == ActiveOutcome::Running) {
   active_outcome_ = ActiveOutcome::Cancelled;
   background_stop = active_stop_;
  }
 }
 static_cast<void>(background_stop.request_stop());
 if (displaced) displaced->ready();
 worker_.Wake();
}
void VisualRuntimeOwner::ServiceWorkspace() {
 if (!workspace_pending_.exchange(false, std::memory_order_acq_rel)) return;
 std::optional<VisualWorkspaceRequest> request;
 {
  std::scoped_lock lock(mutex_);
  request = std::exchange(workspace_request_, std::nullopt);
 }
 if (!request) return;
 const auto destination = request->destination.lock();
 if (!destination) {
  request->ready();
  return;
 }
 try {
  if (runtime_ && !runtime_retirement_blocked_ && !stopping_ && (request->detach_only || !destination->retired())) {
   runtime_->BeginWork();
   diagnose_workspace_service(*request, "prepare_started");
   workspace_retry_.store(true, std::memory_order_release);
   const auto observed = runtime_->ObserveWorkspace();
   const bool expected_product = observed.product_owner == request->product_owner && observed.product_revision == request->product_revision;
   const bool prepared = request->detach_only ? runtime_->DetachDisplay(destination) : expected_product && runtime_->PrepareDisplay(request->product_revision, destination);
   if (!prepared) {
    const bool retry = request->detach_only || expected_product;
    if (retry) {
     std::scoped_lock lock(mutex_);
     if (!workspace_request_) {
      workspace_request_ = std::move(request);
      return;
     }
    }
    if (!request->detach_only) destination->CancelDisplayWrite();
   }
   workspace_retry_.store(false, std::memory_order_release);
   diagnose_workspace_service(*request, "prepare_completed");
  }
 } catch (...) {
  workspace_retry_.store(false, std::memory_order_release);
  request->ready();
  throw;
 }
 request->ready();
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
  created->SetOutputAvailableSink([wake = std::weak_ptr{output_wake_}] {
   if (const auto gate = wake.lock()) {
    std::scoped_lock lock(gate->mutex);
    if (gate->owner) gate->owner->worker_.Wake();
    if (gate->owner && gate->owner->workspace_retry_.load(std::memory_order_acquire)) {
     gate->owner->workspace_pending_.store(true, std::memory_order_release);
     gate->owner->worker_.Wake();
    }
    if (gate->owner && gate->retry_armed) {
     const auto prior = gate->owner->continuation_state_.fetch_or(kOutputRetryPending, std::memory_order_acq_rel);
     if ((prior & kContinuationEnabled) != 0U && (prior & (kContinuationPending | kOutputRetryPending)) == 0U) gate->owner->worker_.Wake();
    }
   }
  });
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
   execution_policy_.emplace(mmltk::common::system::ExecutionPolicyRequest{execution->placement.cpus, {}, 0, execution->placement.numa_node, -10, false});
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
 if (auto failure = FinishDeferredRetirement()) ReportFailure(failure);
 if (completion_) {
  if (!completion_ready_.load(std::memory_order_acquire)) return;
  completion_runtime_->CompleteWork();
  auto completed = std::move(completion_);
  completion_runtime_ = nullptr;
  if (!worker_stop.stop_requested()) completed();
 }
 if (runtime_) runtime_->CompleteWorkspaces();
 ServiceWorkspace();
 std::optional<ScheduledWork> ordered_work;
 std::optional<Work> latest_work;
 bool continuation_work = false;
 Notification notification;
 bool discrete = false;
 bool terminal_barrier = false;
 std::stop_token operation_stop;
 {
  std::scoped_lock lock(mutex_);
  if (stopping_ || runtime_retirement_blocked_) return;
  if (continuation_deadline_ && std::chrono::steady_clock::now() >= *continuation_deadline_) {
   continuation_deadline_.reset();
   continuation_state_.fetch_or(kContinuationPending, std::memory_order_release);
  } else if (continuation_deadline_) {
   worker_.WakeAt(*continuation_deadline_);
  }
  if (!ordered_.empty()) {
   ordered_work.emplace(std::move(ordered_.front()));
   ordered_.pop_front();
   if (ordered_work->ordered_drain) drain_queued_ = false;
   discrete = ordered_work->discrete;
   terminal_barrier = ordered_work->terminal_barrier;
  } else if (latest_) {
   latest_work = std::move(latest_);
   latest_.reset();
  } else if (continuation_cancellation_ == ContinuationCancellation::YieldToWorkspace && (workspace_pending_.load(std::memory_order_acquire) || workspace_retry_.load(std::memory_order_acquire))) {
   // A physical completion or new request wakes us. Keep background
   // work queued without delaying that completion or polling for it.
   return;
  } else if (const auto pending = continuation_state_.fetch_and(static_cast<std::uint8_t>(~(kContinuationPending | kOutputRetryPending)), std::memory_order_acq_rel);
   (pending & kContinuationEnabled) != 0U && (pending & (kContinuationPending | kOutputRetryPending)) != 0U) {
   continuation_work = true;
  } else {
   return;
  }
  active_stop_ = ordered_work ? ordered_work->stop : std::stop_source{};
  active_outcome_ = ActiveOutcome::Running;
  active_discrete_ = discrete;
  active_preserves_input_ =
   (ordered_work && (ordered_work->ordered_drain || ordered_work->terminal_barrier)) || (continuation_work && continuation_cancellation_ == ContinuationCancellation::PreserveOrderedInput);
  active_yields_to_workspace_ = continuation_work && continuation_cancellation_ == ContinuationCancellation::YieldToWorkspace;
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
    if (auto* runtime = RuntimeForWork(worker_stop, operation_stop); runtime && !worker_stop.stop_requested() && !operation_stop.stop_requested())
     notification = ordered_work->run(*runtime, operation_stop);
    const bool completed = TryCompleteActiveWork();
    const bool promote = completed && replacement_ && replacement_->OutputFacts().revision != 0U;
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
   if (auto* runtime = RuntimeForWork(worker_stop, operation_stop); runtime && !worker_stop.stop_requested() && !operation_stop.stop_requested())
    notification = ordered_work->ordered_drain ? ordered_drain_(*runtime, operation_stop) : ordered_work->run(*runtime, operation_stop);
   else
    notification = std::move(ordered_work->cancellation);
  }
 } else if (latest_work) {
  if (auto* runtime = RuntimeForWork(worker_stop, operation_stop); runtime && !worker_stop.stop_requested() && !operation_stop.stop_requested())
   notification = (*latest_work)(*runtime, operation_stop);
 } else {
  if (auto* runtime = RuntimeForWork(worker_stop, operation_stop); runtime && !worker_stop.stop_requested() && !operation_stop.stop_requested()) {
   if (continuation_dispatched_) continuation_dispatched_();
   if (!worker_stop.stop_requested() && !operation_stop.stop_requested()) notification = continuation_(*runtime, operation_stop);
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
  active_preserves_input_ = false;
  active_yields_to_workspace_ = false;
  wake_again = !ordered_.empty() || latest_.has_value() || (continuation_state_.load(std::memory_order_acquire) & (kContinuationPending | kOutputRetryPending)) != 0U;
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
 SetOutputRetry(false);
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
 SetOutputRetry(false);
 {
  std::scoped_lock lock(mutex_);
  ordered_.clear();
  drain_queued_ = false;
  latest_.reset();
  continuation_state_.store(0U, std::memory_order_release);
  discrete_active_ = false;
  terminal_barrier_active_ = false;
  active_stop_ = std::stop_source{std::nostopstate};
  active_discrete_ = false;
  active_preserves_input_ = false;
  runtime_retirement_blocked_ = stopping_ || retained_.index() != 0U || execution_policy_.has_value();
  if (continuation_ && !runtime_retirement_blocked_) continuation_state_.store(kContinuationEnabled, std::memory_order_release);
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
 std::function<void()> workspace_ready;
 {
  std::scoped_lock lock(mutex_);
  if (workspace_request_) workspace_ready = std::move(workspace_request_->ready);
  workspace_request_.reset();
  workspace_pending_.store(false, std::memory_order_release);
  workspace_retry_.store(false, std::memory_order_release);
 }
 std::exception_ptr failure;
 bool safe = true;
 if (retired) {
  auto retirement = retired->Retire();
  failure = retirement.failure;
  safe = retirement.safe_to_destroy;
  if (completion_runtime_ == retired.get()) {
   // Retire establishes physical settlement or terminal custody first.
   // Only then may the deferred candidate abandon its reservation.
   completion_ = {};
   completion_runtime_ = nullptr;
   if (!safe && retirement.custody.deferred()) {
    const auto finished = retirement.custody.FinishRetirement();
    failure = combine_image_failures(failure, finished.failure);
    safe = finished.completion_reached;
   }
  }
  if (!safe) {
   if (retirement.custody.deferred()) retirement.custody.SetRetirementSink(retirement_sink_);
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
 if (workspace_ready) {
  try {
   workspace_ready();
  } catch (...) {}
 }
 return failure;
}
std::exception_ptr VisualRuntimeOwner::FinishDeferredRetirement() noexcept {
 if (!retirement_ready_.exchange(false, std::memory_order_acq_rel)) return {};
 Runtime::UnsafeCustody custody;
 {
  std::scoped_lock lock(mutex_);
  auto* retained = std::get_if<Runtime::UnsafeCustody>(&retained_);
  if (!retained || !retained->deferred()) return {};
  custody = std::move(*retained);
 }
 const auto settled = custody.FinishRetirement();
 {
  std::scoped_lock lock(mutex_);
  if (settled.completion_reached) {
   retained_.emplace<std::monostate>();
   runtime_retirement_blocked_ = stopping_ || execution_policy_.has_value();
   if (continuation_ && !runtime_retirement_blocked_) continuation_state_.fetch_or(kContinuationEnabled, std::memory_order_release);
  } else {
   retained_.emplace<Runtime::UnsafeCustody>(std::move(custody));
  }
 }
 return settled.failure;
}
VisualRuntimeOwner::StagedReplacement::StagedReplacement(VisualRuntimeOwner& owner) : owner_(&owner) {
 if (owner.replacement_active_) throw std::logic_error("visual runtime replacement is already active");
 owner.SetOutputRetry(false);
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
 SetOutputRetry(false);
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
