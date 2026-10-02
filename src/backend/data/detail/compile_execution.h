#pragma once
#include "src/backend/data/detail/compile_resources.h"
#include "src/backend/data/detail/compile_workspace.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string_view>
#include <stop_token>
#include <utility>
#include <vector>
namespace mmltk::backend::data::detail {
enum class CompileTaskKind : std::uint8_t { Acquisition, Metadata, Header, Projection, Archive, Normalize, Recovery, Labels, Pixels, Gather, CacheWrite, Output, Count };
enum class CompileParkReason : std::uint8_t { None, Dependency, Bytes, Descriptors, Cpu, Withdrawal, Continuation, Count };
[[nodiscard]] constexpr CompileWorkClass work_class(CompileTaskKind kind) {
 switch (kind) {
  case CompileTaskKind::Acquisition: return CompileWorkClass::Acquisition;
  case CompileTaskKind::Header:
  case CompileTaskKind::Projection:
  case CompileTaskKind::Archive: return CompileWorkClass::Extraction;
  case CompileTaskKind::Metadata:
  case CompileTaskKind::Normalize:
  case CompileTaskKind::Recovery:
  case CompileTaskKind::Labels: return CompileWorkClass::Annotations;
  case CompileTaskKind::Pixels: return CompileWorkClass::Pixels;
  case CompileTaskKind::Gather: return CompileWorkClass::Gather;
  case CompileTaskKind::CacheWrite:
  case CompileTaskKind::Output: return CompileWorkClass::Output;
  case CompileTaskKind::Count: break;
 }
 throw std::invalid_argument("invalid compile task kind");
}
// Collected only for an enabled observer. Identities describe credit ancestry;
// they never participate in admission, ordering or resource lifetime.
struct CompileCreditObservation {
 std::uintptr_t id = 0, parent = 0;
 std::source_location location;
 CompileResources resources;
 std::size_t available = 0, borrowed = 0;
 bool descriptors_retired = false, continuations_released = false;
};
struct CompileReservationObservation {
 std::source_location location;
 CompileResources requested;
 CompileResourceDenial denial = CompileResourceDenial::None;
 std::uint64_t resource_epoch = 0;
 std::uint64_t largest_eligible_extent = 0;
 std::vector<CompileCreditObservation> parents, owners;
};
struct CompileAdmissionObservation {
 CompileTaskKind kind = CompileTaskKind::Count;
 CompileResourceDenial denial = CompileResourceDenial::None;
 std::uint64_t requested_bytes = 0, opaque_bytes = 0, resource_epoch = 0;
 std::size_t descriptors = 0, continuation_descriptors = 0, cpu_workers = 0;
 CompileWorkspaceWindow window = CompileWorkspaceWindow::Task;
 CompileWorkClass work_class = CompileWorkClass::Count;
 bool supplied_workspace = false, completion_workspace = false, producer = false, retained_handles = false;
 std::uint64_t largest_eligible_extent = 0;
};
MMLTK_REFLECT_FIELDS(CompileAdmissionObservation)
struct CompileExecutionCosts {
 std::uint64_t eligibility_probes = 0, notifier_deliveries = 0, resource_epoch = 0, admission_lock_ns = 0;
 std::uint64_t callback_ns = 0, completion_callback_ns = 0, workers_created = 0, workers_retired = 0;
 std::uint64_t submission_attempts = 0, record_denials = 0;
 bool incomplete = false;
};
MMLTK_REFLECT_FIELDS(CompileExecutionCosts)
struct CompileExecutionSnapshot {
 static constexpr std::size_t classes = static_cast<std::size_t>(CompileWorkClass::Count);
 std::array<bool, classes> live{};
 std::array<std::size_t, classes> baseline{}, ready{}, running{};
 std::array<std::size_t, static_cast<std::size_t>(CompileParkReason::Count)> parked{};
 std::size_t elastic = 0, active = 0, external = 0, services = 0, records = 0, record_limit = 0;
 CompileResources resources{};
 std::uint64_t byte_target = 0;
 std::uint64_t normal_locked = 0, resident = 0, leased = 0, exceptional = 0, exceptional_peak = 0, opaque_library = 0;
 std::uint64_t protected_workspace = 0, shared_workspace = 0, compaction_workspace = 0, exceptional_reserved = 0, workspace_reuses = 0, physical_retirements = 0;
 std::size_t workspace_contexts = 0, context_capacity = 0;
 std::size_t descriptor_limit = 0;
 std::uint64_t admitted = 0, completed = 0;
 // Physical bounded task calls by CompileTaskKind, collected only when observed.
 std::array<std::uint64_t, static_cast<std::size_t>(CompileTaskKind::Count)> task_completed{}, task_duration_ns{}, task_max_duration_ns{};
 std::optional<CompileReservationObservation> reservation;
 CompileArenaObservation arena{};
 CompileExecutionCosts costs{};
 std::vector<CompileAdmissionObservation> demands;
 std::array<std::uint64_t, static_cast<std::size_t>(CompileTaskKind::Count)> task_body_ns{}, task_cpu_ns{}, ready_to_start_ns{};
};
class CompileResourceWait;
class CompileExecution final {
 struct Impl;
 struct Job;
 struct CompletionState;
 struct DependencyState;
 using Credits = CompileAllowance::Credits;
 friend class CompileResourceWait;
 std::optional<CompileAllowance> try_reserve_observed(CompileResources, const CompileAllowance&, std::source_location, CompileExecutionSnapshot*);

public:
 using Observer = std::function<void(std::string_view, const CompileExecutionSnapshot&)>;
 explicit CompileExecution(std::size_t workers, std::span<const int> cpus = {}, CompileExecutionLimits = {}, mmltk::common::concurrency::CancellationObservation = {}, Observer = {},
  std::exception_ptr cancellation_error = {});
 ~CompileExecution();
 CompileExecution(const CompileExecution&) = delete;
 CompileExecution& operator=(const CompileExecution&) = delete;
 class Producer final {
 public:
  Producer() = default;
  ~Producer();
  Producer(Producer&&) noexcept;
  Producer& operator=(Producer&&) noexcept;
  Producer(const Producer&) = delete;
  Producer& operator=(const Producer&) = delete;
  void close() noexcept;

