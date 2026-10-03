#pragma once
#include "src/backend/data/detail/compile_resources.h"
#include "src/backend/data/detail/compile_workspace.h"
#include "src/common/math/checked_arithmetic.h"
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <source_location>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>
namespace mmltk::backend::data::detail {
// Credits form only the actual dependent ownership chain. Children keep their
// producing commitment alive, never workers, sources, callbacks or writers.
struct CompileWorkspaceOffer {
 std::uint64_t bytes = 0;
 std::size_t borrowers = 0;
 std::uint64_t lent = 0;
};
struct CompileAllowance::Credits {
 // Mutation occurs under CompileAdmission::mutex. Snapshot allocation precedes every
 // borrower-count change, so allocation failure leaves both custodians intact.
 // Aliases share this set; a storage partition clones it before moving bytes.
 class Loans final {
  struct Loan {
   std::shared_ptr<CompileWorkspaceOffer> offer;
   std::uint64_t bytes;
   Loan(std::shared_ptr<CompileWorkspaceOffer> value, std::uint64_t count) : offer(std::move(value)), bytes(count) {
    offer->lent += bytes;
    ++offer->borrowers;
   }
   ~Loan() {
    offer->lent -= bytes;
    --offer->borrowers;
   }
  };
  std::vector<std::shared_ptr<Loan>> loans_;

