#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/detail/compile_observation.h"
#include "src/backend/data/detail/compile_admission.h"
#include "src/backend/data/detail/compile_workspace.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/io/scoped_fd.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <dirent.h>
#include <limits>
#include <list>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
namespace mmltk::backend::data::detail {
namespace {
constexpr auto class_count = static_cast<std::size_t>(CompileWorkClass::Count);
constexpr auto kind_count = static_cast<std::size_t>(CompileTaskKind::Count);
std::size_t index(CompileWorkClass value) { return static_cast<std::size_t>(value); }
std::size_t descriptor_headroom() {
 rlimit limit{};
 if (::getrlimit(RLIMIT_NOFILE, &limit)) throw std::system_error(errno, std::generic_category(), "cannot inspect compile descriptor limit");
 std::size_t opened = 0;
 std::unique_ptr<DIR, int (*)(DIR*)> directory(::opendir("/proc/self/fd"), ::closedir);
 if (!directory) throw std::system_error(errno, std::generic_category(), "cannot inspect compile descriptors");
 while (const auto* entry = ::readdir(directory.get()))
  if (entry->d_name[0] != '.') ++opened;
 const auto available = limit.rlim_cur == RLIM_INFINITY ? std::size_t{4096} : static_cast<std::size_t>(std::min<rlim_t>(limit.rlim_cur, 4096));
 if (available <= opened + 16) throw InsufficientCompileResources("insufficient descriptor headroom for compilation");
 return available - opened - 16;
}
void signal_event(int descriptor) noexcept {
 const std::uint64_t value = 1;
 while (::write(descriptor, &value, sizeof(value)) < 0 && errno == EINTR) {}
}
}  // namespace
struct CompileExecution::CompletionState {
 mutable std::mutex mutex;
 std::condition_variable changed;
 bool done = false;
 std::exception_ptr error;
 std::vector<std::function<void(std::exception_ptr)>> callbacks;
 std::weak_ptr<Impl> owner;
 std::weak_ptr<Job> job;
};
struct CompileExecution::DependencyState {
 std::weak_ptr<Impl> owner;
 std::uint64_t generation = 0;
 std::vector<std::weak_ptr<Job>> waiting;
};
struct CompileExecution::Demand {
 CompileResources resources;
 CompileAllowance parent;
 bool finite_consumer = false, needs_record = false;
 std::uint64_t blocked_generation = std::numeric_limits<std::uint64_t>::max();
 bool resource_fits = false;
 CompileResourceDenial denial = CompileResourceDenial::None;
 std::uint64_t age = 0;
 struct Link { Demand* previous = nullptr; Demand* next = nullptr; bool linked = false; } aged;
};
struct CompileResourceWait::State {
 std::shared_ptr<CompileExecution::Impl> execution;
 CompileExecution::Demand demand;
 CompileTaskKind successor;
 struct Link { State* previous = nullptr; State* next = nullptr; bool linked = false; } exceptional;
 bool registered = false;
 State(std::shared_ptr<CompileExecution::Impl> owner, CompileTaskKind kind, CompileResources resources, CompileAllowance parent, bool finite)
     : execution(std::move(owner)), demand{.resources = resources, .parent = std::move(parent), .finite_consumer = finite, .needs_record = kind != CompileTaskKind::Count}, successor(kind) {}
};
struct CompileExecution::Job {
 Task task;
 std::shared_ptr<CompletionState> completion;
 std::shared_ptr<DependencyState> dependency;
 CompileParkReason reason = CompileParkReason::None;
 Demand demand;
 CompileResourceDenial denial = CompileResourceDenial::None;
 std::uint64_t observed_epoch = 0;
 std::uint64_t observed_extent = 0;
 std::chrono::steady_clock::time_point ready_at{};
 bool running = false, cancelled = false, done = false;
 struct Link { Job* previous = nullptr; Job* next = nullptr; bool linked = false; };
 Link exceptional;
 bool retained = false;
 CompileTaskKind successor = CompileTaskKind::Count;
};
struct CompileExecution::Readiness::State {
 std::mutex mutex;
 std::condition_variable changed;
 std::function<void(Changes)> callback;
 // Pending notices are published and acknowledged under admission->mutex.
 Changes pending;
 CompileChange interests = CompileChange::None;
 bool notified = false;
 bool active = false, removed = false;
};
struct CompileExecution::Impl {
 struct Class {
  std::size_t producers = 0, pending = 0, running = 0, kind_cursor = 0;
 };
 struct Idle {
  Workspace* workspace = nullptr;
  CompileAllowance allowance;
 };
 struct Frame;
 struct Lane {
  Frame* active = nullptr;
  Workspace* retiring = nullptr;
  Idle idle;
  CompileWorkClass baseline = CompileWorkClass::Count, service = CompileWorkClass::Count, completing = CompileWorkClass::Count;
  bool dispatching = false, external = false;
  [[nodiscard]] bool busy() const { return active || retiring || service != CompileWorkClass::Count || dispatching || external; }
  [[nodiscard]] bool occupied() const { return busy() || completing != CompileWorkClass::Count; }
  [[nodiscard]] bool owns(Workspace*) const;
 };
 struct Frame {
  Impl& owner;
  std::size_t lane;
  CompileWorkClass grant_class;
  Frame* parent = nullptr;
  Frame* previous = nullptr;
  Workspace* workspace = nullptr;
  CompileAllowance allowance;
  std::shared_ptr<Credits> credit;
  Job* job = nullptr;
  static thread_local Frame* current;
  Frame(Impl& value, std::size_t slot, CompileWorkClass kind) : owner(value), lane(slot), grant_class(kind) {}
  void enter(CompileAllowance value, Workspace* scratch) {
   auto& local = owner.lanes[lane];
   parent = local.active;
   if (parent) grant_class = parent->grant_class;
   previous = current;
   workspace = scratch;
   allowance = std::move(value);
   credit = allowance.credits_;
   if (credit) ++credit->cpu_users;
   local.active = this;
   if (!parent && !local.dispatching) ++owner.admission->active;
   local.dispatching = false;
   current = this;
   owner.reconcile();
   owner.notify_locked();
  }
  ~Frame() {
   std::shared_ptr<Credits> retired_parent;
   {
    const std::lock_guard lock(owner.admission->mutex);
    if (credit) {
     --credit->cpu_users;
     // Publish the final frame-only byte return before removing its ownership
     // record. Otherwise another lane can mistake these still-charged bytes for
     // retained input and temporarily discard an aged demand's reservation.
     if (!credit->cpu_users && credit->resources.bytes && !credit->resources.retained_handles && credit.use_count() == (allowance.credits_ == credit ? 2 : 1))
      credit->workspace_retirement_pending = true;
     retired_parent = owner.admission->retire_workspace(*credit);
    }
    owner.lanes[lane].active = parent;
    if (!parent) --owner.admission->active;
    current = previous;
    owner.reconcile();
    owner.notify_locked();
   }
  }
 };
 std::shared_ptr<CompileAdmission> admission = std::make_shared<CompileAdmission>();
 std::vector<int> cpus;
 std::vector<Lane> lanes;
 std::array<Class, class_count> classes;
 struct ReadyQueue {
  std::list<std::shared_ptr<Job>> jobs;
  decltype(jobs)::iterator fallback = jobs.end();
  auto erase(decltype(jobs)::iterator position) {
   if (fallback == position) fallback = std::next(position);
   return jobs.erase(position);
  }
 };
 // Nonowning indexes of existing bounded records. No per-probe allocation or
 // full-bank discovery is needed for exceptional drainage or aged byte demand.
 template <class Entry> struct DemandQueue {
  typename Entry::Link Entry::* link;
  Entry* head = nullptr;
  Entry* tail = nullptr;
  void remove(Entry& job) noexcept {
   auto& value = job.*link;
   if (!value.linked) return;
   if (value.previous) (value.previous->*link).next = value.next;
   else head = value.next;
   if (value.next) (value.next->*link).previous = value.previous;
   else tail = value.previous;
   value = {};
  }
  void append(Entry& job) noexcept {
   auto& value = job.*link;
   if (value.linked) return;
   value.previous = tail;
   value.linked = true;
   if (tail) (tail->*link).next = &job;
   else head = &job;
   tail = &job;
  }
 };
 DemandQueue<Job> exceptional{&Job::exceptional};
 DemandQueue<CompileResourceWait::State> external_demands{&CompileResourceWait::State::exceptional};
 DemandQueue<Demand> aged_demands{&Demand::aged};
 const void* draining_request = nullptr;
 bool draining_external = false;
 Demand* aged_candidate = nullptr;
 Demand* reserved_demand = nullptr;
 std::uint64_t selections = 0, demand_revision = 0;
 std::uint64_t inspected_revision = std::numeric_limits<std::uint64_t>::max();
 std::uint64_t inspected_resources = 0, inspected_reclaimable = 0, inspected_arena = 0;
 std::uint32_t inspected_records = 0;
 void remove_demand(Demand& demand) noexcept {
  if (demand.aged.linked) ++demand_revision;
  aged_demands.remove(demand);
  if (aged_candidate == &demand) aged_candidate = nullptr;
  if (reserved_demand == &demand) {
   reserved_demand = nullptr;
   notify_locked(CompileChange::Fairness);
  }
 }
 void unindex(Job& job) noexcept {
  exceptional.remove(job);
  remove_demand(job.demand);
 }
 void wait_demand(Demand& demand) noexcept {
  if (demand.aged.linked) return;
  demand.age = selections;
  aged_demands.append(demand);
  ++demand_revision;
 }
 void drain(const void* request, const Credits* parent, std::uint64_t bytes, bool external = false) noexcept {
  if (request == draining_request) return;
  draining_request = request;
  draining_external = external;
  admission->draining_parent = parent;
  admission->draining_bytes = bytes;
  notify_locked(CompileChange::Resources);
 }
 void refresh_external_drain() noexcept {
  // A queued exceptional request that already owns the turn keeps it. Among
  // external requests retain order, skipping successors without record room.
  if (draining_request && !draining_external) return;
  auto* waiting = external_demands.head;
  const auto records = waiting && waiting->demand.needs_record ? record_capacity() : 0;
  while (waiting && !record_eligible(waiting->demand, records)) waiting = waiting->exceptional.next;
  drain(waiting, waiting ? waiting->demand.parent.credits_.get() : nullptr, waiting ? waiting->demand.resources.bytes : 0, waiting != nullptr);
 }
 void remove_wait(CompileResourceWait::State& waiting) noexcept {
  if (!std::exchange(waiting.registered, false)) return;
  remove_demand(waiting.demand);
  if (waiting.exceptional.linked) {
   external_demands.remove(waiting);
   if (draining_request == &waiting) drain(nullptr, nullptr, 0);
   refresh_external_drain();
  }
  admission->change_waiters(false, true);
 }
 std::array<ReadyQueue, kind_count> ready;
 std::list<std::shared_ptr<Job>> jobs;
 std::vector<std::shared_ptr<Job>> available_records;
 std::vector<std::shared_ptr<Readiness::State>> observers;
 std::size_t cursor = 0, record_limit = 0, services = 0;
 std::uint64_t admitted = 0, completed = 0;
 std::array<std::uint64_t, kind_count> task_completed{}, task_duration_ns{}, task_max_duration_ns{};
 std::array<std::uint64_t, kind_count> task_body_ns{}, task_cpu_ns{}, ready_to_start_ns{};
 CompileExecutionCosts costs{};
 bool stopping = false, shutdown = false, floors = true, withdrawal_pending = false;
 std::array<std::size_t, class_count> service_waiters{};
 std::exception_ptr failure;
 std::exception_ptr cancellation_error = std::make_exception_ptr(std::runtime_error("dataset compilation cancelled"));
 const std::exception_ptr retired = std::make_exception_ptr(std::runtime_error("compile work retired before completion"));
 mmltk::common::concurrency::CancellationObservation cancellation;
 Observer observer;
 mmltk::common::io::ScopedFd event;
 std::jthread notifier;
 std::unique_ptr<mmltk::common::concurrency::WorkerPool> pool;
 void publish_locked(CompileChange change) noexcept {
  if (includes(change, CompileChange::Interruption)) withdrawal_pending = true;
  bool wake = bool(observer) || shutdown || stopping || includes(change, CompileChange::Interruption);
  for (const auto& state : observers) {
   const auto delivered = state->interests & change;
   if (delivered == CompileChange::None) continue;
   state->notified = true;
   wake = true;
   state->pending.causes |= delivered;
   const auto retry_causes = delivered & (CompileChange::Resources | CompileChange::Pressure);
   if (retry_causes != CompileChange::None) {
    auto* local = Readiness::Attempt::current_;
    while (local && (!local->owner_ || local->owner_->state_ != state)) local = local->previous_;
    if (local) local->causes_ |= retry_causes;
    else state->pending.retry = true;
   }
  }
  if (wake && event.get() >= 0) signal_event(event.get());
 }
 void notify_locked(CompileAdmission::Change change = CompileAdmission::Change::Cpu) noexcept { admission->notify(change); }
 void park(Job& job, CompileParkReason reason) noexcept {
  const auto resource = [](CompileParkReason value) { return value == CompileParkReason::Bytes || value == CompileParkReason::Descriptors; };
  admission->change_job_pressure(resource(job.reason), resource(reason));
  job.reason = reason;
  if (reason != CompileParkReason::Bytes) remove_demand(job.demand);
 }
 [[nodiscard]] bool live(std::size_t kind) const { return classes[kind].producers || classes[kind].pending || classes[kind].running; }
 void reconcile() {
  std::size_t count = 0;
  for (std::size_t i = 0; i < class_count; ++i) count += live(i);
  // External helpers can only occupy elasticity, including a floor that has
  // become live since a nonpreemptible library grant was issued.
  admission->protected_cpus = std::min(count, lanes.size());
  floors = count <= lanes.size();
  std::array<bool, class_count> assigned{};
  for (auto& lane : lanes) {
   if (!lane.occupied() && (count > lanes.size() || (lane.baseline != CompileWorkClass::Count && !live(index(lane.baseline))))) lane.baseline = CompileWorkClass::Count;
   if (lane.baseline != CompileWorkClass::Count) assigned[index(lane.baseline)] = true;
  }
  if (count > lanes.size()) return;
  for (std::size_t kind = 0; kind < class_count; ++kind) {
   if (!live(kind) || assigned[kind]) continue;
   auto available = std::ranges::find_if(lanes, [](const auto& lane) { return !lane.occupied() && lane.baseline == CompileWorkClass::Count; });
   if (available == lanes.end()) {
    // An elastic borrower can already supply its class's floor. Move that
    // assignment from an idle peer so a newly live class can start without
    // waiting for unrelated work; CPU and workspace custody stay in place.
    for (auto idle = lanes.begin(); idle != lanes.end(); ++idle) {
     if (idle->occupied() || idle->baseline == CompileWorkClass::Count) continue;
     const auto occupied = std::ranges::find_if(lanes, [&](const auto& lane) {
      const auto grant_class = lane.active ? lane.active->grant_class : lane.service != CompileWorkClass::Count ? lane.service : lane.completing;
      return lane.baseline == CompileWorkClass::Count && grant_class == idle->baseline;
     });
     if (occupied == lanes.end()) continue;
     occupied->baseline = std::exchange(idle->baseline, CompileWorkClass::Count);
     available = idle;
     break;
    }
   }
   if (available == lanes.end()) break;
   available->baseline = static_cast<CompileWorkClass>(kind);
  }
 }
 void check() const {
  if (failure) std::rethrow_exception(failure);
  if (cancellation.requested()) std::rethrow_exception(cancellation_error);
  if (stopping || shutdown) throw std::runtime_error("compile admission after shutdown");
 }
 template <class Predicate>
 void wait(std::unique_lock<std::mutex>& lock, Predicate predicate, std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
  while (!predicate()) {
   // Only the legacy outer API with a nonnotifying observation is sampled.
   // Internal dependency/resource changes and notifying cancellation use events.
   if (cancellation.descriptor() < 0) {
    admission->changed.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(100)));
    if (cancellation.requested()) notify_locked(CompileAdmission::Change::Interruption);
   } else if (deadline == std::chrono::steady_clock::time_point::max())
    admission->changed.wait(lock);
   else
    admission->changed.wait_until(lock, deadline);
  }
 }
 CompileAllowance charge(CompileResources resources, const CompileAllowance& parent, bool finite) {
  auto credit = std::make_shared<Credits>(admission, resources, parent.credits_);
  admission->charge(*credit, finite);
  return CompileAllowance(std::move(credit));
 }
 // Admission holds the mutex throughout this inspection. Count only backing
 // whose strong owners are current frames or idle workspace custody. A parent,
 // queued input, copied allowance or external helper keeps its charge retained.
 // This is a conservative lower bound; no credit is returned by the estimate.
 template <class Visitor>
 void visit_workspace_credits(Visitor&& visit) const {
  for (const auto& lane : lanes) {
   for (auto* frame = lane.active; frame; frame = frame->parent) {
    visit(frame->credit);
    visit(frame->allowance.credits_);
   }
   visit(lane.idle.allowance.credits_);
  }
 }
 [[nodiscard]] std::uint64_t reclaimable_workspace_bytes(bool arenas_only = false) const {
  std::uint64_t result = 0;
  visit_workspace_credits([&](const auto& credit) {
   if (!credit || credit->resources.retained_handles || !credit->resources.bytes || (arenas_only && !credit->arena)) return;
   const std::shared_ptr<Credits>* first = nullptr;
   std::size_t references = 0;
   visit_workspace_credits([&](const auto& owned) {
    if (owned.get() != credit.get()) return;
    if (!first) first = &owned;
    ++references;
   });
   if (first == &credit && static_cast<std::size_t>(credit.use_count()) == references) {
    result += credit->resources.bytes;
    if (!arenas_only && credit->arena_padding && credit->arena_padding.use_count() == 1) result += credit->arena_padding->bytes;
   }
  });
  return result;
 }
 void refresh_aged() {
  // Strong custody can change without a resource event. Recompute its
  // conservative totals before reusing prospective resource eligibility.
  const auto reclaimable = aged_demands.head ? reclaimable_workspace_bytes() : 0;
  const auto reclaimable_arena = aged_demands.head ? reclaimable_workspace_bytes(true) : 0;
  const auto records = record_capacity();
  if (inspected_revision != demand_revision || inspected_resources != admission->resource_generation ||
      inspected_reclaimable != reclaimable || inspected_arena != reclaimable_arena || inspected_records != records) {
   aged_candidate = nullptr;
   for (auto* job = aged_demands.head; job; job = job->aged.next) {
    // A direct request must leave already-admitted work runnable when that
    // work owns the record needed for its successor. Record availability is
    // independent of the cached byte/descriptor predicate.
    if (!record_eligible(*job, records)) continue;
    if (admission->eligibility(job->resources, job->parent.credits_.get(), job->finite_consumer, reclaimable, reclaimable_arena).fits) {
     aged_candidate = job;
     break;
    }
   }
   inspected_revision = demand_revision;
   inspected_resources = admission->resource_generation;
   inspected_reclaimable = reclaimable;
   inspected_arena = reclaimable_arena;
   inspected_records = records;
  }
  auto* reservation = aged_candidate && selections - aged_candidate->age >= 8 ? aged_candidate : nullptr;
  if (reservation != reserved_demand) {
   reserved_demand = reservation;
   notify_locked(CompileChange::Fairness);
  }
 }
 [[nodiscard]] bool resource_eligible(Demand& demand) {
  if (demand.blocked_generation != admission->resource_generation) {
   const auto eligible = admission->eligibility(demand.resources, demand.parent.credits_.get(), demand.finite_consumer);
   demand.blocked_generation = admission->resource_generation;
   demand.resource_fits = eligible.fits;
   demand.denial = eligible.denial;
  }
  return demand.resource_fits;
 }
 [[nodiscard]] bool fairness_denied(const CompileResources& resources, const Demand* request = nullptr) const noexcept {
  // Current physical custody removes reservations that need retained owners to
  // finish, preserving their finite consumers without a parent-only bypass.
  return reserved_demand && reserved_demand != request && resources.bytes && !resources.retained_handles;
 }
 void validate(const Task& task) const {
  admission->require(task.resources, true);
  if (!task.work) throw std::invalid_argument("compile task has no work");
  (void)work_class(task.kind);
  if (task.workspace && &task.workspace->owner_ != owner) throw std::invalid_argument("compile workspace belongs to another session");
  for (const auto* value : {&task.allowance, &task.parent})
   if (value->credits_ && value->credits_->owner != admission) throw std::invalid_argument("compile allowance belongs to another session");
  if (task.allowance && !CompileAdmission::covers(*task.allowance.credits_, task.resources)) throw std::invalid_argument("compile task exceeds its transferred allowance");
 }
 CompileExecution* owner = nullptr;
 [[nodiscard]] static bool record_eligible(const Demand& demand, std::uint32_t records) noexcept {
  return !demand.needs_record || (demand.resources.workspace_class != CompileWorkClass::Count && (records & (std::uint32_t{1} << index(demand.resources.workspace_class))));
 }
 [[nodiscard]] std::uint32_t record_capacity() const noexcept {
  if (available_records.empty()) return 0;
  constexpr auto all = (std::uint32_t{1} << class_count) - 1;
  if (record_limit < class_count) return all;
  std::size_t protected_records = 0;
  std::uint32_t protected_classes = 0;
  for (std::size_t i = 0; i < class_count; ++i)
   if (!classes[i].pending) {
    ++protected_records;
    protected_classes |= std::uint32_t{1} << i;
   }
  return jobs.size() < record_limit - protected_records ? all : protected_classes;
 }
 [[nodiscard]] bool has_record(CompileTaskKind kind) const noexcept { return record_capacity() & (std::uint32_t{1} << index(work_class(kind))); }
 [[nodiscard]] std::shared_ptr<Job> claim_record(std::shared_ptr<CompletionState> completion) {
  auto job = available_records.back();
  jobs.push_back(job); // The only fallible mutation precedes published custody.
  available_records.pop_back();
  *job = Job{};
  completion->job = job;
  job->completion = std::move(completion);
  return job;
 }
 void enqueue(const std::shared_ptr<Job>& job) {
  // Complete the only fallible queue mutation before publishing demand custody.
  ready[static_cast<std::size_t>(job->task.kind)].jobs.push_back(job);
  if (observer) job->ready_at = std::chrono::steady_clock::now();
  park(*job, CompileParkReason::None);
  job->demand = Demand{.resources = job->task.resources, .parent = job->task.parent, .finite_consumer = job->task.finite_consumer};
  if (!job->task.allowance && admission->oversized(job->task.resources)) exceptional.append(*job);
 }
 void recycle(const std::shared_ptr<Job>& job) noexcept {
  unindex(*job);
  --classes[index(work_class(job->task.kind))].pending;
  jobs.remove(job);
  if (draining_request == job.get()) drain(nullptr, nullptr, 0);
  job->done = true;
  available_records.push_back(job);
  if (observer) ++completed;
  reconcile();
  notify_locked(CompileChange::Records | CompileChange::Cpu);
 }
 void complete(const std::shared_ptr<Job>& job, std::exception_ptr error) noexcept {
  const auto kind = job->task.kind;
  // Input captures and byte/descriptor custody retire before completion becomes
  // visible; physical owners remain alive across cancellation and failed joins.
  Task retired_task;
  CompileAllowance retired_demand_parent;
  {
   const std::lock_guard lock(admission->mutex);
   retired_task = std::move(job->task);
   retired_demand_parent = std::move(job->demand.parent);
   job->task.workspace = nullptr;
  }
  retired_task = {};
  retired_demand_parent = {};
  std::vector<std::function<void(std::exception_ptr)>> callbacks;
  {
   const std::lock_guard lock(job->completion->mutex);
   job->completion->error = error;
   job->completion->done = true;
   callbacks.swap(job->completion->callbacks);
  }
  job->completion->changed.notify_all();
  CompileObservationClock callbacks_clock{bool(observer)};
  for (auto& callback : callbacks) {
   try {
    callback(error);
   } catch (...) {
    const std::lock_guard lock(admission->mutex);
    if (!failure) failure = std::current_exception();
    notify_locked(CompileAdmission::Change::Interruption);
   }
  }
  callbacks.clear();
  {
   const std::lock_guard lock(admission->mutex);
   job->task.kind = kind;
   if (observer) costs.completion_callback_ns += callbacks_clock.elapsed();
   if (job->retained) {
    // The physical owner may release custody inside a completion callback.
    // Only park after all callbacks return, so a successor cannot race them.
    --classes[index(work_class(kind))].pending;
    job->task = {};
    job->task.kind = job->successor;
    ++classes[index(work_class(job->task.kind))].pending;
    job->running = false;
    park(*job, CompileParkReason::Continuation);
    if (draining_request == job.get()) drain(nullptr, nullptr, 0);
    reconcile();
    notify_locked(CompileChange::Records | CompileChange::Cpu);
   } else
    recycle(job);
  }
 }
 void release_idle(Idle idle, std::size_t lane) noexcept {
  try {
   idle.workspace->retire_(lane);
  } catch (...) {
   const std::lock_guard lock(admission->mutex);
   if (!idle.workspace->failure_) idle.workspace->failure_ = std::current_exception();
  }
  idle.allowance = {};
  const std::lock_guard lock(admission->mutex);
  lanes[lane].retiring = nullptr;
  notify_locked();
 }
 static CompileAdmissionObservation observe_demand(CompileTaskKind kind, const CompileResources& demand, CompileResourceDenial denial, std::uint64_t epoch, std::uint64_t extent) {
  return {kind, denial, demand, epoch, extent};
 }
 void observe_rejection(std::unique_lock<std::mutex>& lock, CompileTaskKind kind, const CompileResources& demand, CompileResourceDenial denial) noexcept {
  if (!observer) return;
  if (denial == CompileResourceDenial::Records) ++costs.record_denials;
  CompileExecutionSnapshot observation;
  try {
   observation = snapshot_locked();
   observation.demands.push_back(observe_demand(kind, demand, denial, admission->resource_generation,
    admission->arenas->largest_available(demand.completion_workspace, demand.workspace_class, demand.workspace_window)));
  } catch (...) { observation.costs.incomplete = true; }
  lock.unlock();
  try { observer(denial == CompileResourceDenial::Records ? "compile.record.rejected" : "compile.continuation.rejected", observation); } catch (...) {}
 }
 [[nodiscard]] CompileExecutionSnapshot snapshot_locked() const {
  CompileExecutionSnapshot result;
  for (std::size_t i = 0; i < class_count; ++i) {
   result.live[i] = live(i);
   result.running[i] = classes[i].running;
   result.parked[static_cast<std::size_t>(CompileParkReason::Cpu)] += service_waiters[i];
  }
  for (const auto& lane : lanes) {
   if (lane.baseline == CompileWorkClass::Count)
    ++result.elastic;
   else
    ++result.baseline[index(lane.baseline)];
  }
  for (const auto& job : jobs) {
   if (job->running || job->done) continue;
   if (job->reason == CompileParkReason::None)
    ++result.ready[index(work_class(job->task.kind))];
   else
    ++result.parked[static_cast<std::size_t>(job->reason)];
  }
  result.parked[static_cast<std::size_t>(CompileParkReason::Withdrawal)] += admission->withdrawals;
  result.active = admission->active;
  result.external = admission->external_cpus;
  result.services = services;
  result.records = jobs.size();
  result.record_limit = record_limit;
  result.resources = {admission->bytes + admission->handle_bytes, admission->descriptors, false, admission->active + admission->external_cpus, false, admission->committed};
  result.opaque_library = admission->opaque_bytes;
  result.byte_target = admission->target;
  result.descriptor_limit = admission->descriptor_capacity;
  if (admission->arenas) {
   const auto bank = admission->arenas->snapshot();
   result.normal_locked = bank.locked;
   result.resident = bank.resident;
   result.leased = bank.leased;
   result.exceptional = bank.exceptional;
   result.exceptional_peak = bank.exceptional_peak;
   result.exceptional_reserved = bank.exceptional_reserved;
   result.workspace_reuses = bank.reused;
   result.physical_retirements = bank.physical_retirements;
   result.arena = bank.observation;
   const auto& plan = admission->arenas->plan();
   result.protected_workspace = plan.protected_bytes;
   result.shared_workspace = plan.shared_bytes;
   result.compaction_workspace = plan.compaction_bytes;
  }
  result.admitted = admitted;
  result.completed = completed;
  result.task_completed = task_completed;
  result.task_duration_ns = task_duration_ns;
  result.task_max_duration_ns = task_max_duration_ns;
  if (observer) {
   result.costs = costs;
   result.costs.eligibility_probes = admission->eligibility_probes;
   result.costs.resource_epoch = admission->resource_generation;
   result.costs.admission_lock_ns = admission->admission_lock_ns;
   result.task_body_ns = task_body_ns;
   result.task_cpu_ns = task_cpu_ns;
   result.ready_to_start_ns = ready_to_start_ns;
   try {
    for (const auto& job : jobs) {
     if (job->done || job->running || job->denial == CompileResourceDenial::None) continue;
     const auto& demand = job->task.resources;
     result.demands.push_back(observe_demand(job->task.kind, demand, job->denial, job->observed_epoch, job->observed_extent));
    }
   } catch (...) {
    result.demands.clear();
    result.costs.incomplete = true;
   }
  }
  return result;
 }
 bool consume(std::size_t lane_index, bool blocking);
 void events() noexcept;
};
thread_local CompileExecution::Impl::Frame* CompileExecution::Impl::Frame::current = nullptr;
bool CompileExecution::Impl::Lane::owns(Workspace* workspace) const {
 if (!workspace) return false;
 for (auto* frame = active; frame; frame = frame->parent)
  if (frame->workspace == workspace) return true;
 return false;
}
bool CompileExecution::Impl::consume(std::size_t lane_index, bool blocking) {
 CompileObservationClock lock_clock{bool(observer)};
 std::shared_ptr<Job> selected;
 Idle retired_idle;
 std::size_t retirement_lane = lane_index;
 bool finished = false;
 std::exception_ptr error;
 CompileAllowance allowance;
 {
  std::unique_lock lock(admission->mutex);
  if (observer) admission->admission_lock_ns += lock_clock.elapsed();
  const auto select = [&] {
   if (shutdown) {
    finished = true;
    return true;
   }
   auto& lane = lanes[lane_index];
   if (lane.retiring || lane.service != CompileWorkClass::Count || lane.external || (blocking && lane.active)) return false;
   const bool discard = stopping || failure || cancellation.requested();
   refresh_external_drain();
   if (!draining_external) {
    while (exceptional.head && exceptional.head->cancelled) exceptional.remove(*exceptional.head);
    const auto* drain = !discard && !admission->arenas->exceptional_active() ? exceptional.head : nullptr;
    this->drain(drain, drain ? drain->task.parent.credits_.get() : nullptr, drain ? drain->task.resources.bytes : 0);
   }
   const auto pressure = admission->pressure();
   if (pressure)
    for (std::size_t i = 0; i < lanes.size(); ++i) {
     auto& value = lanes[i];
     if (!value.retiring && value.idle.workspace && !value.owns(value.idle.workspace)) {
      retired_idle = std::exchange(value.idle, {});
      value.retiring = retired_idle.workspace;
      retirement_lane = i;
      return true;
     }
    }
   refresh_aged();
   for (std::size_t turn = 0; turn < class_count; ++turn) {
    const auto class_index = (cursor + turn) % class_count;
    if (!discard && floors && lane.baseline != CompileWorkClass::Count && index(lane.baseline) != class_index) continue;
    if (!discard && service_waiters[class_index] && !lane.active) {
     // Nonblocking services wait through Readiness, while blocking pumps wait
     // directly on admission. Publish only a real cursor handoff: notifying on
     // an unchanged denied probe would wake this predicate indefinitely.
     if (cursor != class_index) {
      cursor = class_index;
      notify_locked();
     }
     return false;
    }
    auto& kind_cursor = classes[class_index].kind_cursor;
    for (std::size_t step = 0; step < kind_count; ++step) {
     const auto kind = (kind_cursor + step) % kind_count;
     if (index(work_class(static_cast<CompileTaskKind>(kind))) != class_index) continue;
     auto& candidates = ready[kind];
     auto& queue = candidates.jobs;
     auto next = queue.begin();
     for (std::size_t examined = 0; examined < queue.size(); ++examined) {
      const auto position = next;
      // Reconsider the oldest request first, then resume beyond the last
      // successful bypass. The queue owns its cursor through cancellation.
      if (!examined && candidates.fallback != queue.end() && candidates.fallback != queue.begin())
       next = candidates.fallback;
      else {
       ++next;
       if (next == queue.end()) next = std::next(queue.begin());
      }
      auto& job = **position;
      const auto& task = job.task;
      const bool reusable = task.retain_workspace && task.workspace && lane.idle.workspace == task.workspace && CompileAdmission::covers(*lane.idle.allowance.credits_, task.resources);
      if (!job.cancelled && !discard) {
       if (admission->arenas->exceptional_active() && !CompileAdmission::exceptional_custody(task.allowance.credits_.get()) && !CompileAdmission::exceptional_custody(task.parent.credits_.get())) {
        if (observer) { job.denial = CompileResourceDenial::ExceptionalLineage; job.observed_epoch = admission->resource_generation; job.observed_extent = 0; }
        park(job, CompileParkReason::Bytes);
        continue;
       }
       if (admission->active + admission->external_cpus >= cpus.size() + (lane.active ? 1 : 0)) {
        // A temporary service grant does not resolve a resource predicate that
        // this record already checked. Retain its pressure until rechecked;
        // otherwise CPU acquire/release would manufacture pressure wake loops.
        if (job.reason != CompileParkReason::Bytes && job.reason != CompileParkReason::Descriptors) {
         if (observer) { job.denial = CompileResourceDenial::Cpu; job.observed_epoch = admission->resource_generation; job.observed_extent = 0; }
         park(job, CompileParkReason::Cpu);
        }
        continue;
       }
       if (lane.owns(task.workspace)) continue;
       // Transferred custody was excluded from reclaimable bytes by its queued
       // owner. The pressure pass above already retires unowned idle backing
       // before calculating a reservation, so reuse cannot renew counted bytes.
       if (!task.allowance && !reusable) {
        const auto* parent = task.parent.credits_.get();
        const auto eligible = resource_eligible(job.demand);
        job.denial = job.demand.denial;
        if (observer) {
         job.observed_epoch = admission->resource_generation;
         job.observed_extent = admission->arenas->largest_available(task.resources.completion_workspace, task.resources.workspace_class, task.resources.workspace_window);
        }
        const bool unfair = eligible && fairness_denied(task.resources, &job.demand);
        if (unfair) job.denial = CompileResourceDenial::AgedDemand;
        if (!eligible || unfair) {
         park(
          job, task.resources.descriptors + task.resources.continuation_descriptors > admission->descriptor_room(task.resources, parent) ? CompileParkReason::Descriptors : CompileParkReason::Bytes);
         if (job.reason == CompileParkReason::Bytes) wait_demand(job.demand);
         continue;
        }
       }
      }
      selected = *position;
      ++selections;
      if (position != queue.begin()) candidates.fallback = std::next(position);
      candidates.erase(position);
      unindex(job);
      job.running = true;
      if (observer) {
       job.denial = CompileResourceDenial::None;
       if (job.ready_at != std::chrono::steady_clock::time_point{})
        ready_to_start_ns[static_cast<std::size_t>(task.kind)] += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - job.ready_at).count());
      }
      lane.dispatching = true;
      if (!lane.active) ++admission->active;
      park(job, CompileParkReason::None);
      ++classes[class_index].running;
      if (discard || job.cancelled) error = failure ? failure : cancellation.requested() ? cancellation_error : retired;
      if (!error) {
       if (task.workspace && lane.idle.workspace && !lane.owns(lane.idle.workspace)) {
        if (reusable && !task.allowance) {
         job.task.allowance = std::move(lane.idle.allowance);
         lane.idle = {};
        } else {
         retired_idle = std::exchange(lane.idle, {});
         lane.retiring = retired_idle.workspace;
        }
       }
       try {
        allowance = task.allowance ? std::move(job.task.allowance) : charge(task.resources, task.parent, task.finite_consumer);
       } catch (...) { error = std::current_exception(); }
      }
      cursor = (class_index + 1) % class_count;
      kind_cursor = (kind + 1) % kind_count;
      return true;
     }
    }
   }
   return false;
  };
  if (blocking)
   admission->changed.wait(lock, select);
  else if (!select())
   return false;
 }
 if (retired_idle.workspace) release_idle(std::move(retired_idle), retirement_lane);
 if (!selected) return !finished;
 const auto class_index = index(work_class(selected->task.kind));
 const auto started = observer ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
 std::uint64_t body_ns = 0;
 std::uint64_t cpu_ns = 0;
 bool cpu_incomplete = false;
 CompileWorkClass previous_completion;
 {
  Frame frame(*this, lane_index, static_cast<CompileWorkClass>(class_index));
  frame.job = selected.get();
  {
   const std::lock_guard lock(admission->mutex);
   frame.enter(std::move(allowance), selected->task.workspace);
  }
  {
   const std::lock_guard lock(admission->mutex);
   if (!error && selected->task.workspace) error = selected->task.workspace->failure_;
  }
  CompileObservationClock body_clock{bool(observer)};
  CompileObservationClock cpu_clock{bool(observer), true};
  if (!error) try {
    selected->task.work(lane_index, frame.allowance);
   } catch (...) { error = std::current_exception(); }
  if (observer) body_ns = body_clock.elapsed();
  if (observer) { cpu_ns = cpu_clock.elapsed(); cpu_incomplete = cpu_clock.incomplete(); }
  {
   const std::lock_guard lock(admission->mutex);
   if (!error && selected->cancelled) error = retired;
  }
  if (selected->task.finite_consumer && frame.allowance) {
   std::shared_ptr<Credits> parent;
   {
    const std::lock_guard lock(admission->mutex);
    if (!frame.allowance.credits_->borrowed) {
     parent = std::move(frame.allowance.credits_->parent);
     if (parent) notify_locked(CompileAdmission::Change::Resources);
    }
   }
  }
  if (selected->task.workspace && selected->task.retain_workspace && frame.allowance) {
   Idle displaced;
   {
    const std::lock_guard lock(admission->mutex);
    displaced = std::exchange(lanes[lane_index].idle, {selected->task.workspace, std::move(frame.allowance)});
    if (displaced.workspace) lanes[lane_index].retiring = displaced.workspace;
   }
   if (displaced.workspace) release_idle(std::move(displaced), lane_index);
  }
  {
   const std::lock_guard lock(admission->mutex);
   // Return the CPU grant and scratch before callbacks, while retaining the
   // physical worker's class until it can dispatch again. A newly live floor
   // must not be assigned to this still-settling call. Services may continue
   // to borrow its returned CPU independently.
   previous_completion = std::exchange(lanes[lane_index].completing, frame.grant_class);
  }
 }
 {
  const std::lock_guard lock(admission->mutex);
  --classes[class_index].running;
  if (observer) {
   const auto kind = static_cast<std::size_t>(selected->task.kind);
   const auto duration = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
   ++task_completed[kind];
   task_duration_ns[kind] += duration;
   task_max_duration_ns[kind] = std::max(task_max_duration_ns[kind], duration);
   task_body_ns[kind] += body_ns;
   task_cpu_ns[kind] += cpu_ns;
   costs.incomplete |= cpu_incomplete;
  }
 }
 complete(selected, error);
 {
  const std::lock_guard lock(admission->mutex);
  lanes[lane_index].completing = previous_completion;
  reconcile();
  notify_locked();
 }
 return true;
}
void CompileExecution::Impl::events() noexcept {
 bool cancellation_observed = false;
 std::vector<std::pair<std::shared_ptr<Readiness::State>, Readiness::Changes>> callbacks;
 std::vector<std::shared_ptr<Job>> withdrawn;
 withdrawn.reserve(record_limit);
 for (;;) {
  std::array<pollfd, 2> descriptors{{{event.get(), POLLIN, 0}, {cancellation_observed ? -1 : cancellation.descriptor(), POLLIN, 0}}};
  int timeout = -1;
  if (cancellation.descriptor() < 0) {
   const std::lock_guard lock(admission->mutex);
   // Preserve the legacy blocking service wait's cancellation sampling for
   // inputs without a notifying descriptor. Only armed service demand needs
   // this compatibility wake; normal readiness and cancellation stay event driven.
   if (std::ranges::any_of(service_waiters, [](std::size_t count) { return count != 0; })) timeout = 100;
  }
  int result;
  do { result = ::poll(descriptors.data(), descriptors.size(), timeout); } while (result < 0 && errno == EINTR);
  if (result < 0) {
   const std::lock_guard lock(admission->mutex);
   failure = std::make_exception_ptr(std::system_error(errno, std::generic_category(), "compile event wait"));
   admission->changed.notify_all();
   return;
  }
  if (!result && !cancellation.requested()) continue;
  if (descriptors[0].revents) {
   std::uint64_t value;
   while (::read(event.get(), &value, sizeof(value)) < 0 && errno == EINTR) {}
  }
  CompileExecutionSnapshot observation;
  std::exception_ptr withdrawal_error;
  bool cancelled;
  {
   const std::lock_guard lock(admission->mutex);
   if (shutdown) return;
   cancelled = cancellation.requested();
   if (cancelled && (!cancellation_observed || !result)) publish_locked(CompileAdmission::Change::Interruption);
   withdrawal_error = failure ? failure : cancelled ? cancellation_error : retired;
   if (std::exchange(withdrawal_pending, false)) for (const auto& job : jobs) {
    if (job->running || job->done || job->reason == CompileParkReason::Continuation || !(job->cancelled || stopping || failure || cancelled)) continue;
    // Unstarted custody has no physical CPU user to join. Claim its settlement
    // here so unrelated held compute cannot delay source-local withdrawal.
    job->running = true;
    unindex(*job);
    job->dependency.reset();
    park(*job, CompileParkReason::None);
    withdrawn.push_back(job);
   }
   if (!withdrawn.empty()) {
    for (auto& queue : ready)
     for (auto position = queue.jobs.begin(); position != queue.jobs.end();)
      if ((*position)->running)
       position = queue.erase(position);
      else
       ++position;
   }
   admission->changed.notify_all();
   for (const auto& callback : observers)
    if (callback->notified) {
     // Acknowledge exactly this batch. Mutations racing delivery accumulate in
     // a fresh pending batch and retain their own eventfd wake.
     callbacks.emplace_back(callback, std::exchange(callback->pending, {}));
     callback->notified = false;
    }
   if (observer) observation = snapshot_locked();
  }
  for (const auto& job : withdrawn) complete(job, withdrawal_error);
  withdrawn.clear();
  for (const auto& [value, changes] : callbacks) {
   {
    const std::lock_guard lock(value->mutex);
    if (value->removed) continue;
    value->active = true;
   }
   CompileObservationClock callback_clock{bool(observer)};
   try {
    value->callback(changes);
   } catch (...) {
    const std::lock_guard lock(admission->mutex);
    if (!failure) {
     failure = std::current_exception();
     notify_locked(CompileAdmission::Change::Interruption);
    }
   }
   if (observer) {
    const std::lock_guard lock(admission->mutex);
    ++costs.notifier_deliveries;
    costs.callback_ns += callback_clock.elapsed();
   }
   {
    const std::lock_guard lock(value->mutex);
    value->active = false;
    value->changed.notify_all();
   }
  }
  callbacks.clear();
  if (observer) try {
    observer("compile.execution", observation);
   } catch (...) {}
  // The token remains readable. Keep completion/readiness dispatch armed on
  // our event descriptor while physical users drain, without polling the token.
  cancellation_observed = cancellation_observed || cancelled;
 }
}
CompileExecution::CompileExecution(std::size_t workers, std::span<const int> eligible, CompileExecutionLimits limits, mmltk::common::concurrency::CancellationObservation cancellation,
 Observer observer, std::exception_ptr cancellation_error)
    : impl_(std::make_shared<Impl>()) {
 auto& state = *impl_;
 workers = std::max<std::size_t>(1, workers);
 // WorkerPool resolves physical-core-first placement once. Its installed
 // policies are the authority shared by compute lanes, services and helpers.
 state.pool = std::make_unique<mmltk::common::concurrency::WorkerPool>(workers, std::vector<int>(eligible.begin(), eligible.end()), "compile", workers);
 if (state.pool->size() != workers) throw std::invalid_argument("compile worker budget exceeds distinct assigned CPUs");
 state.cpus.reserve(workers);
 for (std::size_t lane = 0; lane < workers; ++lane) {
  const auto& affinity = state.pool->policy(lane).affinity;
  if (affinity.size() != 1) throw std::logic_error("compile worker requires one resolved CPU");
  state.cpus.push_back(affinity.front());
 }
 state.lanes.resize(workers);
 state.record_limit = limits.task_records ? limits.task_records : workers * 8 + class_count;
 state.available_records.reserve(state.record_limit);
 for (std::size_t record = 0; record < state.record_limit; ++record) {
  auto job = std::make_shared<Job>();
  job->done = true;
  state.available_records.push_back(std::move(job));
 }
 if (state.record_limit < workers) throw std::invalid_argument("compile record capacity is smaller than its CPU grant");
 state.admission->cpu_capacity = workers;
 const auto workspace_plan = limits.workspace_plan ? *limits.workspace_plan : CompileWorkspacePlan::prepare(workers, {}, 0, 0, limits.transient_bytes);
 state.admission->target = limits.transient_bytes ? limits.transient_bytes : workspace_plan.budget;
 std::vector<int> nodes;
 nodes.reserve(workers);
 for (std::size_t lane = 0; lane < workers; ++lane) nodes.push_back(state.pool->policy(lane).numa_node);
 state.admission->arenas = std::make_shared<CompileArenaBank>(workspace_plan, nodes, [weak = std::weak_ptr<CompileAdmission>(state.admission)] {
  if (auto admission = weak.lock()) {
   const std::lock_guard lock(admission->mutex);
   admission->notify(CompileAdmission::Change::Resources);
  }
 }, bool(observer));
 const auto descriptors = descriptor_headroom();
 state.admission->descriptor_capacity = limits.descriptors ? std::min(limits.descriptors, descriptors) : descriptors;
 if (state.admission->descriptor_capacity < 2) throw std::invalid_argument("compile descriptor admission requires completion headroom");
 state.owner = this;
 state.cancellation = cancellation;
 if (cancellation_error) state.cancellation_error = std::move(cancellation_error);
 state.observer = std::move(observer);
 state.admission->tracing = bool(state.observer);
 state.event = mmltk::common::io::ScopedFd(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
 if (state.event.get() < 0) throw std::system_error(errno, std::generic_category(), "cannot create compile event descriptor");
 state.admission->publish_change = [weak = std::weak_ptr<Impl>(impl_)](CompileAdmission::Change change) {
  if (auto owner = weak.lock()) owner->publish_locked(change);
 };
 state.admission->release_cpus = [weak = std::weak_ptr<Impl>(impl_)](std::span<const std::size_t> lanes) {
  if (auto owner = weak.lock()) {
   for (auto lane : lanes) owner->lanes[lane].external = false;
   owner->reconcile();
  }
 };
 try {
  state.notifier = std::jthread([owner = impl_] { owner->events(); });
  for (std::size_t worker = 0; worker < workers; ++worker)
   state.pool->enqueue_detached([owner = impl_] {
    const auto lane = owner->pool->current_worker_index();
    if (owner->observer) {
     const std::lock_guard lock(owner->admission->mutex);
     ++owner->costs.workers_created;
    }
    while (owner->consume(lane, true)) {}
    if (owner->observer) {
     const std::lock_guard lock(owner->admission->mutex);
     ++owner->costs.workers_retired;
    }
   });
 } catch (...) {
  {
   const std::lock_guard lock(state.admission->mutex);
   state.shutdown = true;
   state.notify_locked();
  }
  state.pool.reset();
  if (state.notifier.joinable()) state.notifier.join();
  throw;
 }
}
CompileExecution::~CompileExecution() {
 retire_attempt();
 {
  const std::lock_guard lock(impl_->admission->mutex);
  impl_->shutdown = true;
  impl_->notify_locked();
 }
 impl_->pool.reset();
 impl_->notifier.join();
 if (impl_->observer) try { impl_->observer("compile.execution.retired", snapshot()); } catch (...) {}
}
CompileExecution::Producer::Producer(std::shared_ptr<Impl> owner, CompileWorkClass kind) : owner_(std::move(owner)), class_(kind) {}
CompileExecution::Producer::~Producer() { close(); }
CompileExecution::Producer::Producer(Producer&& other) noexcept : owner_(std::move(other.owner_)), class_(other.class_) {}
CompileExecution::Producer& CompileExecution::Producer::operator=(Producer&& other) noexcept {
 if (this != &other) {
  close();
  owner_ = std::move(other.owner_);
  class_ = other.class_;
 }
 return *this;
}
void CompileExecution::Producer::close() noexcept {
 if (!owner_) return;
 auto owner = std::move(owner_);
 const std::lock_guard lock(owner->admission->mutex);
 --owner->classes[index(class_)].producers;
 owner->reconcile();
 owner->notify_locked();
}
CompileExecution::Producer CompileExecution::open_class(CompileWorkClass kind) {
 if (index(kind) >= class_count) throw std::invalid_argument("invalid compile class");
 const std::lock_guard lock(impl_->admission->mutex);
 impl_->check();
 ++impl_->classes[index(kind)].producers;
 impl_->reconcile();
 impl_->notify_locked();
 return Producer(impl_, kind);
}
bool CompileExecution::Completion::ready() const {
 if (!state_) return true;
 const std::lock_guard lock(state_->mutex);
 return state_->done;
}
std::exception_ptr CompileExecution::Completion::failure() const {
 if (!state_) return {};
 const std::lock_guard lock(state_->mutex);
 return state_->error;
}
void CompileExecution::Completion::wait() const {
 if (!state_) return;
 auto owner = state_->owner.lock();
 if (owner && Impl::Frame::current && &Impl::Frame::current->owner == owner.get()) throw std::logic_error("compile CPU lane cannot join a completion");
 std::unique_lock lock(state_->mutex);
 while (!state_->done) {
  if (owner && owner->cancellation.descriptor() < 0) {
   state_->changed.wait_for(lock, std::chrono::milliseconds(100));
   lock.unlock();
   if (owner->cancellation.requested()) {
    const std::lock_guard admission_lock(owner->admission->mutex);
    owner->notify_locked(CompileAdmission::Change::Interruption);
   }
   lock.lock();
  } else
   state_->changed.wait(lock);
 }
 if (state_->error) std::rethrow_exception(state_->error);
}
void CompileExecution::Completion::subscribe(std::function<void(std::exception_ptr)> callback) const {
 if (!callback) throw std::invalid_argument("empty compile completion subscription");
 if (!state_) {
  callback({});
  return;
 }
 std::exception_ptr error;
 {
  const std::lock_guard lock(state_->mutex);
  if (!state_->done) {
   state_->callbacks.push_back(std::move(callback));
   return;
  }
  error = state_->error;
 }
 callback(error);
}
void CompileExecution::Completion::cancel() const noexcept {
 if (!state_) return;
 if (auto owner = state_->owner.lock()) {
  const std::lock_guard lock(owner->admission->mutex);
  const std::lock_guard completion_lock(state_->mutex);
  if (state_->done) return;
  if (auto job = state_->job.lock(); job && !job->done && job->completion == state_) {
   job->cancelled = true;
   owner->unindex(*job);
   owner->refresh_aged();
   if (job->reason == CompileParkReason::Dependency) {
    job->dependency.reset();
    owner->enqueue(job);
   }
   owner->notify_locked(CompileChange::Interruption);
  }
 }
}
CompileExecution::CompletionSource::CompletionSource(CompileExecution* owner) : state_(std::make_shared<CompletionState>()) {
 if (owner) state_->owner = owner->impl_;
}
void CompileExecution::CompletionSource::settle(std::exception_ptr error) const noexcept {
 std::vector<std::function<void(std::exception_ptr)>> callbacks;
 {
  const std::lock_guard lock(state_->mutex);
  if (state_->done) return;
  state_->done = true;
  state_->error = error;
  callbacks.swap(state_->callbacks);
 }
 state_->changed.notify_all();
 for (auto& callback : callbacks) try {
   callback(error);
  } catch (...) {
   if (auto owner = state_->owner.lock()) {
    const std::lock_guard lock(owner->admission->mutex);
    if (!owner->failure) owner->failure = std::current_exception();
    owner->notify_locked(CompileAdmission::Change::Interruption);
   }
  }
}
CompileExecution::Dependency::Dependency(CompileExecution& owner) : state_(std::make_shared<DependencyState>()) { state_->owner = owner.impl_; }
std::uint64_t CompileExecution::Dependency::generation() const {
 auto owner = state_->owner.lock();
 if (!owner) return state_->generation;
 const std::lock_guard lock(owner->admission->mutex);
 return state_->generation;
}
void CompileExecution::Dependency::notify() noexcept {
 if (auto owner = state_->owner.lock()) {
  const std::lock_guard lock(owner->admission->mutex);
  ++state_->generation;
  for (auto& value : state_->waiting)
   if (auto job = value.lock(); job && !job->done && job->reason == CompileParkReason::Dependency && job->dependency == state_) {
    job->dependency.reset();
    owner->enqueue(job);
   }
  state_->waiting.clear();
  owner->notify_locked();
 }
}
CompileExecution::Continuation::Continuation(std::shared_ptr<Impl> owner, std::shared_ptr<CompletionState> completion) : owner_(std::move(owner)), completion_(std::move(completion)) {}
CompileExecution::Continuation::~Continuation() { release(); }
CompileExecution::Continuation::Continuation(Continuation&& other) noexcept : owner_(std::move(other.owner_)), completion_(std::move(other.completion_)), allowance_(std::move(other.allowance_)) {}
CompileExecution::Continuation& CompileExecution::Continuation::operator=(Continuation&& other) noexcept {
 if (this != &other) {
  release();
  owner_ = std::move(other.owner_);
  completion_ = std::move(other.completion_);
  allowance_ = std::move(other.allowance_);
 }
 return *this;
}
void CompileExecution::Continuation::release() noexcept {
 allowance_ = {};
 if (!owner_) return;
 auto owner = std::move(owner_);
 auto completion = std::move(completion_);
 const std::lock_guard lock(owner->admission->mutex);
 if (auto job = completion->job.lock(); job && !job->done && job->completion == completion && job->retained) {
  job->retained = false;
  // An executing task or its callbacks still own their normal settlement.
  if (job->reason == CompileParkReason::Continuation) owner->recycle(job);
 }
}
std::optional<CompileExecution::Continuation> CompileExecution::try_retain(
 CompileTaskKind successor, CompileResources resources, const CompileAllowance& parent, bool finite_consumer, CompileResourceDenial* denial) {
 return retain(successor, resources, parent, finite_consumer, denial, nullptr);
}
std::optional<CompileExecution::Continuation> CompileExecution::try_retain(CompileResourceWait& waiting, CompileResourceDenial* denial) {
 const auto& state = *waiting.state_;
 if (state.execution != impl_) throw std::invalid_argument("compile continuation demand belongs to another session");
 if (!state.demand.needs_record) throw std::invalid_argument("compile pressure registration has no successor");
 const auto& demand = state.demand;
 return retain(state.successor, demand.resources, demand.parent, demand.finite_consumer, denial, &waiting);
}
std::optional<CompileExecution::Continuation> CompileExecution::retain(
 CompileTaskKind successor, CompileResources resources, const CompileAllowance& parent, bool finite_consumer, CompileResourceDenial* denial, CompileResourceWait* deferred) {
 resources.workspace_class = work_class(successor);
 CompileAllowance allowance;
 std::unique_lock lock(impl_->admission->mutex);
 impl_->check();
 if (deferred && !deferred->state_->registered) throw std::invalid_argument("compile continuation demand already admitted");
 auto* waiting = deferred ? &deferred->state_->demand : nullptr;
 // A direct waiter cannot drain the very task that must return its successor
 // record. Refresh only selection; its class and resource cache stay unchanged.
 impl_->refresh_external_drain();
 impl_->admission->require(resources, true);
 if (parent.credits_ && parent.credits_->owner != impl_->admission) throw std::invalid_argument("compile continuation parent belongs to another session");
 if (impl_->observer) ++impl_->costs.submission_attempts;
 const auto reject = [&](CompileResourceDenial reason) -> std::optional<Continuation> {
  if (denial) *denial = reason;
  impl_->observe_rejection(lock, successor, resources, reason);
  return {};
 };
 if (!impl_->has_record(successor)) return reject(CompileResourceDenial::Records);
 const bool needs_resources = resources.bytes || resources.descriptors || resources.continuation_descriptors || resources.opaque_bytes;
 if (needs_resources) {
  Demand local{.resources = resources, .parent = parent, .finite_consumer = finite_consumer};
  auto& demand = waiting ? *waiting : local;
  if (!impl_->resource_eligible(demand)) return reject(demand.denial);
  impl_->refresh_aged();
  if (impl_->fairness_denied(resources, waiting)) return reject(CompileResourceDenial::AgedDemand);
 }
 auto completion = std::make_shared<CompletionState>();
 completion->owner = impl_;
 if (needs_resources) allowance = impl_->charge(resources, parent, finite_consumer);
 // A failed record-link allocation returns charged custody after unlocking.
 auto job = impl_->claim_record(completion);
 if (needs_resources) ++impl_->selections;
 job->task.kind = successor;
 job->successor = successor;
 job->retained = true;
 job->reason = CompileParkReason::Continuation;
 completion->done = true; // No predecessor or callback remains to settle.
 ++impl_->classes[index(work_class(successor))].pending;
 if (deferred) impl_->remove_wait(*deferred->state_);
 if (impl_->observer) ++impl_->admitted;
 impl_->reconcile();
 impl_->notify_locked(CompileChange::Cpu);
 Continuation retained(impl_, std::move(completion));
 retained.allowance_ = std::move(allowance);
 if (denial) *denial = CompileResourceDenial::None;
 return retained;
}
CompileExecution::Continuation CompileExecution::retain_current(CompileTaskKind successor) {
 (void)work_class(successor);
 if (!on_lane() || !Impl::Frame::current->job) throw std::logic_error("compile continuation requires a current task record");
 const auto& job = Impl::Frame::current->job;
 const std::lock_guard lock(impl_->admission->mutex);
 if (job->retained) throw std::logic_error("compile task already retained its record");
 job->retained = true;
 job->successor = successor;
 return Continuation(impl_, job->completion);
}
std::optional<CompileExecution::Completion> CompileExecution::Continuation::try_submit(Task task) {
 if (!owner_) throw std::logic_error("compile continuation has no record");
 auto owner = owner_;
 std::unique_lock lock(owner->admission->mutex);
 owner->check();
 auto job = completion_->job.lock();
 if (!job || job->done || job->completion != completion_ || !job->retained) throw std::logic_error("compile continuation record retired");
 if (job->cancelled) std::rethrow_exception(owner->retired);
 if (task.kind != job->successor) throw std::invalid_argument("compile continuation changed its task kind");
 task.resources.workspace_class = work_class(task.kind);
 if (allowance_) {
  if (task.allowance && !task.allowance.aliases(allowance_)) throw std::invalid_argument("compile continuation changed its retained allowance");
  task.allowance = allowance_;
 }
 owner->validate(task);
 // Completion callbacks publish readiness before relinquishing their task.
 // The record-availability notification at that boundary retries this draw.
 if (job->reason != CompileParkReason::Continuation) return {};
 auto completion = std::make_shared<CompletionState>();
 completion->owner = owner;
 completion->job = job;
 try {
  if (task.completed) completion->callbacks.push_back(std::move(task.completed));
  // Allocate the queue link before changing the parked record's identity.
  owner->ready[static_cast<std::size_t>(task.kind)].jobs.push_back(job);
 } catch (...) {
  // A callback capture may own credits; destroy it outside admission locks.
  lock.unlock();
  throw;
 }
 job->task = std::move(task);
 job->completion = completion;
 job->retained = false;
 job->demand = Demand{.resources = job->task.resources, .parent = job->task.parent, .finite_consumer = job->task.finite_consumer};
 if (!job->task.allowance && owner->admission->oversized(job->task.resources)) owner->exceptional.append(*job);
 owner->park(*job, CompileParkReason::None);
 if (owner->observer) { job->ready_at = std::chrono::steady_clock::now(); job->denial = CompileResourceDenial::None; }
 completion_.reset();
 owner_.reset();
 // The queued task now retains the complete admitted envelope.
 allowance_ = {};
 owner->reconcile();
 owner->notify_locked();
 return Completion(std::move(completion));
}
std::optional<CompileExecution::Completion> CompileExecution::submit_impl(Task task, const std::shared_ptr<DependencyState>& dependency, std::uint64_t observed) {
 CompileObservationClock lock_clock{bool(impl_->observer)};
 CompileAllowance retired_demand_parent;
 std::unique_lock lock(impl_->admission->mutex);
 if (impl_->observer) impl_->admission->admission_lock_ns += lock_clock.elapsed();
 task.resources.workspace_class = work_class(task.kind);
 impl_->check();
 impl_->validate(task);
 if (impl_->observer) ++impl_->costs.submission_attempts;
 if (!impl_->has_record(task.kind)) {
  impl_->observe_rejection(lock, task.kind, task.resources, CompileResourceDenial::Records);
  return {};
 }
 if (dependency && dependency->owner.lock() != impl_) throw std::invalid_argument("compile dependency belongs to another session");
 auto completion = std::make_shared<CompletionState>();
 completion->owner = impl_;
 if (task.completed) completion->callbacks.push_back(std::move(task.completed));
 std::shared_ptr<Job> job;
 try { job = impl_->claim_record(completion); }
 catch (...) {
  // Callback captures can own resource custody and retire outside this lock.
  lock.unlock();
  throw;
 }
 job->task = std::move(task);
 try {
  if (dependency && dependency->generation == observed) {
   std::erase_if(dependency->waiting, [&](const auto& value) {
    const auto waiting = value.lock();
    return !waiting || waiting == job || waiting->done || waiting->dependency != dependency;
   });
   job->reason = CompileParkReason::Dependency;
   job->dependency = dependency;
   dependency->waiting.push_back(job);
  } else
   impl_->enqueue(job);
 } catch (...) {
  impl_->unindex(*job);
  retired_demand_parent = std::move(job->demand.parent);
  impl_->jobs.remove(job);
  job->done = true;
  impl_->available_records.push_back(job);
  task = std::move(job->task);
  auto retired_completion = std::exchange(job->completion, {});
  lock.unlock();
  throw;
 }
 ++impl_->classes[index(work_class(job->task.kind))].pending;
 if (impl_->observer) ++impl_->admitted;
 impl_->reconcile();
 impl_->notify_locked();
 return Completion(job->completion);
}
std::optional<CompileExecution::Completion> CompileExecution::try_submit(Task task) { return submit_impl(std::move(task), {}, 0); }
CompileExecution::Completion CompileExecution::submit(Task task) {
 if (on_lane()) throw std::logic_error("compile CPU lane cannot wait for task-record capacity");
 for (;;) {
  const auto observed = admission_generation();
  if (auto accepted = try_submit(task)) return *accepted;
  wait_for_admission_change(observed);
 }
}
std::optional<CompileExecution::Completion> CompileExecution::try_subscribe(Dependency& dependency, std::uint64_t observed, Task task) {
 return submit_impl(std::move(task), dependency.state_, observed);
}
void CompileExecution::resume(const Completion& completion) noexcept {
 if (!completion.state_ || completion.state_->owner.lock() != impl_) return;
 const std::lock_guard lock(impl_->admission->mutex);
 if (auto job = completion.state_->job.lock(); job && !job->done && job->completion == completion.state_ && job->reason == CompileParkReason::Dependency) {
  job->dependency.reset();
  impl_->enqueue(job);
  impl_->notify_locked();
 }
}
void CompileExecution::fail(std::exception_ptr error) noexcept {
 const std::lock_guard lock(impl_->admission->mutex);
 if (!impl_->failure) impl_->failure = std::move(error);
 impl_->notify_locked(CompileAdmission::Change::Interruption);
}
void CompileExecution::cancel() noexcept {
 const std::lock_guard lock(impl_->admission->mutex);
 impl_->stopping = true;
 impl_->notify_locked(CompileAdmission::Change::Interruption);
}
std::exception_ptr CompileExecution::interruption() const noexcept {
 const std::lock_guard lock(impl_->admission->mutex);
 return impl_->failure ? impl_->failure : impl_->cancellation.requested() ? impl_->cancellation_error : impl_->stopping || impl_->shutdown ? impl_->retired : std::exception_ptr{};
}
void CompileExecution::join() {
 if (on_lane()) throw std::logic_error("compile CPU lane cannot join execution");
 std::unique_lock lock(impl_->admission->mutex);
 impl_->wait(lock, [&] { return impl_->jobs.empty(); });
 impl_->check();
}
void CompileExecution::retire_attempt() noexcept {
 cancel();
 std::unique_lock lock(impl_->admission->mutex);
 impl_->wait(lock, [&] { return impl_->jobs.empty() && impl_->admission->active == 0 && impl_->services == 0; });
 for (std::size_t lane = 0; lane < impl_->lanes.size(); ++lane) {
  auto& state = impl_->lanes[lane];
  impl_->wait(lock, [&] { return !state.retiring; });
  if (!state.idle.workspace) continue;
  auto idle = std::exchange(state.idle, {});
  state.retiring = idle.workspace;
  lock.unlock();
  impl_->release_idle(std::move(idle), lane);
  lock.lock();
 }
 impl_->failure = {};
 impl_->stopping = false;
 impl_->notify_locked();
}
bool CompileExecution::on_lane() const noexcept { return Impl::Frame::current && &Impl::Frame::current->owner == impl_.get(); }
std::size_t CompileExecution::current_lane() const {
 if (!on_lane()) throw std::logic_error("compile lane requested outside CPU work");
 return Impl::Frame::current->lane;
}
CompileAllowance CompileExecution::current_allowance() const {
 if (!on_lane()) throw std::logic_error("compile allowance requested outside CPU work");
 return Impl::Frame::current->allowance;
}
std::size_t CompileExecution::workers() const noexcept { return impl_->cpus.size(); }
std::span<const int> CompileExecution::cpus() const noexcept { return impl_->cpus; }
const CompileWorkspacePlan& CompileExecution::workspace_plan() const noexcept { return impl_->admission->arenas->plan(); }
std::size_t CompileExecution::workspace_capacity(bool completion, CompileWorkClass kind, CompileWorkspaceWindow window) const noexcept {
 return impl_->admission->arenas->normal_capacity(completion, kind, window);
}
CompileExecutionSnapshot CompileExecution::snapshot() const {
 const std::lock_guard lock(impl_->admission->mutex);
 return impl_->snapshot_locked();
}
std::uint64_t CompileExecution::transient_target() const noexcept { return impl_->admission->target; }
std::size_t CompileExecution::descriptor_limit() const noexcept { return impl_->admission->descriptor_capacity; }
CompileResources CompileExecution::resource_usage() const {
 const std::lock_guard lock(impl_->admission->mutex);
 const auto& state = *impl_->admission;
 return {state.bytes + state.handle_bytes, state.descriptors, false, state.active + state.external_cpus, false, state.committed};
}
bool CompileExecution::resource_pressure() const {
 const std::lock_guard lock(impl_->admission->mutex);
 return impl_->admission->pressure();
}
bool CompileExecution::oversized_workspace_pending() const {
 const std::lock_guard lock(impl_->admission->mutex);
 return impl_->draining_request != nullptr;
}
std::uint64_t CompileExecution::admission_generation() const {
 const std::lock_guard lock(impl_->admission->mutex);
 return impl_->admission->generation;
}
void CompileExecution::wait_for_admission_change(std::uint64_t observed, std::chrono::steady_clock::time_point deadline) {
 if (on_lane()) throw std::logic_error("compile CPU lane cannot wait for admission");
 std::unique_lock lock(impl_->admission->mutex);
 CompileAdmission::Waiter waiter(*impl_->admission);
 impl_->wait(lock, [&] {
  impl_->check();
  return impl_->admission->generation != observed || std::chrono::steady_clock::now() >= deadline;
 }, deadline);
}
void CompileExecution::notify_change(CompileChange change) noexcept {
 const std::lock_guard lock(impl_->admission->mutex);
 impl_->notify_locked(change);
}
std::uint64_t CompileExecution::resource_generation() const {
 const std::lock_guard lock(impl_->admission->mutex);
 return impl_->admission->resource_generation;
}
CompileExecution::Readiness::Readiness(CompileExecution& owner, CompileChange interests, std::function<void(Changes)> callback) : owner_(owner), state_(std::make_shared<State>()) {
 state_->callback = std::move(callback);
 state_->interests = interests | CompileChange::Interruption;
 const std::lock_guard lock(owner_.impl_->admission->mutex);
 // Control owners remain independently live even with one prepared CPU task
 // record. Keep bounded class-owner headroom for their readiness registrations.
 if (owner_.impl_->observers.size() >= std::max(owner_.impl_->record_limit, class_count)) throw std::length_error("compile readiness capacity exceeded");
 owner_.impl_->observers.push_back(state_);
}
void CompileExecution::Readiness::interests(CompileChange interests) noexcept {
 const std::lock_guard lock(owner_.impl_->admission->mutex);
 state_->interests = interests | CompileChange::Interruption;
}
void CompileExecution::Readiness::notify() noexcept {
 const auto& impl = owner_.impl_;
 const std::lock_guard lock(impl->admission->mutex);
 ++impl->admission->generation;
 impl->admission->changed.notify_all();
 state_->pending.causes |= CompileChange::Local;
 state_->notified = true;
 signal_event(impl->event.get());
}
CompileExecution::Readiness::~Readiness() {
 // Keep state_ alive while unlinking. Publication never takes or destroys a
 // final callback owner under the admission lock.
 {
  const std::lock_guard lock(owner_.impl_->admission->mutex);
  std::erase(owner_.impl_->observers, state_);
 }
 std::unique_lock lock(state_->mutex);
 state_->removed = true;
 state_->changed.wait(lock, [&] { return !state_->active; });
}
thread_local CompileExecution::Readiness::Attempt* CompileExecution::Readiness::Attempt::current_ = nullptr;
CompileExecution::Readiness::Attempt::Attempt(Readiness* owner) : owner_(owner), previous_(current_) { current_ = this; }
CompileExecution::Readiness::Attempt::~Attempt() {
 current_ = previous_;
 if (!owner_ || !committed_ || causes_ == CompileChange::None) return;
 const auto& impl = owner_->owner_.impl_;
 const std::lock_guard lock(impl->admission->mutex);
 auto& state = *owner_->state_;
 state.pending.causes |= causes_;
 state.pending.retry = true;
 state.notified = true;
 signal_event(impl->event.get());
}
CompileResourceWait::CompileResourceWait(std::shared_ptr<CompileExecution::Impl> execution, CompileTaskKind successor, CompileResources resources, CompileAllowance parent, bool finite_consumer)
    : state_(std::make_unique<State>(std::move(execution), successor, resources, std::move(parent), finite_consumer)) {
 auto& state = *state_;
 auto& owner = *state.execution;
 const std::lock_guard lock(owner.admission->mutex);
 if (state.demand.parent.credits_ && state.demand.parent.credits_->owner != owner.admission) throw std::invalid_argument("compile demand belongs to another session");
 // The successor and effective class are already bound. Every fallible check
 // precedes intrusive publication; the remaining mutations cannot allocate.
 owner.admission->require(state.demand.resources, state.demand.needs_record);
 (void)owner.admission->reservation_bytes(state.demand.resources);
 const bool exceptional = owner.admission->oversized(state.demand.resources);
 if (exceptional) {
  owner.external_demands.append(state);
  owner.refresh_external_drain();
 }
 if (state.demand.needs_record) owner.wait_demand(state.demand);
 owner.admission->change_waiters(true, true);
 state.registered = true;
}
CompileResourceWait::~CompileResourceWait() {
 // Unlink while the state still owns its parent, then release final custody
 // after unlocking. A successful retain has already removed this registration.
 const std::lock_guard lock(state_->execution->admission->mutex);
 state_->execution->remove_wait(*state_);
}
std::unique_ptr<CompileResourceWait> CompileExecution::defer_resources(CompileResources resources, const CompileAllowance& parent) {
 return std::unique_ptr<CompileResourceWait>(new CompileResourceWait(impl_, CompileTaskKind::Count, resources, parent, false));
}
std::unique_ptr<CompileResourceWait> CompileExecution::defer_continuation(CompileTaskKind successor, CompileResources resources, const CompileAllowance& parent, bool finite_consumer) {
 resources.workspace_class = work_class(successor);
 return std::unique_ptr<CompileResourceWait>(new CompileResourceWait(impl_, successor, resources, parent, finite_consumer));
}
void CompileExecution::require_feasible(CompileResources resources) const {
 const std::lock_guard lock(impl_->admission->mutex);
 impl_->admission->require(resources);
}
std::size_t CompileExecution::descriptor_ceiling(CompileResources resources) const {
 const std::lock_guard lock(impl_->admission->mutex);
 return impl_->admission->descriptor_ceiling(resources);
}
std::optional<CompileAllowance> CompileExecution::try_reserve(CompileResources resources, const CompileAllowance& parent, std::source_location location) {
 return try_reserve_observed(resources, parent, location, nullptr);
}
std::optional<CompileAllowance> CompileExecution::try_reserve_observed(CompileResources resources, const CompileAllowance& parent, std::source_location location, CompileExecutionSnapshot* observation) {
 CompileObservationClock lock_clock{bool(impl_->observer)};
 const std::lock_guard lock(impl_->admission->mutex);
 if (impl_->observer) impl_->admission->admission_lock_ns += lock_clock.elapsed();
 impl_->check();
 if (parent.credits_ && parent.credits_->owner != impl_->admission) throw std::invalid_argument("compile parent allowance belongs to another session");
 const auto denied = [&](CompileResourceDenial denial) -> std::optional<CompileAllowance> {
  if (observation) try {
     *observation = impl_->snapshot_locked();
     auto& request = observation->reservation.emplace();
     request.location = location;
     request.requested = resources;
     request.denial = denial;
     request.resource_epoch = impl_->admission->resource_generation;
     request.largest_eligible_extent = impl_->admission->arenas->largest_available(resources.completion_workspace, resources.workspace_class, resources.workspace_window);
     const auto credit_observation = [](const Credits* credit) {
      return CompileCreditObservation{
       reinterpret_cast<std::uintptr_t>(credit), reinterpret_cast<std::uintptr_t>(credit->parent.get()), credit->location, credit->resources, credit->available, credit->borrowed,
       credit->descriptors_retired, credit->continuations_released
      };
     };
     for (auto* credit = parent.credits_.get(); credit; credit = credit->parent.get()) request.parents.push_back(credit_observation(credit));
     for (const auto* credit : impl_->admission->observed_credits)
      if (credit->resources.bytes || credit->resources.descriptors || credit->available) request.owners.push_back(credit_observation(credit));
  } catch (...) { observation->costs.incomplete = true; }
  return {};
 };
 const auto eligibility = impl_->admission->eligibility(resources, parent.credits_.get());
 if (!eligibility.fits) return denied(eligibility.denial);
 auto credit = std::make_shared<Credits>(impl_->admission, resources, parent.credits_);
 if (resources.cpu_workers) {
  credit->cpu_lanes.reserve(resources.cpu_workers);
  credit->cpus.reserve(resources.cpu_workers);
  for (std::size_t lane = 0; lane < impl_->lanes.size() && credit->cpu_lanes.size() < resources.cpu_workers; ++lane) {
   const auto& state = impl_->lanes[lane];
   if (!state.busy() && state.baseline == CompileWorkClass::Count) {
    credit->cpu_lanes.push_back(lane);
    credit->cpus.push_back(impl_->cpus[lane]);
   }
  }
  if (credit->cpu_lanes.size() != resources.cpu_workers) return denied(CompileResourceDenial::Cpu);
 }
 impl_->admission->charge(*credit, false, location);
 for (auto lane : credit->cpu_lanes) impl_->lanes[lane].external = true;
 return CompileAllowance(std::move(credit));
}
CompileAllowance CompileExecution::reserve(CompileResources resources, const CompileAllowance& parent, std::source_location location) {
 if (on_lane()) throw std::logic_error("compile CPU lane cannot wait for resource credits");
 require_feasible(resources);
 std::unique_ptr<CompileResourceWait> waiter;
 bool reported = false;
 for (;;) {
  const auto observed = resources.cpu_workers ? admission_generation() : resource_generation();
  CompileExecutionSnapshot observation;
  const bool report = impl_->observer && !reported;
  if (auto value = try_reserve_observed(resources, parent, location, report ? &observation : nullptr)) return std::move(*value);
  if (report) {
   reported = true;
   try { impl_->observer("compile.resource.wait", observation); } catch (...) {}
  }
  if (!waiter) waiter = defer_resources(resources, parent);
  if (resources.cpu_workers) wait_for_admission_change(observed);
  else {
   std::unique_lock lock(impl_->admission->mutex);
   impl_->wait(lock, [&] { impl_->check(); return impl_->admission->resource_generation != observed; });
  }
 }
}
bool CompileExecution::try_resize_workspace(const CompileAllowance& allowance, std::uint64_t bytes, bool retain_capacity) {
 const auto& credits = allowance.credits_;
 if (!credits) return bytes == 0;
 if (credits->owner != impl_->admission) throw std::invalid_argument("compile allowance belongs to another session");
 if (on_lane()) throw std::logic_error("compile workspace resize inside a CPU lane");
 enum class Resize { Complete, Active, Pressure };
 const auto resize = [&] {
  const std::lock_guard lock(impl_->admission->mutex);
  auto& owner = *impl_->admission;
  impl_->check();
  auto& resources = credits->resources;
  if (credits->cpu_users) return Resize::Active;
  if (credits->arena || resources.retained_handles || !credits->workspace_loans.empty() || credits->workspace_offer) throw std::logic_error("compile workspace resize requires settled own custody");
  if (bytes == resources.bytes || (retain_capacity && !owner.resource_waiters && bytes < resources.bytes)) return Resize::Complete;
  if (bytes > resources.bytes) {
   const auto growth = bytes - resources.bytes;
   if (!owner.fits_bytes({growth, 0}, credits.get())) return Resize::Pressure;
   owner.bytes = mmltk::common::math::checked_add(owner.bytes, growth, "compile workspace resize overflow");
  } else
   owner.bytes -= resources.bytes - bytes;
  resources.bytes = bytes;
  impl_->notify_locked(CompileAdmission::Change::Resources);
  return Resize::Complete;
 };
 auto result = resize();
 for (std::size_t lane = 0; result == Resize::Pressure && lane < impl_->lanes.size(); ++lane) {
  Impl::Idle idle;
  {
   const std::lock_guard lock(impl_->admission->mutex);
   auto& state = impl_->lanes[lane];
   if (state.busy() || !state.idle.workspace) continue;
   idle = std::exchange(state.idle, {});
   state.retiring = idle.workspace;
  }
  impl_->release_idle(std::move(idle), lane);
  result = resize();
 }
 return result == Resize::Complete;
}
void CompileExecution::with_unused_workspace(const CompileAllowance& allowance, std::uint64_t live_bytes, const std::function<void()>& callback) {
 if (on_lane()) throw std::logic_error("compile workspace lending cannot wait inside a CPU lane");
 if (!allowance.credits_ || allowance.credits_->owner != impl_->admission) throw std::invalid_argument("compile workspace offer requires owned credits");
 {
  const auto offer = [&] {
   const std::lock_guard lock(impl_->admission->mutex);
   impl_->check();
   return CompileAdmission::OfferScope(allowance.credits_, live_bytes);
  }();
  callback();
 }
 const std::lock_guard lock(impl_->admission->mutex);
 impl_->check();
}
CompileExecution::Workspace::Workspace(CompileExecution& owner, std::function<void(std::size_t)> retire) : owner_(owner), retire_(std::move(retire)) {
 if (!retire_) throw std::invalid_argument("compile workspace requires physical retirement");
}
CompileExecution::Workspace::~Workspace() { owner_.retire_workspace(this); }
void CompileExecution::retire_workspace(Workspace* workspace) noexcept {
 std::unique_lock lock(impl_->admission->mutex);
 impl_->wait(lock, [&] { return std::ranges::none_of(impl_->jobs, [&](const auto& job) { return !job->done && job->task.workspace == workspace; }); });
 for (std::size_t lane = 0; lane < impl_->lanes.size(); ++lane) {
  auto& state = impl_->lanes[lane];
  impl_->wait(lock, [&] { return state.retiring != workspace && !state.owns(workspace); });
  if (state.idle.workspace != workspace) continue;
  auto idle = std::exchange(state.idle, {});
  state.retiring = workspace;
  lock.unlock();
  impl_->release_idle(std::move(idle), lane);
  lock.lock();
 }
}
void CompileExecution::run(CompileTaskKind kind, CompileResources resources, const std::function<void(std::size_t)>& callback, CompileAllowance allowance) {
 if (on_lane()) {
  auto* parent = Impl::Frame::current;
  {
   const std::lock_guard lock(impl_->admission->mutex);
   if (allowance.credits_ && (allowance.credits_->owner != impl_->admission || !CompileAdmission::covers(*allowance.credits_, resources)))
    throw std::invalid_argument("nested compile allowance belongs to another task");
   if (resources.cpu_workers || (!allowance && (!parent->allowance || !CompileAdmission::covers(*parent->allowance.credits_, resources))))
    throw std::logic_error("nested compile work exceeds its parent's allowance");
  }
  Impl::Frame frame(*impl_, parent->lane, parent->grant_class);
  {
   const std::lock_guard lock(impl_->admission->mutex);
   frame.enter(allowance ? std::move(allowance) : parent->allowance, parent->workspace);
  }
  callback(frame.lane);
  return;
 }
 auto producer = open_class(work_class(kind));
 submit({.kind = kind, .resources = resources, .work = [callback](std::size_t lane, const CompileAllowance&) { callback(lane); }, .allowance = std::move(allowance)}).wait();
 const std::lock_guard lock(impl_->admission->mutex);
 impl_->check();
}
void CompileExecution::for_each(CompileTaskKind kind, std::size_t count, CompileResources resources, const std::function<void(std::size_t)>& callback, const std::function<void(std::size_t)>& retire) {
 for_each(kind, count, [resources](std::size_t) { return resources; }, callback, retire);
}
void CompileExecution::for_each(CompileTaskKind kind, std::size_t count, const std::function<CompileResources(std::size_t)>& resources, const std::function<void(std::size_t)>& callback,
 const std::function<void(std::size_t)>& retire) {
 if (!retire) {
  for_each_impl(kind, count, resources, callback, nullptr);
  return;
 }
 Workspace workspace(*this, retire);
 std::exception_ptr failure;
 try {
  for_each(kind, count, resources, callback, workspace);
 } catch (...) { failure = std::current_exception(); }
 retire_workspace(&workspace);
 {
  const std::lock_guard lock(impl_->admission->mutex);
  if (!failure) failure = workspace.failure_;
 }
 if (failure) std::rethrow_exception(failure);
}
void CompileExecution::for_each(
 CompileTaskKind kind, std::size_t count, const std::function<CompileResources(std::size_t)>& resources, const std::function<void(std::size_t)>& callback, Workspace& workspace) {
 if (&workspace.owner_ != this) throw std::invalid_argument("compile workspace belongs to another session");
 {
  const std::lock_guard lock(impl_->admission->mutex);
  if (workspace.active_) throw std::logic_error("compile workspace already in use");
  workspace.active_ = true;
 }
 try {
  for_each_impl(kind, count, resources, callback, &workspace);
 } catch (...) {
  const std::lock_guard lock(impl_->admission->mutex);
  workspace.active_ = false;
  throw;
 }
 const std::lock_guard lock(impl_->admission->mutex);
 workspace.active_ = false;
 if (workspace.failure_) std::rethrow_exception(workspace.failure_);
}
void CompileExecution::for_each_impl(
 CompileTaskKind kind, std::size_t count, const std::function<CompileResources(std::size_t)>& resources, const std::function<void(std::size_t)>& callback, Workspace* workspace) {
 if (!count) return;
 if (on_lane()) {
  auto* parent = Impl::Frame::current;
  bool borrowed;
  Impl::Idle previous;
  {
   const std::lock_guard lock(impl_->admission->mutex);
   auto& lane = impl_->lanes[parent->lane];
   borrowed = lane.owns(workspace);
   if (workspace && lane.retiring == workspace) throw std::logic_error("nested compile workspace is retiring");
   for (std::size_t i = 0; i < count; ++i) {
    impl_->admission->require(resources(i), true);
    if (!parent->allowance || !CompileAdmission::covers(*parent->allowance.credits_, resources(i))) throw std::logic_error("nested compile chunks exceed their parent's allowance");
   }
   if (workspace && lane.idle.workspace == workspace) {
    previous = std::exchange(lane.idle, {});
    lane.retiring = workspace;
   }
  }
  Impl::Frame frame(*impl_, parent->lane, parent->grant_class);
  {
   const std::lock_guard lock(impl_->admission->mutex);
   frame.enter(parent->allowance, workspace ? workspace : parent->workspace);
  }
  if (previous.workspace) impl_->release_idle(std::move(previous), frame.lane);
  std::exception_ptr error;
  try {
   for (std::size_t i = 0; i < count; ++i) {
    if (impl_->cancellation.requested()) std::rethrow_exception(impl_->cancellation_error);
    callback(i);
   }
  } catch (...) { error = std::current_exception(); }
  if (workspace && !borrowed) try {
    workspace->retire_(frame.lane);
   } catch (...) {
    if (!error) error = std::current_exception();
   }
  if (error) std::rethrow_exception(error);
  return;
 }
 auto producer = open_class(work_class(kind));
 // Siblings observe the first source failure before invoking borrowed work,
 // even when a freed lane races the external group's join/cancel loop.
 std::mutex group_mutex;
 std::exception_ptr group_failure;
 const auto invoke = [&](std::size_t item) {
  {
   const std::lock_guard lock(group_mutex);
   if (group_failure) std::rethrow_exception(group_failure);
  }
  try {
   callback(item);
  } catch (...) {
   const std::lock_guard lock(group_mutex);
   if (!group_failure) group_failure = std::current_exception();
   throw;
  }
 };
 std::vector<Completion> active;
 active.reserve(std::min(count, workers() * 2));
 std::size_t next = 0;
 std::exception_ptr failure;
 try {
  while (next < count || !active.empty()) {
   const auto observed = admission_generation();
   for (auto position = active.begin(); position != active.end();) {
    if (!position->ready()) {
     ++position;
     continue;
    }
    position->wait();
    position = active.erase(position);
   }
   while (next < count && active.size() < active.capacity()) {
    auto task = try_submit({.kind = kind, .resources = resources(next), .work = [&, item = next](std::size_t, const CompileAllowance&) { invoke(item); }, .workspace = workspace});
    if (!task) break;
    active.push_back(std::move(*task));
    ++next;
   }
   if (next < count || !active.empty()) wait_for_admission_change(observed);
  }
 } catch (...) {
  failure = std::current_exception();
  for (auto& task : active) task.cancel();
 }
 for (auto& task : active) try {
   task.wait();
  } catch (...) {
   if (!failure) failure = std::current_exception();
  }
 if (failure) std::rethrow_exception(failure);
 const std::lock_guard lock(impl_->admission->mutex);
 impl_->check();
}
void CompileExecution::cooperate() {
 if (!on_lane()) throw std::logic_error("compile cooperative yield requires a CPU lane");
 if (impl_->cancellation.requested()) std::rethrow_exception(impl_->cancellation_error);
 (void)impl_->consume(current_lane(), false);
 if (impl_->cancellation.requested()) std::rethrow_exception(impl_->cancellation_error);
}
CompileExecution::ServiceGrant::ServiceGrant(std::shared_ptr<Impl> owner, CompileWorkClass kind, std::size_t lane) : owner_(std::move(owner)), class_(kind), lane_(lane) {}
CompileExecution::ServiceGrant::~ServiceGrant() { release(); }
CompileExecution::ServiceGrant::ServiceGrant(ServiceGrant&& other) noexcept : owner_(std::move(other.owner_)), class_(other.class_), lane_(other.lane_) {}
CompileExecution::ServiceGrant& CompileExecution::ServiceGrant::operator=(ServiceGrant&& other) noexcept {
 if (this != &other) {
  release();
  owner_ = std::move(other.owner_);
  class_ = other.class_;
  lane_ = other.lane_;
 }
 return *this;
}
void CompileExecution::ServiceGrant::release() noexcept {
 if (!owner_) return;
 auto owner = std::move(owner_);
 const std::lock_guard lock(owner->admission->mutex);
 owner->lanes[lane_].service = CompileWorkClass::Count;
 --owner->admission->active;
 --owner->services;
 --owner->classes[index(class_)].running;
 owner->reconcile();
 owner->notify_locked();
}
int CompileExecution::ServiceGrant::cpu() const noexcept { return owner_ ? owner_->cpus[lane_] : -1; }
CompileExecution::ServiceDemand::ServiceDemand(CompileExecution& owner) : owner_(owner.impl_) {}
CompileExecution::ServiceDemand::~ServiceDemand() { update({}); }
void CompileExecution::ServiceDemand::update_locked(const Classes& classes) noexcept {
 if (pending_ == classes) return;
 for (std::size_t kind = 0; kind < class_count; ++kind) {
  if (pending_[kind] == classes[kind]) continue;
  if (classes[kind])
   ++owner_->service_waiters[kind];
  else
   --owner_->service_waiters[kind];
 }
 pending_ = classes;
 owner_->notify_locked();
}
void CompileExecution::ServiceDemand::update(const Classes& classes) noexcept {
 const std::lock_guard lock(owner_->admission->mutex);
 update_locked(classes);
}
std::optional<CompileExecution::ServiceGrant> CompileExecution::ServiceDemand::try_grant_locked(std::stop_token local_stop) {
 owner_->check();
 if (local_stop.stop_requested()) throw std::runtime_error("compile service admission cancelled");
 if (owner_->admission->active + owner_->admission->external_cpus >= owner_->lanes.size()) return {};
 for (std::size_t turn = 0; turn < class_count; ++turn) {
  const auto kind = (owner_->cursor + turn) % class_count;
  if (!pending_[kind]) continue;
  if (!owner_->floors) {
   for (std::size_t earlier = 0; earlier < turn; ++earlier)
    if (owner_->service_waiters[(owner_->cursor + earlier) % class_count]) return {};
   // Compute and services use one cursor. Earlier runnable compute receives a
   // bounded turn before handing the cursor to a waiting service class.
   if (turn && std::ranges::any_of(owner_->ready, [](const auto& queue) { return !queue.jobs.empty(); })) return {};
  }
  const auto service_class = static_cast<CompileWorkClass>(kind);
  for (bool baseline : {true, false})
   for (std::size_t lane_index = 0; lane_index < owner_->lanes.size(); ++lane_index) {
    auto& lane = owner_->lanes[lane_index];
    if (lane.busy()) continue;
    if (!((baseline && lane.baseline == service_class) || (!baseline && (!owner_->floors || lane.baseline == CompileWorkClass::Count)))) continue;
    update_locked({});
    owner_->cursor = (kind + 1) % class_count;
    lane.service = service_class;
    ++owner_->admission->active;
    ++owner_->services;
    ++owner_->classes[kind].running;
    owner_->notify_locked();
    return ServiceGrant(owner_, service_class, lane_index);
   }
 }
 return {};
}
std::optional<CompileExecution::ServiceGrant> CompileExecution::ServiceDemand::try_grant(std::stop_token local_stop) {
 const std::lock_guard lock(owner_->admission->mutex);
 return try_grant_locked(local_stop);
}
CompileExecution::ServiceGrant CompileExecution::service_grant(CompileWorkClass kind, std::stop_token local_stop) {
 if (index(kind) >= class_count) throw std::invalid_argument("invalid compile service class");
 if (on_lane()) throw std::logic_error("compile CPU lane cannot wait for a service grant");
 ServiceDemand demand(*this);
 ServiceDemand::Classes classes{};
 classes[index(kind)] = true;
 demand.update(classes);
 // Register before taking the admission lock. Cancellation synchronizes with
 // the predicate/wait transition; callback destruction follows lock release.
 const std::stop_callback interrupted(local_stop, [owner = impl_] {
  const std::lock_guard lock(owner->admission->mutex);
  owner->admission->changed.notify_all();
 });
 std::unique_lock lock(impl_->admission->mutex);
 std::optional<ServiceGrant> grant;
 impl_->admission->changed.wait(lock, [&] {
  grant = demand.try_grant_locked(local_stop);
  return bool(grant);
 });
 return std::move(*grant);
}
}  // namespace mmltk::backend::data::detail