 private:
  friend class CompileExecution;
  Producer(std::shared_ptr<Impl>, CompileWorkClass);
  std::shared_ptr<Impl> owner_;
  CompileWorkClass class_ = CompileWorkClass::Count;
 };
 [[nodiscard]] Producer open_class(CompileWorkClass);
 // Each task owns its completion independently of reusable scheduler records.
 // Subscribers run once, outside locks, and only post completion/ready work.
 class CompletionSource;
 class Completion final {
 public:
  Completion() = default;
  [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
  [[nodiscard]] bool ready() const;
  [[nodiscard]] std::exception_ptr failure() const;
  void wait() const;
  void subscribe(std::function<void(std::exception_ptr)>) const;
  void cancel() const noexcept;

 private:
  friend class CompileExecution;
  friend class CompletionSource;
  friend struct Impl;
  explicit Completion(std::shared_ptr<CompletionState> state) : state_(std::move(state)) {}
  std::shared_ptr<CompletionState> state_;
 };
 class CompletionSource final {
 public:
  explicit CompletionSource(CompileExecution* owner = nullptr);
  [[nodiscard]] Completion completion() const { return Completion(state_); }
  void settle(std::exception_ptr = {}) const noexcept;

 private:
  std::shared_ptr<CompletionState> state_;
 };
 class Dependency final {
 public:
  explicit Dependency(CompileExecution&);
  [[nodiscard]] std::uint64_t generation() const;
  void notify() noexcept;

 private:
  friend class CompileExecution;
  friend struct Impl;
  std::shared_ptr<DependencyState> state_;
 };
 // Custody for source owners that still allocate their own input/parser window.
 // Supplied image/label work uses CompileAllowance arena leases directly; this
 // callback only retires actual separately owned source backing.
 class Workspace final {
 public:
  Workspace(CompileExecution&, std::function<void(std::size_t)> retire);
  ~Workspace();
  Workspace(const Workspace&) = delete;
  Workspace& operator=(const Workspace&) = delete;