 public:
  Loans() = default;
  Loans(const Loans&) = delete;
  Loans& operator=(const Loans&) = delete;
  [[nodiscard]] bool empty() const noexcept { return loans_.empty(); }
  void attach(const std::vector<std::shared_ptr<CompileWorkspaceOffer>>& offers, std::uint64_t bytes) {
   std::vector<std::shared_ptr<Loan>> selected;
   selected.reserve(offers.size());
   for (const auto& offer : offers) {
    const auto draw = std::min(bytes, offer->bytes - offer->lent);
    if (!draw) continue;
    selected.push_back(std::make_shared<Loan>(offer, draw));
    bytes -= draw;
    if (!bytes) break;
   }
   if (bytes) throw std::logic_error("compile workspace loan exceeds participating offers");
   loans_.swap(selected);
  }
  // Split/aliased backing shares the quantified draw. The offer is reusable
  // only after the last physical descendant retires; it is never charged twice.
  void clone(const Loans& source) { loans_ = source.loans_; }
  void retire() noexcept { loans_.clear(); }
 } workspace_loans;
 struct ArenaPadding {
  std::shared_ptr<CompileAdmission> owner;
  std::uint64_t bytes = 0;
  bool retained_handles = false;
  ~ArenaPadding();
 };
 // Physical aliases share padding independently of descriptor/producer ancestry.
 // Declare it before arena so the final physical lease retires before its charge.
 std::shared_ptr<ArenaPadding> arena_padding;
 CompileArenaLease arena;
 std::shared_ptr<CompileAdmission> owner;
 CompileResources resources;
 [[nodiscard]] std::uint64_t charged_bytes() const noexcept {
  // An ancestry walk counts shared physical padding only at its oldest owner.
  if (arena_padding) {
   for (auto* ancestor = parent.get(); ancestor; ancestor = ancestor->parent.get())
    if (ancestor->arena_padding == arena_padding) return resources.bytes;
   return resources.bytes + arena_padding->bytes;
  }
  return resources.bytes;
 }
 std::shared_ptr<Credits> parent;
 std::size_t available = 0, borrowed = 0, cpu_users = 0;
 bool charged = false;
 bool descriptors_retired = false, continuations_released = false;
 bool workspace_retirement_pending = false;
 std::source_location location;
 std::shared_ptr<CompileWorkspaceOffer> workspace_offer;
 std::vector<std::size_t> cpu_lanes;
 std::vector<int> cpus;
 Credits(std::shared_ptr<CompileAdmission> value, CompileResources demand, std::shared_ptr<Credits> producing)
     : owner(std::move(value)), resources(demand), parent(std::move(producing)), available(demand.continuation_descriptors) {}
 ~Credits();
};
struct CompileAdmission {
 using Credits = CompileAllowance::Credits;
 using Change = CompileChange;
 struct Eligibility {
  bool fits = false;
  std::uint64_t loan_bytes = 0;
  CompileResourceDenial denial = CompileResourceDenial::None;
 };
 std::shared_ptr<CompileArenaBank> arenas;
 std::mutex mutex;
 std::condition_variable changed;
 std::uint64_t generation = 0, resource_generation = 0;
 // Called only under mutex; publication records readiness and signals the
 // existing event owner. User callbacks run later, outside this mutex.
 std::function<void(Change)> publish_change;
 std::function<void(std::span<const std::size_t>)> release_cpus;
 std::size_t protected_cpus = 0;
 std::uint64_t target = 0, bytes = 0, handle_bytes = 0, offered_bytes = 0, opaque_bytes = 0;
 // Only positive offers participate. Zero-capacity scopes still retain their
 // Credits sentinel, preventing reentry/resize without joining unrelated loans.
 std::vector<std::shared_ptr<CompileWorkspaceOffer>> workspace_offers;
 std::size_t descriptor_capacity = 0, descriptors = 0, committed = 0;
 std::size_t cpu_capacity = 0, active = 0, external_cpus = 0, waiters = 0, resource_waiters = 0, resource_jobs = 0;
 std::size_t withdrawals = 0;
 // Execution selects one ready oversized demand. Its constraint closes new
 // transient production while finite consumers finish their retained inputs.
 const Credits* draining_parent = nullptr;
 std::uint64_t draining_bytes = 0;
 [[nodiscard]] static bool exceptional_custody(const Credits* credit) {
  for (; credit; credit = credit->parent.get())
   if (credit->arena.exceptional()) return true;
  return false;
 }
 [[nodiscard]] bool oversized(CompileResources value) const {
  return value.supplied_workspace && value.bytes && arenas && !arenas->normal_fit(value.bytes, value.completion_workspace, value.workspace_class, value.workspace_window);
 }
 bool tracing = false, observe_credits = false;
 mutable std::uint64_t eligibility_probes = 0;
 std::uint64_t admission_lock_ns = 0;
 std::vector<Credits*> observed_credits;
 void notify(Change change) noexcept {
  if (change == Change::None) return;
  if (includes(change, Change::Resources)) ++resource_generation;
  // Pressure registration must not invalidate its own failed reserve attempt.
  if (change != Change::Pressure) ++generation;
  changed.notify_all();
  if (publish_change) publish_change(change);
 }
 [[nodiscard]] bool pressure() const noexcept { return resource_waiters || resource_jobs; }
 void change_pressure(std::size_t& owners, bool before, bool after) noexcept {
  if (before == after) return;
  const auto previous = pressure();
  if (after)
   ++owners;
  else
   --owners;
  if (previous != pressure()) notify(Change::Pressure);
 }
 [[nodiscard]] std::size_t descriptor_ceiling(CompileResources value) const {
  // An explicit dependent promise already reserves consumer headroom. Count
  // that capacity once; otherwise persistent output files can strand every
  // source before its first open even though its complete chain fits.
  const auto headroom = value.producer ? std::min<std::size_t>(8, descriptor_capacity / 4) : 0;
  return descriptor_capacity - (headroom - std::min(headroom, value.continuation_descriptors));
 }
 [[nodiscard]] bool feasible(CompileResources value) const {
  const auto ceiling = descriptor_ceiling(value);
  return (!value.cpu_workers || value.cpu_workers < cpu_capacity) && value.continuation_descriptors <= ceiling && value.descriptors <= ceiling - value.continuation_descriptors;
 }
 void require(CompileResources value, bool cpu_job = false) const {
  if (cpu_job && value.cpu_workers) throw InsufficientCompileResources("benchmark CPU job cannot own an external CPU grant");
  if (value.cpu_workers && value.cpu_workers >= cpu_capacity) throw std::invalid_argument("external benchmark work must retain a CPU for consumers");
  if (!feasible(value)) throw InsufficientCompileResources("benchmark operation exceeds resource admission");
 }
 [[nodiscard]] bool fits_bytes(CompileResources value, const Credits* parent = nullptr, std::uint64_t retiring_bytes = 0) const {
  if (value.retained_handles || !value.bytes) return true;
  const auto requested = reservation_bytes(value);
  const auto remaining = bytes - std::min(bytes, retiring_bytes);
  if (requested <= target && remaining <= target - requested) return true;
  // A legal oversized consumer may finish its own retained input. The complete
  // lineage is charged, and unrelated transient users must first retire.
  std::uint64_t lineage = 0;
  for (auto* credit = parent; credit; credit = credit->parent.get())
   if (!credit->resources.retained_handles) lineage += credit->charged_bytes();
  return remaining == lineage;
 }
 [[nodiscard]] std::uint64_t reservation_bytes(CompileResources value) const {
  return value.supplied_workspace && value.bytes && arenas ? arenas->reservation_bytes(value.bytes, value.completion_workspace, value.workspace_class, value.workspace_window) : value.bytes;
 }
 [[nodiscard]] std::size_t descriptor_room(CompileResources value, const Credits* parent = nullptr) const {
  const auto ceiling = descriptor_ceiling(value);
  const auto used = descriptors + committed;  // Always bounded by physical capacity.
  const auto free = used < ceiling ? ceiling - used : 0;
  // A consumer may already occupy producer headroom. Its physical charge must
  // not prevent a producer from using capacity that it committed earlier.
  return std::min(ceiling, free + (parent ? parent->available : 0));
 }
 // Only a ready pixel job with admitted input/output, or an owning label
 // projection with geometry, is a borrower. Arbitrary callbacks and headers never
 // use this path; the reader may resume without waiting on a new producer.
 // Prospective retirement changes only charged workspace bytes. Offers and
 // descriptor promises remain owned by their actual producers. Zero retirement
 // gives the exact participating draw used by charge under this same mutex.
 [[nodiscard]] Eligibility eligibility(
  CompileResources value, const Credits* parent = nullptr, bool finite_consumer = false, std::uint64_t retiring_bytes = 0, std::uint64_t retiring_arena_bytes = 0) const {
  if (tracing) ++eligibility_probes;
  if (!feasible(value)) return {.denial = CompileResourceDenial::Infeasible};
  if (draining_bytes && value.bytes && !value.retained_handles && !(value.supplied_workspace && value.bytes == draining_bytes && parent == draining_parent) && !(parent && finite_consumer))
   return {.denial = CompileResourceDenial::ExceptionalDrain};
  if (arenas && arenas->exceptional_active() && value.bytes && !exceptional_custody(parent)) return {.denial = CompileResourceDenial::ExceptionalLineage};
  if (value.supplied_workspace && value.bytes && arenas) {
   if (!arenas->fits(value.bytes, value.completion_workspace, value.workspace_class, value.workspace_window)) {
    // Shape fit ignores fragmentation and busy slices. Only shapes exceeding
    // all compatible regions have an exclusive exceptional route.
    const auto occupied = arenas->snapshot().leased;
    if (arenas->exceptional_active() || occupied > retiring_arena_bytes) return {.denial = CompileResourceDenial::ArenaBusy};
    if (!arenas->normal_fit(value.bytes, value.completion_workspace, value.workspace_class, value.workspace_window)) {
     std::uint64_t lineage = 0;
     for (auto* credit = parent; credit; credit = credit->parent.get())
      if (!credit->resources.retained_handles) lineage += credit->charged_bytes();
     if (bytes - std::min(bytes, retiring_bytes) != lineage) return {.denial = CompileResourceDenial::RetainedLineage};
    }
   }
  }
  if (value.cpu_workers && (value.cpu_workers > cpu_capacity - std::min(cpu_capacity, protected_cpus) || external_cpus + active > cpu_capacity - value.cpu_workers))
   return {.denial = CompileResourceDenial::Cpu};
  if (value.descriptors + value.continuation_descriptors > descriptor_room(value, parent)) return {.denial = CompileResourceDenial::Descriptors};
  if (fits_bytes(value, parent, retiring_bytes)) return {.fits = true};
  const auto requested = reservation_bytes(value);
  if (!finite_consumer || value.producer || value.cpu_workers || value.retained_handles || !requested || requested > target) return {.denial = CompileResourceDenial::Bytes};
  const auto remaining = bytes - std::min(bytes, retiring_bytes);
  if (offered_bytes > remaining) return {.denial = CompileResourceDenial::LoanCapacity};
  std::uint64_t loaned = 0;
  for (const auto& offer : workspace_offers) loaned += offer->lent;
  const auto uncovered = remaining - loaned;
  const auto room = target - requested;
  const auto needed = uncovered > room ? uncovered - room : 0;
  if (needed > offered_bytes - loaned) return {.denial = CompileResourceDenial::LoanCapacity};
  return {.fits = true, .loan_bytes = needed};
 }
 // Returned child promises pass through ancestors whose descriptor work has
 // explicitly ended. Only live ancestors can promise them to a new child.
 void return_promise(Credits* parent, std::size_t count) noexcept {
  while (parent && count) {
   if (!parent->descriptors_retired && !parent->continuations_released) {
    parent->available += count;
    return;
   }
   const auto upstream = std::min(count, parent->borrowed);
   parent->borrowed -= upstream;
   committed -= count - upstream;
   count = upstream;
   parent = parent->parent.get();
  }
 }
 void return_descriptors(Credits& credit, std::size_t closed, std::size_t unused) noexcept {
  descriptors -= closed;
  committed -= unused;
  const auto returned = std::min(closed + unused, credit.borrowed);
  credit.borrowed -= returned;
  committed += returned;
  return_promise(credit.parent.get(), returned);
  notify(Change::Resources);
 }
 static bool covers(const Credits& credit, CompileResources demand) {
  const auto held = credit.resources;
  return (!demand.supplied_workspace || held.supplied_workspace) && (!demand.bytes || demand.workspace_window == held.workspace_window) && demand.opaque_bytes <= held.opaque_bytes &&
         demand.bytes <= held.bytes && demand.descriptors <= held.descriptors && demand.cpu_workers <= held.cpu_workers && (!demand.bytes || demand.retained_handles == held.retained_handles) &&
         (!demand.producer || held.producer) && demand.continuation_descriptors <= credit.available;
 }
 void charge(Credits& credit, bool finite_consumer = false, std::source_location location = std::source_location::current()) {
  using mmltk::common::math::checked_add;
  const auto value = credit.resources;
  const auto admitted = eligibility(value, credit.parent.get(), finite_consumer);
  if (!admitted.fits) throw InsufficientCompileResources("compile operation no longer fits resource admission");
  const auto claim = checked_add(value.descriptors, value.continuation_descriptors, "benchmark continuation admission overflow");
  const auto borrowed = credit.parent ? std::min(claim, credit.parent->available) : 0;
  auto& storage = value.retained_handles ? handle_bytes : bytes;
  const auto reserved = reservation_bytes(value);
  const auto next_bytes = checked_add(storage, reserved, "benchmark byte admission overflow");
  const auto next_descriptors = checked_add(descriptors, value.descriptors, "benchmark descriptor admission overflow");
  const auto next_commitment = checked_add(committed - borrowed, value.continuation_descriptors, "benchmark continuation admission overflow");
  const auto next_cpus = checked_add(external_cpus, value.cpu_workers, "benchmark CPU admission overflow");
  const auto next_opaque = checked_add(opaque_bytes, value.opaque_bytes, "compile opaque library charge overflow");
  if (admitted.loan_bytes) credit.workspace_loans.attach(workspace_offers, admitted.loan_bytes);
  if (reserved != value.bytes) {
   credit.arena_padding = std::make_shared<Credits::ArenaPadding>();
   credit.arena_padding->owner = credit.owner;
   credit.arena_padding->retained_handles = value.retained_handles;
  }
  if (value.supplied_workspace && value.bytes && arenas)
   credit.arena = arenas->acquire(value.bytes, !arenas->normal_fit(value.bytes, value.completion_workspace, value.workspace_class, value.workspace_window), value.completion_workspace,
    value.workspace_class, value.workspace_window);
  if (credit.arena_padding) credit.arena_padding->bytes = reserved - value.bytes;
  storage = next_bytes;
  descriptors = next_descriptors;
  committed = next_commitment;
  external_cpus = next_cpus;
  opaque_bytes = next_opaque;
  credit.borrowed = borrowed;
  if (credit.parent) credit.parent->available -= borrowed;
  credit.charged = true;
  notify((value.bytes || value.descriptors || value.continuation_descriptors ? Change::Resources : Change::None) | (value.cpu_workers ? Change::Cpu : Change::None));
  if (observe_credits) try {
    observed_credits.push_back(&credit);
    credit.location = location;
   } catch (...) { /* Diagnostic tracking cannot change admission. */
   }
 }
 // The physical owner requests retirement after freeing its backing. A CPU
 // frame or stable lending window may still use this credit's commitment.
 // Return detached ancestry to a caller that destroys it outside the mutex.
 [[nodiscard]] std::shared_ptr<Credits> retire_workspace(Credits& credit) noexcept {
  if (!credit.workspace_retirement_pending || credit.cpu_users || credit.workspace_offer || credit.arena) return {};
  credit.workspace_retirement_pending = false;
  auto& value = credit.resources;
  const bool resources_changed = value.bytes || !credit.workspace_loans.empty() || (!credit.borrowed && credit.parent);
  const auto returned_cpus = std::exchange(value.cpu_workers, 0);
  external_cpus -= returned_cpus;
  if (!credit.cpu_lanes.empty() && release_cpus) release_cpus(credit.cpu_lanes);
  credit.cpu_lanes.clear();
  (value.retained_handles ? handle_bytes : bytes) -= std::exchange(value.bytes, 0);
  credit.workspace_loans.retire();
  notify((resources_changed ? Change::Resources : Change::None) | (returned_cpus ? Change::Cpu : Change::None));
  // Live descriptor draws still need their producing promise chain. Storage
  // descendants retain their own charges and loans regardless of this parent.
  return credit.borrowed ? std::shared_ptr<Credits>{} : std::move(credit.parent);
 }
 void release(Credits& credit) noexcept {
  {
   const std::lock_guard lock(mutex);
   const auto value = credit.resources;
   if (observe_credits) std::erase(observed_credits, &credit);
   credit.workspace_loans.retire();
   (value.retained_handles ? handle_bytes : bytes) -= value.bytes;
   descriptors -= value.descriptors;
   external_cpus -= value.cpu_workers;
   opaque_bytes -= value.opaque_bytes;
   if (!credit.cpu_lanes.empty() && release_cpus) release_cpus(credit.cpu_lanes);
   // Children hold this credit, so its own children have all returned before
   // destruction. Return the borrowed promise atomically with physical release.
   committed = committed - credit.available + credit.borrowed;
   return_promise(credit.parent.get(), credit.borrowed);
   notify((value.bytes || value.descriptors || credit.available || credit.borrowed ? Change::Resources : Change::None) | (value.cpu_workers ? Change::Cpu : Change::None));
  }
  changed.notify_all();
 }
 void change_waiters(bool add, bool resource) {
  const auto previous = pressure();
  if (add)
   ++waiters;
  else
   --waiters;
  if (resource) change_pressure(resource_waiters, !add, add);
  changed.notify_all();
  if (previous == pressure() && publish_change) publish_change(Change::Cpu);
 }
 struct Waiter {
  CompileAdmission& owner;
  bool resource;
  explicit Waiter(CompileAdmission& value, bool needs_resources = false) : owner(value), resource(needs_resources) { owner.change_waiters(true, resource); }
  ~Waiter() { owner.change_waiters(false, resource); }
  Waiter(const Waiter&) = delete;
  Waiter& operator=(const Waiter&) = delete;
 };
 class OfferScope final {
  std::shared_ptr<Credits> producer_;
  std::shared_ptr<CompileWorkspaceOffer> offer_ = std::make_shared<CompileWorkspaceOffer>();