 private:
  friend class CompileExecution;
  friend struct Impl;
  CompileExecution& owner_;
  std::function<void(std::size_t)> retire_;
  bool active_ = false;
  std::exception_ptr failure_;
 };
 struct Task {
  CompileTaskKind kind = CompileTaskKind::Metadata;
  CompileResources resources{};
  std::function<void(std::size_t, const CompileAllowance&)> work{};
  CompileAllowance allowance{}, parent{};
  Workspace* workspace = nullptr;
  // Only bounded consumers with retained immutable inputs may draw finite loans.
  bool finite_consumer = false;
  bool retain_workspace = true;
  std::function<void(std::exception_ptr)> completed{};
 };
 // Retain the current record across externally owned physical work, without a
 // CPU grant. The current task still settles its own Completion normally. Its
 // successor uses this same record after those callbacks return; successful
 // submission consumes the custody. Destruction returns an unused record.
 // Cancellation/join wait for the physical owner to submit or release custody.
 class Continuation final {
 public:
  Continuation() = default;
  ~Continuation();
  Continuation(Continuation&&) noexcept;
  Continuation& operator=(Continuation&&) noexcept;
  Continuation(const Continuation&) = delete;
  Continuation& operator=(const Continuation&) = delete;
  [[nodiscard]] explicit operator bool() const noexcept { return bool(owner_); }
  [[nodiscard]] std::optional<Completion> try_submit(Task);

 private:
  friend class CompileExecution;
  Continuation(std::shared_ptr<Impl>, std::shared_ptr<CompletionState>);
  void release() noexcept;
  std::shared_ptr<Impl> owner_;
  std::shared_ptr<CompletionState> completion_;
 };
 [[nodiscard]] Continuation retain_current(CompileTaskKind successor);
 [[nodiscard]] std::optional<Completion> try_submit(Task);
 [[nodiscard]] Completion submit(Task);
 // Capture generation before checking the owning product predicate. Registration
 // and the generation check share one lock, including cancellation and resume.
 [[nodiscard]] std::optional<Completion> try_subscribe(Dependency&, std::uint64_t observed, Task);
 void resume(const Completion&) noexcept;
 void fail(std::exception_ptr) noexcept;
 void cancel() noexcept;
 // Observe the current terminal cause without admission or allocation: first
 // failure, then cancellation, then retirement. Empty means still active.
 [[nodiscard]] std::exception_ptr interruption() const noexcept;
 void join();
 void retire_attempt() noexcept;
 [[nodiscard]] bool on_lane() const noexcept;
 [[nodiscard]] std::size_t current_lane() const;
 [[nodiscard]] CompileAllowance current_allowance() const;
 [[nodiscard]] std::size_t workers() const noexcept;
 [[nodiscard]] std::span<const int> cpus() const noexcept;
 [[nodiscard]] const CompileWorkspacePlan& workspace_plan() const noexcept;
 [[nodiscard]] CompileExecutionSnapshot snapshot() const;
 [[nodiscard]] std::uint64_t transient_target() const noexcept;
 [[nodiscard]] std::size_t descriptor_limit() const noexcept;
 [[nodiscard]] CompileResources resource_usage() const;
 [[nodiscard]] bool resource_pressure() const;
 // A legal oversized request needs every ordinary arena to retire. Ordinary
 // contention alone does not require a forward reader to discard its cursor.
 [[nodiscard]] bool oversized_workspace_pending() const;
 [[nodiscard]] std::uint64_t admission_generation() const;
 void wait_for_admission_change(std::uint64_t, std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());
 void notify_admission_change() noexcept;
 // A bounded owner registration receives readiness events, not CPU work. Its
 // destructor joins callbacks, so captured owner state cannot outlive custody.
 class Readiness final {
 public:
  struct Changes {
   bool resources = false, pressure = false, interrupted = false;
   // Resource/pressure changes from outside an unsuccessful local attempt.
   // CPU-only scheduler activity never requests a resource retry.
   bool retry = false;
  };
  Readiness(CompileExecution&, std::function<void(Changes)>);
  ~Readiness();
  Readiness(const Readiness&) = delete;
  Readiness& operator=(const Readiness&) = delete;
  // Thread-local provenance covers a nonblocking draw and all of its rollback.
  // Readers still receive its changes; this registration retries only external
  // changes unless the complete draw commits. Other registrations are unaffected.
  class Attempt final {
  public:
   explicit Attempt(Readiness*);
   ~Attempt();
   Attempt(const Attempt&) = delete;
   Attempt& operator=(const Attempt&) = delete;
   void commit() noexcept { committed_ = true; }

  private:
   friend struct Impl;
   static thread_local Attempt* current_;
   Readiness* owner_;
   Attempt* previous_;
   bool resources_ = false, pressure_ = false, committed_ = false;
  };