 public:
  // Construct only while holding the producing admission's mutex. Publication
  // is the last throwing transition; this scope then owns withdrawal/reclaim.
  OfferScope(std::shared_ptr<Credits> producer, std::uint64_t live_bytes) : producer_(std::move(producer)) {
   auto& owner = *producer_->owner;
   const auto& resources = producer_->resources;
   if (producer_->arena || resources.retained_handles || live_bytes > resources.bytes || !producer_->workspace_loans.empty() || producer_->workspace_retirement_pending)
    throw std::invalid_argument("benchmark workspace offer exceeds stable input custody");
   if (producer_->workspace_offer) throw std::invalid_argument("benchmark input already offers unused workspace");
   offer_->bytes = resources.bytes - live_bytes;
   const auto total = mmltk::common::math::checked_add(owner.offered_bytes, offer_->bytes, "benchmark workspace offer overflow");
   if (offer_->bytes) owner.workspace_offers.push_back(offer_);
   producer_->workspace_offer = offer_;
   owner.offered_bytes = total;
   owner.notify(Change::Resources);
  }
  ~OfferScope() noexcept {
   auto& owner = *producer_->owner;
   std::shared_ptr<Credits> retired_parent;
   std::unique_lock lock(owner.mutex);
   if (offer_->bytes) std::erase(owner.workspace_offers, offer_);
   owner.offered_bytes -= offer_->bytes;
   owner.notify(Change::Resources);
   if (offer_->borrowers) {
    // Withdrawal prevents new borrowers first. Actual idle/active storage must
    // retire even on callback failure/cancellation; this wait needs no polling
    // and never replaces the callback's original exception.
    Waiter reclaim(owner, true);
    ++owner.withdrawals;
    owner.changed.wait(lock, [&] { return offer_->borrowers == 0; });
    --owner.withdrawals;
   }
   producer_->workspace_offer.reset();
   retired_parent = owner.retire_workspace(*producer_);
   owner.notify(Change::Resources);
  }
  OfferScope(const OfferScope&) = delete;
  OfferScope& operator=(const OfferScope&) = delete;
 };
};
}  // namespace mmltk::backend::data::detail