 private:
  struct State;
  friend struct Impl;
  CompileExecution& owner_;
  std::shared_ptr<State> state_;
 };
 [[nodiscard]] std::unique_ptr<CompileResourceWait> defer_resources();
 void require_feasible(CompileResources) const;
 [[nodiscard]] std::size_t descriptor_ceiling(CompileResources) const;
 [[nodiscard]] std::optional<CompileAllowance> try_reserve(CompileResources, const CompileAllowance& parent = {}, std::source_location = std::source_location::current());
 [[nodiscard]] CompileAllowance reserve(CompileResources, const CompileAllowance& parent = {}, std::source_location = std::source_location::current());
 [[nodiscard]] bool try_resize_workspace(const CompileAllowance&, std::uint64_t, bool retain_capacity = false);
 void with_unused_workspace(const CompileAllowance&, std::uint64_t live_bytes, const std::function<void()>&);
 void run(CompileTaskKind, CompileResources, const std::function<void(std::size_t)>&, CompileAllowance = {});
 void for_each(CompileTaskKind, std::size_t, CompileResources, const std::function<void(std::size_t)>&, const std::function<void(std::size_t)>& retire = {});
 void for_each(CompileTaskKind, std::size_t, const std::function<CompileResources(std::size_t)>&, const std::function<void(std::size_t)>&, const std::function<void(std::size_t)>& retire = {});
 void for_each(CompileTaskKind, std::size_t, const std::function<CompileResources(std::size_t)>&, const std::function<void(std::size_t)>&, Workspace&);
 void cooperate();
 // File/Curl CPU-bearing pumps replace a compute lane while active. A service's
 // Producer retains the waiting baseline; this short grant covers only a pump.
 class ServiceDemand;
 class ServiceGrant final {
 public:
  ServiceGrant() = default;
  ~ServiceGrant();
  ServiceGrant(ServiceGrant&&) noexcept;
  ServiceGrant& operator=(ServiceGrant&&) noexcept;
  ServiceGrant(const ServiceGrant&) = delete;
  ServiceGrant& operator=(const ServiceGrant&) = delete;
  [[nodiscard]] int cpu() const noexcept;
  [[nodiscard]] CompileWorkClass work_class() const noexcept { return class_; }

 private:
  friend class CompileExecution;
  friend class ServiceDemand;
  ServiceGrant(std::shared_ptr<Impl>, CompileWorkClass, std::size_t);
  void release() noexcept;
  std::shared_ptr<Impl> owner_;
  CompileWorkClass class_ = CompileWorkClass::Count;
  std::size_t lane_ = 0;
 };
 // A bounded set of alternative classes an available service dispatcher can
 // accept. Keep it armed across failed probes; update only when that actual
 // ability changes. It owns neither a producer nor physical-operation custody.
 class ServiceDemand final {
 public:
  using Classes = std::array<bool, static_cast<std::size_t>(CompileWorkClass::Count)>;
  explicit ServiceDemand(CompileExecution&);
  ~ServiceDemand();
  ServiceDemand(const ServiceDemand&) = delete;
  ServiceDemand& operator=(const ServiceDemand&) = delete;
  void update(const Classes&) noexcept;
  // Success clears this demand. Failure keeps it armed and publishes no change;
  // the caller waits on Readiness before probing again. Serialize owner access.
  [[nodiscard]] std::optional<ServiceGrant> try_grant(std::stop_token local_stop = {});

 private:
  friend class CompileExecution;
  void update_locked(const Classes&) noexcept;
  [[nodiscard]] std::optional<ServiceGrant> try_grant_locked(std::stop_token);
  std::shared_ptr<Impl> owner_;
  Classes pending_{};
 };
 // A local stop interrupts only this grant wait and never cancels the session.
 [[nodiscard]] ServiceGrant service_grant(CompileWorkClass, std::stop_token local_stop = {});

private:
 void retire_workspace(Workspace*) noexcept;
 [[nodiscard]] std::optional<Completion> submit_impl(Task, const std::shared_ptr<DependencyState>&, std::uint64_t);
 void for_each_impl(CompileTaskKind, std::size_t, const std::function<CompileResources(std::size_t)>&, const std::function<void(std::size_t)>&, Workspace*);
 std::shared_ptr<Impl> impl_;
};
class CompileResourceWait final {
public:
 ~CompileResourceWait();
 CompileResourceWait(const CompileResourceWait&) = delete;
 CompileResourceWait& operator=(const CompileResourceWait&) = delete;

private:
 friend class CompileExecution;
 explicit CompileResourceWait(std::shared_ptr<CompileAdmission>);
 std::shared_ptr<CompileAdmission> owner_;
};
}  // namespace mmltk::backend::data::detail
