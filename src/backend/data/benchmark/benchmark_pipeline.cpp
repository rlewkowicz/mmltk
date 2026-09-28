#include "src/backend/data/benchmark/detail/benchmark_image_input.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_curl.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_writer.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/math/checked_arithmetic.h"
#include <sys/resource.h>
#include <dirent.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <chrono>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <variant>
namespace mmltk::backend::data::benchmark_internal {
namespace {
constexpr std::size_t stage_count = static_cast<std::size_t>(BenchmarkStage::Count);
std::size_t descriptor_headroom() {
 rlimit limit{};
 if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) throw std::runtime_error("cannot inspect benchmark descriptor limit");
 std::size_t opened = 0;
 std::unique_ptr<DIR, decltype(&::closedir)> directory(::opendir("/proc/self/fd"), ::closedir);
 if (!directory) throw std::runtime_error("cannot inspect benchmark open descriptors");
 while (const auto* entry = ::readdir(directory.get()))
  if (entry->d_name[0] != '.') ++opened;
 // Runtime/library/stderr capacity remains outside admitted source handles.
 const auto available = limit.rlim_cur == RLIM_INFINITY ? std::size_t{4096} : static_cast<std::size_t>(std::min<rlim_t>(limit.rlim_cur, 4096));
 if (available <= opened + 16) throw std::runtime_error("insufficient descriptor headroom for benchmark compilation");
 return available - opened - 16;
}
}
BenchmarkResources BenchmarkResources::handles(std::size_t count, bool producer, std::size_t continuation) {
 return {mmltk::common::math::checked_multiply(std::uint64_t{8192}, static_cast<std::uint64_t>(count), "benchmark handle custody overflow"), count, producer, 0, true, continuation};
}
BenchmarkResources BenchmarkTransferEnvelope::demand(std::size_t count) const {
 using mmltk::common::math::checked_add;
 using mmltk::common::math::checked_multiply;
 return {checked_add(fixed.bytes, checked_multiply(per_transfer.bytes, static_cast<std::uint64_t>(count), "benchmark transfer workspace overflow"), "benchmark transfer workspace overflow"),
  checked_add(fixed.descriptors, checked_multiply(per_transfer.descriptors, count, "benchmark transfer descriptor overflow"), "benchmark transfer descriptor overflow"), true};
}
// Credits form only the actual dependent ownership chain. Children keep their
// producing commitment alive, never workers, sources, callbacks or writers.
struct BenchmarkWorkspaceOffer {
 std::uint64_t bytes = 0;
 std::size_t borrowers = 0;
};
struct BenchmarkAllowance::Credits {
 // Mutation occurs under Admission::mutex. Snapshot allocation precedes every
 // borrower-count change, so allocation failure leaves both custodians intact.
 // Aliases share this set; a storage partition clones it before moving bytes.
 class Loans final {
  std::vector<std::shared_ptr<BenchmarkWorkspaceOffer>> offers_;
 public:
  Loans() = default;
  Loans(const Loans&) = delete;
  Loans& operator=(const Loans&) = delete;
  [[nodiscard]] bool empty() const noexcept { return offers_.empty(); }
  void attach(const std::vector<std::shared_ptr<BenchmarkWorkspaceOffer>>& offers) {
   auto snapshot = offers;
   retire();
   offers_.swap(snapshot);
   for (const auto& offer : offers_) ++offer->borrowers;
  }
  void clone(const Loans& source) { attach(source.offers_); }
  void retire() noexcept {
   for (const auto& offer : offers_) --offer->borrowers;
   offers_.clear();
  }
 } workspace_loans;
 std::shared_ptr<BenchmarkCompilePipeline::Admission> owner;
 BenchmarkResources resources;
 std::shared_ptr<Credits> parent;
 std::size_t available = 0, borrowed = 0, cpu_users = 0;
 bool charged = false;
 bool descriptors_retired = false;
 bool workspace_retirement_pending = false;
 std::shared_ptr<BenchmarkWorkspaceOffer> workspace_offer;
 Credits(std::shared_ptr<BenchmarkCompilePipeline::Admission> value, BenchmarkResources demand, std::shared_ptr<Credits> producing)
  : owner(std::move(value)), resources(demand), parent(std::move(producing)), available(demand.continuation_descriptors) {}
 ~Credits();
};
struct BenchmarkCompilePipeline::Admission {
 std::mutex mutex;
 std::condition_variable changed;
 std::uint64_t generation = 0;
 std::function<void()> transport_wakeup;
 std::uint64_t target = 0, bytes = 0, handle_bytes = 0, offered_bytes = 0;
 // Only positive offers participate. Zero-capacity scopes still retain their
 // Credits sentinel, preventing reentry/resize without joining unrelated loans.
 std::vector<std::shared_ptr<BenchmarkWorkspaceOffer>> workspace_offers;
 std::size_t descriptor_capacity = 0, descriptors = 0, committed = 0;
 std::size_t cpu_capacity = 0, active = 0, external_cpus = 0, waiters = 0, resource_waiters = 0;
 [[nodiscard]] std::size_t descriptor_ceiling(BenchmarkResources value) const {
  return descriptor_capacity - (value.producer ? std::min<std::size_t>(8, descriptor_capacity / 4) : 0);
 }
 [[nodiscard]] bool feasible(BenchmarkResources value) const {
  const auto ceiling = descriptor_ceiling(value);
  return (!value.cpu_workers || value.cpu_workers < cpu_capacity) && value.continuation_descriptors <= ceiling && value.descriptors <= ceiling - value.continuation_descriptors;
 }
 void require(BenchmarkResources value, bool cpu_job = false) const {
  if (cpu_job && value.cpu_workers) throw InsufficientBenchmarkResources("benchmark CPU job cannot own an external CPU grant");
  if (value.cpu_workers && value.cpu_workers >= cpu_capacity) throw std::invalid_argument("external benchmark work must retain a CPU for consumers");
  if (!feasible(value)) throw InsufficientBenchmarkResources("benchmark operation exceeds resource admission");
 }
 [[nodiscard]] bool fits_bytes(BenchmarkResources value, const Credits* parent = nullptr) const {
  if (value.retained_handles || !value.bytes) return true;
  if (value.bytes <= target && bytes <= target - value.bytes) return true;
  // A legal oversized consumer may finish its own retained input. The complete
  // lineage is charged, and unrelated transient users must first retire.
  std::uint64_t lineage = 0;
  for (auto* credit = parent; credit; credit = credit->parent.get())
   if (!credit->resources.retained_handles) lineage += credit->resources.bytes;
  return bytes == lineage;
 }
 [[nodiscard]] std::size_t descriptor_room(BenchmarkResources value, const Credits* parent = nullptr) const {
  const auto ceiling = descriptor_ceiling(value);
  const auto used = descriptors + committed; // Always bounded by physical capacity.
  const auto free = used < ceiling ? ceiling - used : 0;
  // A consumer may already occupy producer headroom. Its physical charge must
  // not prevent a producer from using capacity that it committed earlier.
  return std::min(ceiling, free + (parent ? parent->available : 0));
 }
 [[nodiscard]] bool fits(BenchmarkResources value, const Credits* parent = nullptr) const {
  if (!feasible(value)) return false;
  if (value.cpu_workers && external_cpus + active > cpu_capacity - value.cpu_workers) return false;
  return value.descriptors + value.continuation_descriptors <= descriptor_room(value, parent) && fits_bytes(value, parent);
 }
 // Only a ready Pixels job with its input and output continuation already
 // admitted is a borrower. Arbitrary callbacks and header continuations never
 // use this path; the reader may resume without waiting on a new producer.
 [[nodiscard]] bool finite_pixel_fits(BenchmarkResources value, const Credits* parent) const {
  if (fits(value, parent)) return true;
  if (value.producer || value.cpu_workers || value.retained_handles || !value.bytes || !feasible(value) ||
      value.descriptors + value.continuation_descriptors > descriptor_room(value, parent) || value.bytes > target) return false;
  return offered_bytes <= bytes && bytes - offered_bytes <= target - value.bytes;
 }
 // Returned child promises pass through ancestors whose descriptor work has
 // explicitly ended. Only live ancestors can promise them to a new child.
 void return_promise(Credits* parent, std::size_t count) noexcept {
  while (parent && count) {
   if (!parent->descriptors_retired) { parent->available += count; return; }
   const auto upstream = std::min(count, parent->borrowed);
   parent->borrowed -= upstream;
   committed -= count - upstream;
   count = upstream;
   parent = parent->parent.get();
  }
 }
 static bool covers(const Credits& credit, BenchmarkResources demand) {
  const auto held = credit.resources;
  return demand.bytes <= held.bytes && demand.descriptors <= held.descriptors && demand.cpu_workers <= held.cpu_workers && (!demand.bytes || demand.retained_handles == held.retained_handles) &&
   (!demand.producer || held.producer) && demand.continuation_descriptors <= credit.available;
 }
 void charge(Credits& credit, bool borrow_workspace = false) {
  using mmltk::common::math::checked_add;
  const auto value = credit.resources;
  const auto claim = checked_add(value.descriptors, value.continuation_descriptors, "benchmark continuation admission overflow");
  const auto borrowed = credit.parent ? std::min(claim, credit.parent->available) : 0;
  auto& storage = value.retained_handles ? handle_bytes : bytes;
  const auto next_bytes = checked_add(storage, value.bytes, "benchmark byte admission overflow");
  const auto next_descriptors = checked_add(descriptors, value.descriptors, "benchmark descriptor admission overflow");
  const auto next_commitment = checked_add(committed - borrowed, value.continuation_descriptors, "benchmark continuation admission overflow");
  const auto next_cpus = checked_add(external_cpus, value.cpu_workers, "benchmark CPU admission overflow");
  // Every active positive offer participates conservatively. Allocate the
  // snapshot before committing any ledger state; nothing below can throw.
  if (borrow_workspace) credit.workspace_loans.attach(workspace_offers);
  storage = next_bytes;
  descriptors = next_descriptors;
  committed = next_commitment;
  external_cpus = next_cpus;
  credit.borrowed = borrowed;
  if (credit.parent) credit.parent->available -= borrowed;
  credit.charged = true;
 }
 // The physical owner requests retirement after freeing its backing. A CPU
 // frame or stable lending window may still use this credit's commitment.
 // Return detached ancestry to a caller that destroys it outside the mutex.
 [[nodiscard]] std::shared_ptr<Credits> retire_workspace(Credits& credit) noexcept {
  if (!credit.workspace_retirement_pending || credit.cpu_users || credit.workspace_offer) return {};
  credit.workspace_retirement_pending = false;
  auto& value = credit.resources;
  external_cpus -= std::exchange(value.cpu_workers, 0);
  (value.retained_handles ? handle_bytes : bytes) -= std::exchange(value.bytes, 0);
  credit.workspace_loans.retire();
  ++generation;
  if (transport_wakeup) transport_wakeup();
  // Live descriptor draws still need their producing promise chain. Storage
  // descendants retain their own charges and loans regardless of this parent.
  return credit.borrowed ? std::shared_ptr<Credits>{} : std::move(credit.parent);
 }
 void release(Credits& credit) noexcept {
  { const std::lock_guard lock(mutex);
   const auto value = credit.resources;
   credit.workspace_loans.retire();
   (value.retained_handles ? handle_bytes : bytes) -= value.bytes;
   descriptors -= value.descriptors;
   external_cpus -= value.cpu_workers;
   // Children hold this credit, so its own children have all returned before
   // destruction. Return the borrowed promise atomically with physical release.
   committed = committed - credit.available + credit.borrowed;
   return_promise(credit.parent.get(), credit.borrowed);
   ++generation;
   if (transport_wakeup) transport_wakeup();
  }
  changed.notify_all();
 }
 void change_waiters(bool add, bool resource) {
  if (add) { ++waiters; resource_waiters += resource; }
  else { --waiters; resource_waiters -= resource; }
  changed.notify_all();
  if (transport_wakeup) transport_wakeup();
 }
 struct Waiter {
  Admission& owner;
  bool resource;
  explicit Waiter(Admission& value, bool needs_resources = false) : owner(value), resource(needs_resources) { owner.change_waiters(true, resource); }
  ~Waiter() { owner.change_waiters(false, resource); }
  Waiter(const Waiter&) = delete;
  Waiter& operator=(const Waiter&) = delete;
 };
 class OfferScope final {
  std::shared_ptr<Credits> producer_;
  std::shared_ptr<BenchmarkWorkspaceOffer> offer_ = std::make_shared<BenchmarkWorkspaceOffer>();
 public:
  // Construct only while holding the producing admission's mutex. Publication
  // is the last throwing transition; this scope then owns withdrawal/reclaim.
  OfferScope(std::shared_ptr<Credits> producer, std::uint64_t live_bytes) : producer_(std::move(producer)) {
   auto& owner = *producer_->owner;
   const auto& resources = producer_->resources;
   if (resources.retained_handles || live_bytes > resources.bytes || !producer_->workspace_loans.empty() || producer_->workspace_retirement_pending)
    throw std::invalid_argument("benchmark workspace offer exceeds stable input custody");
   if (producer_->workspace_offer) throw std::invalid_argument("benchmark input already offers unused workspace");
   offer_->bytes = resources.bytes - live_bytes;
   const auto total = mmltk::common::math::checked_add(owner.offered_bytes, offer_->bytes, "benchmark workspace offer overflow");
   if (offer_->bytes) owner.workspace_offers.push_back(offer_);
   producer_->workspace_offer = offer_;
   owner.offered_bytes = total;
   ++owner.generation;
   owner.changed.notify_all();
  }
  ~OfferScope() noexcept {
   auto& owner = *producer_->owner;
   std::shared_ptr<Credits> retired_parent;
   std::unique_lock lock(owner.mutex);
   if (offer_->bytes) std::erase(owner.workspace_offers, offer_);
   owner.offered_bytes -= offer_->bytes;
   ++owner.generation;
   owner.changed.notify_all();
   if (offer_->borrowers) {
    // Withdrawal prevents new borrowers first. Actual idle/active storage must
    // retire even on callback failure/cancellation; this wait needs no polling
    // and never replaces the callback's original exception.
    Waiter reclaim(owner, true);
    owner.changed.wait(lock, [&] { return offer_->borrowers == 0; });
   }
   producer_->workspace_offer.reset();
   retired_parent = owner.retire_workspace(*producer_);
   owner.changed.notify_all();
  }
  OfferScope(const OfferScope&) = delete;
  OfferScope& operator=(const OfferScope&) = delete;
 };
};
BenchmarkAllowance::Credits::~Credits() { if (charged) owner->release(*this); }
struct BenchmarkCompilePipeline::Impl {
 bool consume(std::size_t lane, bool wait);
 struct Job;
 struct Slot;
 struct WorkGroup {
  BenchmarkCompilePipeline& pipeline;
  Impl& owner;
  Job* members = nullptr;
  Job* completed = nullptr;
  std::size_t outstanding = 0;
  bool joined = false;
  bool withdrawn = false;
  std::exception_ptr failure;
  const std::function<void(std::size_t)>* retire = nullptr;
  bool retain_scratch = false;
  explicit WorkGroup(BenchmarkCompilePipeline& value, const std::function<void(std::size_t)>* scratch = nullptr, bool retain = false) : pipeline(value), owner(*value.impl_), retire(scratch), retain_scratch(retain) {}
  ~WorkGroup();
  void attach(Job&);
  void complete(Job&);
  Job* take_completed();
  void join() noexcept;
  void finish();
  WorkGroup(const WorkGroup&) = delete;
  WorkGroup& operator=(const WorkGroup&) = delete;
 };
 struct Job {
  BenchmarkStage stage = BenchmarkStage::Metadata;
  BenchmarkResources resources{};
  BenchmarkAllowance allowance, parent;
  std::variant<const std::function<void(std::size_t)>*, Slot*> work{static_cast<const std::function<void(std::size_t)>*>(nullptr)};
  [[nodiscard]] Slot* pixel() const { const auto* value = std::get_if<Slot*>(&work); return value ? *value : nullptr; }
  [[nodiscard]] bool finite_borrower() const;
  Job* next = nullptr;
  Job* previous = nullptr;
  WorkGroup* group = nullptr;
  Job* group_next = nullptr;
  Job* group_previous = nullptr;
  Job* completed_next = nullptr;
  std::size_t index = 0;
  bool indexed = false;
  // Ready pixels have independent source admission. A writer's completion
  // group may join them, but cannot change their failure/cancellation policy.
  bool independent = false;
  const std::function<void(std::size_t)>* retire = nullptr;
  bool queued = false;
  bool done = false, finish_started = false;
  std::exception_ptr failure;
 };
 // All links and the fallback cursor borrow the same stable Job records.
 // Mutation and selection share the admission mutex, including withdrawal.
 class StageReadyQueue {
 public:
  [[nodiscard]] const Job* head() const { return head_; }
  void push(Job& job) {
   job.next = nullptr;
   job.previous = tail_;
   job.queued = true;
   if (tail_) tail_->next = &job;
   else head_ = &job;
   tail_ = &job;
  }
  void remove(Job& job) {
   if (fallback_ == &job) fallback_ = job.next ? job.next : head_;
   if (job.previous) job.previous->next = job.next; else head_ = job.next;
   if (job.next) job.next->previous = job.previous; else tail_ = job.previous;
   if (fallback_ == &job) fallback_ = nullptr;
   job.next = job.previous = nullptr;
   job.queued = false;
  }
  Job* pop() {
   auto* job = head_;
   if (job) remove(*job);
   return job;
  }
  template <class Eligible>
  Job* take(Eligible eligible) {
   if (!head_) return nullptr;
   if (eligible(*head_)) return pop();
   auto* first = fallback_ && fallback_ != head_ ? fallback_ : head_->next;
   if (!first) return nullptr;
   auto* candidate = first;
   do {
    // Resume beyond the last successful bypass. The head still gets first
    // refusal on every selection; a full unsuccessful lap does not notify.
    fallback_ = candidate->next ? candidate->next : head_->next;
    if (eligible(*candidate)) { remove(*candidate); return candidate; }
    candidate = fallback_;
   } while (candidate != first);
   return nullptr;
  }
 private:
  Job* head_ = nullptr;
  Job* tail_ = nullptr;
  Job* fallback_ = nullptr;
 };
 struct Source;
 struct Slot {
  Job job;
  Source* source = nullptr;
  BenchmarkSplitWriter* writer = nullptr;
  std::size_t index = 0;
  std::uint64_t image_id = 0;
  BenchmarkSourceGeneration generation = 0;
  bool submitted = false, retiring = false;
  BenchmarkSourcePublication publication;
  std::shared_ptr<BenchmarkPixelInput> input;
  std::shared_ptr<const BenchmarkEncodedImage> payload;
  BenchmarkAllowance pixel_allowance;
 };
 struct Source {
  std::filesystem::path root;
  BenchmarkSourceGeneration generation = 1, counter = 1;
  std::unordered_map<std::uint64_t, BenchmarkSourceGeneration> replacements;
  BenchmarkSourceGeneration image_generation(std::uint64_t id) const { const auto found = replacements.find(id); return found == replacements.end() ? generation : found->second; }
  bool retiring = false;
  std::size_t pending = 0;
  std::unordered_map<std::uint64_t, Slot> slots;
  std::vector<BenchmarkSplitWriter*> writers;
  struct Geometry { BenchmarkSourceGeneration generation; std::uint32_t width, height; std::shared_ptr<const BenchmarkEncodedImage> input; };
  std::unordered_map<std::uint64_t, Geometry> geometry;
 };
 // The sole mutable physical image owner. Scheduler jobs borrow stable slots;
 // source identity is bound once outside per-image production/registration.
 class ImageState {
 public:
  explicit ImageState(Impl& execution) : execution_(execution) {}
  Source& source(const std::filesystem::path&);
  [[nodiscard]] const Source* find(const std::filesystem::path&) const;
  void register_split(BenchmarkSplitWriter&, const PreparedBenchmarkSplit&, std::size_t);
  void admit(Slot&, bool independent);
  void publish(Source&, std::uint64_t, BenchmarkSourceGeneration, BenchmarkSourcePublication,
   std::optional<std::pair<std::uint32_t, std::uint32_t>>, bool, std::uint64_t attempt, std::shared_ptr<const BenchmarkEncodedImage>);
  void geometry_ready(Source&, std::uint64_t, BenchmarkSourceGeneration, std::pair<std::uint32_t, std::uint32_t>);
  [[nodiscard]] std::optional<BenchmarkImageGeometry> geometry(const std::filesystem::path&, std::uint64_t) const;
  void retire_image(const std::filesystem::path&, std::uint64_t);
  void retire_source(const std::filesystem::path&);
  [[nodiscard]] bool registered(BenchmarkSplitWriter& writer) const { return writers_.contains(&writer); }
  [[nodiscard]] Slot& writer_slot(BenchmarkSplitWriter& writer, std::size_t index) { return *writers_.at(&writer).at(index); }
  [[nodiscard]] std::uint64_t attempt() const { return attempt_; }
  void close() {
   ++attempt_;
   for (auto& [root, source] : sources_) { (void)root; source.slots.clear(); source.writers.clear(); }
   writers_.clear();
   admitted_ = false;
  }
 private:
  Impl& execution_;
  std::unordered_map<std::filesystem::path, Source> sources_;
  std::unordered_map<BenchmarkSplitWriter*, std::vector<Slot*>> writers_;
  std::uint64_t attempt_ = 1;
  bool admitted_ = false;
 };
 struct IdleScratch {
  BenchmarkSplitWriter* writer = nullptr;
  const std::function<void(std::size_t)>* retire = nullptr;
  BenchmarkAllowance allowance;
  WorkGroup* group = nullptr;
  const void* owner() const { return writer ? static_cast<const void*>(writer) : retire; }
  std::exception_ptr release(std::size_t lane) noexcept {
   std::exception_ptr failure;
   try { if (writer) writer->retire_scratch(lane); else if (retire) (*retire)(lane); } catch (...) { failure = std::current_exception(); }
   allowance = {};
   return failure;
  }
 };
 struct Frame;
 struct Lane {
  Frame* active = nullptr;
  IdleScratch idle;
  const void* retiring = nullptr;
  [[nodiscard]] bool busy() const { return active || retiring; }
  [[nodiscard]] bool owns(const void*) const;
 };
 struct Frame {
  Impl& owner;
  std::size_t lane;
  Frame* parent = nullptr;
  Frame* previous = nullptr;
  BenchmarkAllowance allowance;
  std::shared_ptr<Credits> cpu_credit;
  const void* scratch = nullptr;
  bool entered = false;
  static thread_local Frame* current;
  Frame(Impl& value, std::size_t index) : owner(value), lane(index) {}
  ~Frame() { leave(); }
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;
  // enter is protected by the scheduler mutex; each lane's stack has one CPU.
  void enter(BenchmarkAllowance value, const void* identity) {
   parent = owner.lanes[lane].active;
   previous = current;
   allowance = std::move(value);
   cpu_credit = allowance.credits_;
   if (cpu_credit) ++cpu_credit->cpu_users;
   scratch = identity;
   owner.lanes[lane].active = this;
   if (!parent) ++owner.admission->active;
   current = this;
   entered = true;
  }
  void leave() noexcept {
   if (!entered) return;
   auto retired = std::move(allowance);
   auto cpu = std::move(cpu_credit);
   std::shared_ptr<Credits> retired_parent;
   { const std::lock_guard lock(owner.mutex);
    if (cpu) { --cpu->cpu_users; retired_parent = owner.admission->retire_workspace(*cpu); }
    owner.lanes[lane].active = parent;
    if (!parent) { --owner.admission->active; ++owner.admission->generation; }
    current = previous;
    entered = false;
   }
   owner.changed.notify_all();
  }
 };
 std::vector<Lane> lanes;
 std::mutex curl_mutex;
 std::unique_ptr<BenchmarkCurl> curl;
 StorageReservationPool storage{".", {}};
 std::vector<int> cpus;
 std::shared_ptr<BenchmarkCompilePipeline::Admission> admission = std::make_shared<Admission>();
 std::mutex& mutex = admission->mutex;
 std::condition_variable& changed = admission->changed;
 std::array<StageReadyQueue, stage_count> ready;
 ImageState images{*this};
 std::size_t cursor = 0, metadata_streak = 0, pending = 0;
 bool membership_pending = true, stopping = false, shutdown = false;
 std::exception_ptr failure;
 std::exception_ptr retired_failure = std::make_exception_ptr(std::runtime_error("benchmark work retired before completion"));
 mmltk::common::concurrency::CancellationObservation cancellation;
 std::unique_ptr<mmltk::common::concurrency::WorkerPool> pool;
 template <class Predicate>
 void wait(std::unique_lock<std::mutex>& lock, Predicate ready) {
  // CancellationObservation deliberately borrows a poll-only source. Only a
  // blocked caller bridges that observation into a worker wakeup; idle CPU
  // workers and ordinary successful work have no polling loop.
  while (!ready()) {
   changed.wait_for(lock, std::chrono::milliseconds(100));
   if (cancellation.requested()) changed.notify_all();
  }
 }
 BenchmarkAllowance charge(BenchmarkResources resources, const BenchmarkAllowance& parent = {}, bool finite_pixel = false) {
  auto credits = std::make_shared<Credits>(admission, resources, parent.credits_);
  admission->charge(*credits, finite_pixel && !admission->fits(resources, parent.credits_.get()));
  return BenchmarkAllowance(std::move(credits));
 }
 void check_admission() const {
  if (failure) std::rethrow_exception(failure);
  throw_if_benchmark_cancelled(cancellation);
  if (stopping) throw std::logic_error("benchmark admission after shutdown");
 }
 void push(Job& job) {
  ready[static_cast<std::size_t>(job.stage)].push(job);
  ++admission->generation;
  if (admission->transport_wakeup) admission->transport_wakeup();
 }
 void remove(Job& job) {
  ready[static_cast<std::size_t>(job.stage)].remove(job);
 }
 Job* fail_group(WorkGroup*, std::exception_ptr, bool withdraw = true);
 void settle(Job&, std::exception_ptr = {}, bool recoverable = false) noexcept;
 void settle_list(Job*, std::exception_ptr) noexcept;
 void release_scratch(IdleScratch, std::size_t) noexcept;
 static const void* scratch_owner(const Job& job) {
  const auto* slot = job.pixel();
  // Header reading also uses the writer's decoder; it must respect a suspended
  // pixel frame even though it does not retain a pixel workspace allowance.
  return slot ? static_cast<const void*>(slot->writer) : job.retire;
 }
};
std::uint64_t BenchmarkAllowance::bytes() const noexcept {
 if (!credits_) return 0;
 const std::lock_guard lock(credits_->owner->mutex);
 return credits_->resources.bytes;
}
std::size_t BenchmarkAllowance::descriptors() const noexcept {
 if (!credits_) return 0;
 const std::lock_guard lock(credits_->owner->mutex);
 return credits_->resources.descriptors;
}
bool BenchmarkAllowance::try_resize_workspace(std::uint64_t bytes, bool retain_capacity) const {
 if (!credits_) return bytes == 0;
 auto& owner = *credits_->owner;
 const auto* frame = BenchmarkCompilePipeline::Impl::Frame::current;
 if (frame && frame->owner.admission.get() == &owner) throw std::logic_error("benchmark workspace resize inside a CPU lane");
 {
  const std::lock_guard lock(owner.mutex);
  auto& resources = credits_->resources;
  if (credits_->cpu_users) return false;
  if (resources.retained_handles || !credits_->workspace_loans.empty() || credits_->workspace_offer)
   throw std::logic_error("benchmark workspace resize requires settled own custody");
  if (bytes == resources.bytes || (retain_capacity && !owner.resource_waiters && bytes < resources.bytes)) return true;
  if (bytes > resources.bytes) {
   const auto growth = bytes - resources.bytes;
   if (!owner.fits_bytes({growth, 0}, credits_.get())) return false;
   owner.bytes = mmltk::common::math::checked_add(owner.bytes, growth, "benchmark workspace resize overflow");
  } else owner.bytes -= resources.bytes - bytes;
  resources.bytes = bytes;
  ++owner.generation;
  if (owner.transport_wakeup) owner.transport_wakeup();
 }
 owner.changed.notify_all();
 return true;
}
void BenchmarkAllowance::retire_workspace() const noexcept {
 if (!credits_) return;
 auto& owner = *credits_->owner;
 std::shared_ptr<Credits> parent;
 { const std::lock_guard lock(owner.mutex);
  credits_->workspace_retirement_pending = true;
  parent = owner.retire_workspace(*credits_);
 }
 owner.changed.notify_all();
}
void BenchmarkAllowance::retire_descriptors() const noexcept {
 if (!credits_) return;
 auto& owner = *credits_->owner;
 {
  const std::lock_guard lock(owner.mutex);
  const auto closed = std::exchange(credits_->resources.descriptors, 0);
  const auto unused = std::exchange(credits_->available, 0);
  credits_->descriptors_retired = true;
  credits_->resources.continuation_descriptors = 0;
  owner.descriptors -= closed;
  owner.committed -= unused;
  const auto returned = std::min(closed + unused, credits_->borrowed);
  credits_->borrowed -= returned;
  owner.committed += returned;
  owner.return_promise(credits_->parent.get(), returned);
  ++owner.generation;
  if (owner.transport_wakeup) owner.transport_wakeup();
 }
 owner.changed.notify_all();
}
BenchmarkAllowance BenchmarkAllowance::split_storage(std::uint64_t bytes) {
 if (!credits_) return {};
 auto& owner = *credits_->owner;
 // The consumer may finish while its oversized producing stream is live.
 // This lineage is accounting only: the stream explicitly retires its own
 // capacity/CPUs when their physical backing settles, regardless of aliases.
 auto storage = std::make_shared<Credits>(credits_->owner, BenchmarkResources{bytes, 0}, credits_);
 { const std::lock_guard lock(owner.mutex);
  if (credits_->resources.retained_handles || bytes > credits_->resources.bytes) throw std::invalid_argument("benchmark storage partition exceeds its admitted envelope");
  if (credits_->workspace_offer || credits_->workspace_retirement_pending) throw std::logic_error("benchmark storage partition requires a settled input window");
  if (bytes) storage->workspace_loans.clone(credits_->workspace_loans);
  credits_->resources.bytes -= bytes;
  if (!credits_->resources.bytes) credits_->workspace_loans.retire();
  storage->charged = true;
 }
 return BenchmarkAllowance(std::move(storage));
}
thread_local BenchmarkCompilePipeline::Impl::Frame* BenchmarkCompilePipeline::Impl::Frame::current = nullptr;
bool BenchmarkCompilePipeline::Impl::Job::finite_borrower() const {
 const auto* slot = pixel();
 // Native ready pixels own their input and a writer with admitted output.
 // The stage label alone cannot give arbitrary callbacks borrowing privilege.
 return slot && stage == BenchmarkStage::Pixels && slot->input && slot->writer &&
  !resources.producer && !resources.cpu_workers && !resources.retained_handles;
}
bool BenchmarkCompilePipeline::Impl::Lane::owns(const void* identity) const {
 if (!identity) return false;
 for (auto* frame = active; frame; frame = frame->parent) if (frame->scratch == identity) return true;
 return false;
}
void BenchmarkCompilePipeline::Impl::WorkGroup::attach(Job& job) {
 if (job.group) throw std::logic_error("benchmark job already belongs to a work group");
 job.group = this;
 job.group_previous = nullptr;
 job.group_next = members;
 if (members) members->group_previous = &job;
 members = &job;
 ++outstanding;
}
void BenchmarkCompilePipeline::Impl::WorkGroup::complete(Job& job) {
 if (job.group_previous) job.group_previous->group_next = job.group_next;
 else members = job.group_next;
 if (job.group_next) job.group_next->group_previous = job.group_previous;
 job.group = nullptr;
 job.group_next = job.group_previous = nullptr;
 job.completed_next = completed;
 completed = &job;
 --outstanding;
}
BenchmarkCompilePipeline::Impl::Job* BenchmarkCompilePipeline::Impl::WorkGroup::take_completed() {
 auto* result = completed;
 if (result) { completed = result->completed_next; result->completed_next = nullptr; }
 return result;
}
BenchmarkCompilePipeline::Impl::Job* BenchmarkCompilePipeline::Impl::fail_group(WorkGroup* group, std::exception_ptr error, bool withdraw) {
 if (!group) return nullptr;
 if (!group->failure) group->failure = error;
 // Local pixel repair reports a failed joined result while preserving every
 // unrelated admitted pixel. Only a borrowed-group failure/unwind withdraws it.
 if (!withdraw || group->withdrawn) return nullptr;
 group->withdrawn = true;
 Job* detached = nullptr;
 // A failure walks the bounded live membership once. Successful completion
 // unlinks one member; there is no all-members predicate on each wake.
 for (auto* member = group->members; member; member = member->group_next) {
  if (!member->queued || member->independent) continue;
  remove(*member);
  member->next = detached;
  detached = member;
 }
 return detached;
}
void BenchmarkCompilePipeline::Impl::settle(Job& job, std::exception_ptr error, bool recoverable) noexcept {
 Job* detached = nullptr;
 auto* slot = job.pixel();
 {
  const std::lock_guard lock(mutex);
  if (error) {
   detached = fail_group(job.group, error, !slot || !recoverable);
   if (slot && !recoverable && job.independent && !failure) { failure = error; ++admission->generation; }
  }
 }
 // Credits follow the actual input and source backing. A terminal notification
 // cannot let the borrower disappear while any of these releases is pending.
 if (slot) { slot->input.reset(); slot->payload.reset(); slot->pixel_allowance = {}; job.parent = {}; slot->publication = {}; }
 job.allowance = {};
 {
  const std::lock_guard lock(mutex);
  job.failure = error;
  job.done = true;
  --pending;
  if (slot) --slot->source->pending;
  if (job.group) job.group->complete(job);
 }
 changed.notify_all();
 settle_list(detached, error);
}
void BenchmarkCompilePipeline::Impl::settle_list(Job* jobs, std::exception_ptr error) noexcept {
 while (jobs) {
  auto* job = jobs;
  jobs = job->next;
  job->next = nullptr;
  // Retirement is not a new compile failure; the source/group initiating it
  // retains its own cause and an independently repairable image stays local.
  settle(*job, error, true);
 }
}
void BenchmarkCompilePipeline::Impl::release_scratch(IdleScratch scratch, std::size_t lane) noexcept {
 const auto error = scratch.release(lane);
 Job* detached = nullptr;
 {
  const std::lock_guard lock(mutex);
  if (error) detached = fail_group(scratch.group, error);
  lanes[lane].retiring = nullptr;
 }
 changed.notify_all();
 settle_list(detached, error);
}
BenchmarkCompilePipeline::Impl::WorkGroup::~WorkGroup() {
 if (joined) return;
 Job* detached;
 std::exception_ptr error;
 {
  const std::lock_guard lock(owner.mutex);
  detached = outstanding ? owner.fail_group(this, owner.retired_failure) : nullptr;
  error = failure;
 }
 owner.settle_list(detached, error);
 join();
}
void BenchmarkCompilePipeline::Impl::WorkGroup::join() noexcept {
 if (joined) return;
 {
  std::unique_lock lock(owner.mutex);
  owner.wait(lock, [&] { return outstanding == 0; });
 }
 if (retire && !retain_scratch) pipeline.retire_workspace(retire);
 else if (retire) {
  std::unique_lock lock(owner.mutex);
  // A pressure retirement already copied our group link. Join that release
  // before detaching the stable source identity from this completed group.
  owner.wait(lock, [&] { return std::ranges::none_of(owner.lanes, [&](const auto& lane) { return lane.retiring == retire; }); });
  for (auto& lane : owner.lanes) if (lane.idle.group == this) lane.idle.group = nullptr;
 }
 joined = true;
}
void BenchmarkCompilePipeline::Impl::WorkGroup::finish() {
 join();
 const std::lock_guard lock(owner.mutex);
 if (failure) std::rethrow_exception(failure);
 owner.check_admission();
}
bool BenchmarkCompilePipeline::Impl::consume(std::size_t lane, bool wait_for_work) {
 Job* job = nullptr;
 IdleScratch retired;
 std::size_t retired_lane = lane;
 Frame frame(*this, lane);
 {
  std::unique_lock lock(mutex);
  const auto take_idle = [&](std::size_t index) {
   auto& value = lanes[index];
   retired = std::move(value.idle);
   value.idle = {};
   retired_lane = index;
   value.retiring = retired.owner();
  };
  const auto select = [&] {
   if (shutdown) return true;
   auto& local = lanes[lane];
   if (local.retiring || (wait_for_work && local.active)) return false;
   const bool discard = stopping || failure || cancellation.requested();
   const bool pressure = admission->waiters || std::ranges::any_of(ready, [&](const auto& queue) {
    const auto* candidate = queue.head();
    if (!candidate || candidate->allowance || admission->fits_bytes(candidate->resources)) return false;
    const void* identity = scratch_owner(*candidate);
    return !identity || std::ranges::none_of(lanes, [&](const auto& value) {
     return !value.busy() && value.idle.owner() == identity && Admission::covers(*value.idle.allowance.credits_, candidate->resources);
    });
   });
   if (pressure)
    for (std::size_t i = 0; i < lanes.size(); ++i) {
     if (lanes[i].retiring || !lanes[i].idle.owner() || lanes[i].owns(lanes[i].idle.owner())) continue;
     take_idle(i);
     return true;
   }
   const auto eligible = [&](const Job& candidate) {
    const bool cancelled_group = !candidate.independent && candidate.group && candidate.group->withdrawn;
    const auto* slot = candidate.pixel();
    if (cancelled_group || (slot && (slot->retiring || slot->source->retiring)) || (discard && !candidate.finish_started)) return true;
    if (admission->active + admission->external_cpus >= cpus.size() + (local.active ? 1 : 0)) return false;
    const void* identity = scratch_owner(candidate);
    // A cooperative child must not mutate a decoder/parser that an outer frame
    // still borrows, or steal that frame's grant during pressure retirement.
    if (local.owns(identity)) return false;
    const bool reusable = candidate.stage != BenchmarkStage::Header && identity && local.idle.owner() == identity && Admission::covers(*local.idle.allowance.credits_, candidate.resources);
    return candidate.allowance || reusable || (candidate.finite_borrower()
     ? admission->finite_pixel_fits(candidate.resources, candidate.parent.credits_.get()) : admission->fits(candidate.resources, candidate.parent.credits_.get()));
   };
   const bool prefer_metadata = membership_pending && metadata_streak < 2;
   if (prefer_metadata) job = ready[0].take(eligible);
   if (job) {
    ++metadata_streak;
   } else {
    for (std::size_t count = 0; count < stage_count; ++count) {
     const auto stage = (cursor + count) % stage_count;
     if (stage == 0 && prefer_metadata) continue;
     job = ready[stage].take(eligible);
     if (!job) continue;
     cursor = (stage + 1) % stage_count;
     metadata_streak = 0;
     break;
    }
   }
   if (!job) {
    if (admission->waiters || std::ranges::any_of(ready, [](const auto& queue) { return queue.head() != nullptr; }))
     for (std::size_t i = 0; i < lanes.size(); ++i) {
      if (!lanes[i].retiring && lanes[i].idle.owner() && !lanes[i].owns(lanes[i].idle.owner())) { take_idle(i); return true; }
     }
    return false;
   }
   const void* identity = scratch_owner(*job);
   const auto* slot = job->pixel();
   const bool invoke = (!discard || job->finish_started) && !(!job->independent && job->group && job->group->withdrawn) && !(slot && (slot->retiring || slot->source->retiring));
   if (identity && local.idle.owner() && !local.owns(local.idle.owner())) {
    if (invoke && !job->allowance && job->stage != BenchmarkStage::Header && local.idle.owner() == identity && Admission::covers(*local.idle.allowance.credits_, job->resources)) {
     job->allowance = std::move(local.idle.allowance);
     local.idle = {};
    } else if (local.idle.owner() != identity || job->stage != BenchmarkStage::Header) take_idle(lane);
   }
   if (invoke && !job->allowance) {
    try { job->allowance = charge(job->resources, job->parent, job->finite_borrower()); } catch (...) { job->failure = std::current_exception(); }
   }
   frame.enter(std::move(job->allowance), identity);
   return true;
  };
  if (wait_for_work) changed.wait(lock, select);
  else if (!select()) return false;
 }
 if (retired.owner()) {
  release_scratch(std::move(retired), retired_lane);
  if (!job) return true;
 }
 if (!job) return false;
 std::exception_ptr error = job->failure;
 bool continue_pixels = false, recoverable = false;
 std::uint64_t pixel_bytes = 0;
 auto* slot = job->pixel();
 try {
  bool discarded;
  {
   const std::lock_guard lock(mutex);
   discarded = ((stopping || failure) && !job->finish_started) || (slot && (slot->source->retiring || slot->retiring)) || (!job->independent && job->group && job->group->withdrawn);
   if (discarded) error = job->group && job->group->failure ? job->group->failure : (failure ? failure : retired_failure);
  }
  if (!discarded && !error) {
   throw_if_benchmark_cancelled(cancellation);
   if (!slot) (*std::get<const std::function<void(std::size_t)>*>(job->work))(job->indexed ? job->index : lane);
   else if (job->stage == BenchmarkStage::Header) {
    slot->input = slot->writer->prepare_pixel(slot->index, lane, std::move(slot->publication), frame.allowance, std::move(slot->payload));
    if (slot->input) {
     const auto dimensions = slot->writer->header_dimensions(slot->index);
     { const std::lock_guard lock(mutex); images.geometry_ready(*slot->source, slot->image_id, slot->generation, *dimensions); }
     pixel_bytes = slot->writer->pixel_workspace_bytes(*slot->input);
     continue_pixels = true;
    }
   } else slot->writer->write_pixel(slot->index, lane, slot->input);
  } else if (discarded) recoverable = true;
 } catch (const BenchmarkImageReadError&) {
  error = std::current_exception();
  recoverable = slot != nullptr;
 } catch (...) { error = std::current_exception(); }
 // Pixel scratch owns its own charged capacity. Its input remains in the slot
 // until settlement, but idle scratch has no descriptor commitment to that input.
 if (slot && job->stage == BenchmarkStage::Pixels && frame.allowance) {
  std::shared_ptr<Credits> input;
  { const std::lock_guard lock(mutex);
   auto& credit = *frame.allowance.credits_;
   if (!credit.borrowed) input = std::move(credit.parent);
  }
 }
 if (((slot && job->stage == BenchmarkStage::Pixels) || job->retire) && frame.allowance) {
  IdleScratch scratch{slot ? slot->writer : nullptr, job->retire, std::move(frame.allowance), job->retire ? job->group : nullptr};
  IdleScratch displaced;
  {
   const std::lock_guard lock(mutex);
   displaced = std::move(lanes[lane].idle);
   lanes[lane].idle = std::move(scratch);
   if (displaced.owner()) lanes[lane].retiring = displaced.owner();
  }
  // Cooperative work can leave its own idle scratch while an outer frame is
  // suspended. Retiring it here cannot evict the active outer allocation.
  if (displaced.owner()) release_scratch(std::move(displaced), lane);
 }
 frame.leave();
 if (continue_pixels) {
  const std::lock_guard lock(mutex);
  job->finish_started = stopping || static_cast<bool>(failure);
  job->stage = BenchmarkStage::Pixels;
  job->resources = {pixel_bytes, 0};
  job->parent = slot->writer->pixel_input_allowance(*slot->input);
  job->allowance = std::move(slot->pixel_allowance);
  push(*job);
  changed.notify_all();
 } else settle(*job, error, recoverable);
 return true;
}
BenchmarkCompilePipeline::BenchmarkCompilePipeline(std::size_t workers, std::span<const int> cpus, BenchmarkExecutionLimits limits,
 mmltk::common::concurrency::CancellationObservation cancellation) : impl_(std::make_shared<Impl>()) {
 auto& state = *impl_;
 state.cpus = cpus.empty() ? mmltk::common::system::allowed_cpu_set() : std::vector<int>(cpus.begin(), cpus.end());
 workers = std::max<std::size_t>(1, workers);
 if (state.cpus.size() < workers) throw std::invalid_argument("benchmark worker budget exceeds assigned CPUs");
 state.cpus.resize(workers);
 state.lanes.resize(workers);
 state.admission->cpu_capacity = workers;
 state.admission->target = limits.transient_bytes ? limits.transient_bytes : std::min<std::uint64_t>(2ULL << 30, std::max<std::uint64_t>(256ULL << 20, workers * (64ULL << 20)));
 const auto headroom = descriptor_headroom();
 state.admission->descriptor_capacity = limits.descriptors ? std::min(limits.descriptors, headroom) : headroom;
 if (state.admission->descriptor_capacity < 2) throw std::invalid_argument("benchmark descriptor admission requires completion headroom");
 state.cancellation = cancellation;
 state.pool = std::make_unique<mmltk::common::concurrency::WorkerPool>(workers, state.cpus, "bench_cpu", workers);
 if (state.pool->size() != workers) throw std::invalid_argument("benchmark CPU assignment contains repeated CPUs");
 try {
  for (std::size_t lane = 0; lane < workers; ++lane) state.pool->enqueue_detached([this, lane] {
   auto& state = *impl_;
   while (state.consume(lane, true)) {}
  });
 } catch (...) {
  { const std::lock_guard lock(state.mutex); state.stopping = state.shutdown = true; }
  state.changed.notify_all();
  state.pool.reset();
  throw;
 }
}
BenchmarkCompilePipeline::~BenchmarkCompilePipeline() {
 impl_->curl.reset();
 retire_attempt();
 { const std::lock_guard lock(impl_->mutex); impl_->stopping = impl_->shutdown = true; }
 impl_->changed.notify_all();
 impl_->pool.reset();
}
std::size_t BenchmarkCompilePipeline::workers() const noexcept { return impl_->cpus.size(); }
std::size_t BenchmarkCompilePipeline::current_lane() const {
 const auto* frame = Impl::Frame::current;
 if (!frame || &frame->owner != impl_.get()) throw std::logic_error("benchmark lane requested outside CPU work");
 return frame->lane;
}
BenchmarkCurl& BenchmarkCompilePipeline::curl() {
 const std::lock_guard lock(impl_->curl_mutex);
 if (!impl_->curl) {
  impl_->curl = std::make_unique<BenchmarkCurl>(workers(), this);
  const std::lock_guard admission_lock(impl_->mutex);
  impl_->admission->transport_wakeup = impl_->curl->admission_wakeup();
 }
 return *impl_->curl;
}
StorageReservationPool& BenchmarkCompilePipeline::storage() noexcept { return impl_->storage; }
std::span<const int> BenchmarkCompilePipeline::cpus() const noexcept { return impl_->cpus; }
std::uint64_t BenchmarkCompilePipeline::transient_target() const noexcept { return impl_->admission->target; }
std::size_t BenchmarkCompilePipeline::descriptor_limit() const noexcept { return impl_->admission->descriptor_capacity; }
bool BenchmarkCompilePipeline::resource_pressure() const {
 const std::lock_guard lock(impl_->mutex);
 return impl_->admission->resource_waiters || std::ranges::any_of(impl_->ready, [&](const auto& queue) {
  const auto* job = queue.head();
  return job && !job->allowance && !impl_->admission->fits_bytes(job->resources);
 });
}
std::uint64_t BenchmarkCompilePipeline::admission_generation() const {
 const std::lock_guard lock(impl_->mutex);
 return impl_->admission->generation;
}
void BenchmarkCompilePipeline::wait_for_admission_change(std::uint64_t observed, std::chrono::steady_clock::time_point deadline) {
 const auto* frame = Impl::Frame::current;
 if (frame && &frame->owner == impl_.get()) throw std::logic_error("benchmark CPU lane cannot wait for source admission");
 std::unique_lock lock(impl_->mutex);
 Admission::Waiter waiter(*impl_->admission);
 impl_->wait(lock, [&] {
  impl_->check_admission();
  return impl_->admission->generation != observed || std::chrono::steady_clock::now() >= deadline;
 });
}
void BenchmarkCompilePipeline::notify_admission_change() noexcept {
 { const std::lock_guard lock(impl_->mutex); ++impl_->admission->generation; }
 impl_->changed.notify_all();
}
BenchmarkResourceWait::BenchmarkResourceWait(std::shared_ptr<BenchmarkCompilePipeline::Admission> owner) : owner_(std::move(owner)) {
 const std::lock_guard lock(owner_->mutex);
 owner_->change_waiters(true, true);
}
BenchmarkResourceWait::~BenchmarkResourceWait() {
 const std::lock_guard lock(owner_->mutex);
 owner_->change_waiters(false, true);
}
std::unique_ptr<BenchmarkResourceWait> BenchmarkCompilePipeline::defer_resources() {
 return std::unique_ptr<BenchmarkResourceWait>(new BenchmarkResourceWait(impl_->admission));
}
std::size_t BenchmarkCompilePipeline::descriptor_ceiling(BenchmarkResources resources) const {
 const std::lock_guard lock(impl_->mutex);
 return impl_->admission->descriptor_ceiling(resources);
}
void BenchmarkCompilePipeline::require_feasible(BenchmarkResources resources) const {
 const std::lock_guard lock(impl_->mutex);
 impl_->admission->require(resources);
}
std::optional<BenchmarkAllowance> BenchmarkCompilePipeline::try_reserve(BenchmarkResources resources, const BenchmarkAllowance& parent) {
 const std::lock_guard lock(impl_->mutex);
 impl_->check_admission();
 if (parent.credits_ && parent.credits_->owner != impl_->admission) throw std::invalid_argument("benchmark parent allowance belongs to another compile");
 if (!impl_->admission->fits(resources, parent.credits_.get())) return std::nullopt;
 return impl_->charge(resources, parent);
}
BenchmarkAllowance BenchmarkCompilePipeline::reserve(BenchmarkResources resources, const BenchmarkAllowance& parent) {
 const auto* frame = Impl::Frame::current;
 if (frame && &frame->owner == impl_.get()) throw std::logic_error("benchmark CPU lane cannot wait for resource credits");
 auto& ledger = *impl_->admission;
 if (parent.credits_ && parent.credits_->owner != impl_->admission) throw std::invalid_argument("benchmark parent allowance belongs to another compile");
 ledger.require(resources);
 std::unique_lock lock(impl_->mutex);
 for (;;) {
  impl_->check_admission();
  if (ledger.fits(resources, parent.credits_.get())) return impl_->charge(resources, parent);
  Admission::Waiter waiter(ledger, true);
  impl_->wait(lock, [&] { return impl_->stopping || impl_->failure || impl_->cancellation.requested() || ledger.fits(resources, parent.credits_.get()); });
 }
}
std::pair<std::size_t, BenchmarkAllowance> BenchmarkCompilePipeline::reserve_transfers(std::size_t requested, BenchmarkTransferEnvelope envelope, const BenchmarkAllowance& parent) {
 if (!requested || !envelope.per_transfer.descriptors) throw std::invalid_argument("benchmark transfer admission requires positive concurrency and descriptor demand");
 const auto* frame = Impl::Frame::current;
 if (frame && &frame->owner == impl_.get()) throw std::logic_error("benchmark CPU lane cannot wait for transfers");
 const auto demand = [&](std::size_t count) { return envelope.demand(count); };
 const auto fixed_descriptors = envelope.fixed.descriptors;
 const auto per_connection = envelope.per_transfer.bytes;
 const auto fixed_bytes = envelope.fixed.bytes;
 auto& ledger = *impl_->admission;
 if (parent.credits_ && parent.credits_->owner != impl_->admission) throw std::invalid_argument("benchmark parent allowance belongs to another compile");
 const auto minimum = demand(1);
 ledger.require(minimum);
 std::unique_lock lock(impl_->mutex);
 for (;;) {
  impl_->check_admission();
  if (ledger.fits(minimum, parent.credits_.get())) {
   auto count = std::min(requested, (ledger.descriptor_room(minimum, parent.credits_.get()) - fixed_descriptors) / envelope.per_transfer.descriptors);
   if (per_connection && fixed_bytes < ledger.target && ledger.bytes <= ledger.target - fixed_bytes)
    count = std::min(count, std::max<std::size_t>(1, (ledger.target - fixed_bytes - ledger.bytes) / per_connection));
   else count = 1;
   return {count, impl_->charge(demand(count), parent)};
  }
  Admission::Waiter waiter(ledger, true);
  impl_->wait(lock, [&] { return impl_->stopping || impl_->failure || impl_->cancellation.requested() || ledger.fits(minimum, parent.credits_.get()); });
 }
}
void BenchmarkCompilePipeline::run(BenchmarkStage stage, BenchmarkResources resources, const std::function<void(std::size_t)>& callback, BenchmarkAllowance allowance) {
 if (allowance.credits_ && allowance.credits_->owner != impl_->admission) throw std::invalid_argument("benchmark allowance belongs to another compile");
 { const std::lock_guard lock(impl_->mutex);
  if (allowance && !Admission::covers(*allowance.credits_, resources)) throw std::invalid_argument("benchmark job exceeds its transferred allowance");
 }
 auto* parent = Impl::Frame::current;
 if (parent && &parent->owner == impl_.get()) {
  { const std::lock_guard lock(impl_->mutex);
   if (resources.cpu_workers || (!allowance && !Admission::covers(*parent->allowance.credits_, resources))) throw std::logic_error("nested benchmark work exceeds its parent's allowance");
  }
  Impl::Frame frame(*impl_, parent->lane);
  { const std::lock_guard lock(impl_->mutex); frame.enter(allowance ? std::move(allowance) : parent->allowance, parent->scratch); }
  callback(frame.lane);
  return;
 }
 impl_->admission->require(resources, true);
 Impl::Job job;
 Impl::WorkGroup group(*this);
 job.stage = stage;
 job.resources = resources;
 job.work = &callback;
 job.allowance = std::move(allowance);
 {
  const std::lock_guard lock(impl_->mutex);
  impl_->check_admission();
  group.attach(job);
  ++impl_->pending;
  impl_->push(job);
 }
 impl_->changed.notify_all();
 group.finish();
}
void BenchmarkCompilePipeline::for_each(BenchmarkStage stage, std::size_t count, BenchmarkResources resources, const std::function<void(std::size_t)>& callback, const std::function<void(std::size_t)>& retire) {
 for_each(stage, count, [resources](std::size_t) { return resources; }, callback, retire);
}
void BenchmarkCompilePipeline::for_each(BenchmarkStage stage, std::size_t count, const std::function<BenchmarkResources(std::size_t)>& resources, const std::function<void(std::size_t)>& callback, const std::function<void(std::size_t)>& retire) {
 for_each_impl(stage, count, resources, callback, retire, false);
}
BenchmarkCompilePipeline::Workspace::Workspace(BenchmarkCompilePipeline& owner, std::function<void(std::size_t)> retire)
 : owner_(owner), retire_(std::move(retire)) {
 if (!retire_) throw std::invalid_argument("benchmark workspace requires retirement");
}
BenchmarkCompilePipeline::Workspace::~Workspace() { owner_.retire_workspace(&retire_); }
void BenchmarkCompilePipeline::for_each(BenchmarkStage stage, std::size_t count, const std::function<BenchmarkResources(std::size_t)>& resources, const std::function<void(std::size_t)>& callback, Workspace& workspace) {
 if (&workspace.owner_ != this) throw std::invalid_argument("benchmark workspace belongs to another compile");
 {
  const std::lock_guard lock(impl_->mutex);
  if (workspace.active_) throw std::logic_error("benchmark workspace is already in use");
  workspace.active_ = true;
 }
 try { for_each_impl(stage, count, resources, callback, workspace.retire_, true); }
 catch (...) { const std::lock_guard lock(impl_->mutex); workspace.active_ = false; throw; }
 const std::lock_guard lock(impl_->mutex);
 workspace.active_ = false;
}
void BenchmarkCompilePipeline::for_each_impl(BenchmarkStage stage, std::size_t count, const std::function<BenchmarkResources(std::size_t)>& resources, const std::function<void(std::size_t)>& callback, const std::function<void(std::size_t)>& retire, bool retain_scratch) {
 for (std::size_t index = 0; index < count; ++index) impl_->admission->require(resources(index), true);
 auto* parent = Impl::Frame::current;
 if (parent && &parent->owner == impl_.get()) {
  { const std::lock_guard lock(impl_->mutex);
   for (std::size_t index = 0; index < count; ++index)
    if (!Admission::covers(*parent->allowance.credits_, resources(index))) throw std::logic_error("nested benchmark chunks exceed their parent's allowance");
  }
  if (!count) return;
  Impl::Frame frame(*impl_, parent->lane);
  bool borrowed_scratch;
  Impl::IdleScratch previous_scratch;
  {
   const std::lock_guard lock(impl_->mutex);
   const auto& lane = impl_->lanes[parent->lane];
   if (retain_scratch && (lane.owns(&retire) || lane.retiring == &retire))
    throw std::logic_error("nested benchmark workspace is still borrowed or retiring");
   borrowed_scratch = retire && impl_->lanes[parent->lane].owns(&retire);
   // A nested scope borrows its parent's promise and must release its own
   // capacity before returning. Retire any earlier standalone lane custody
   // once, before the new callback can borrow that parser again.
   if (retain_scratch && lane.idle.owner() == &retire) {
    previous_scratch = std::move(impl_->lanes[parent->lane].idle);
    impl_->lanes[parent->lane].idle = {};
    impl_->lanes[parent->lane].retiring = &retire;
   }
   frame.enter(parent->allowance, retire ? static_cast<const void*>(&retire) : parent->scratch);
  }
  if (previous_scratch.owner()) impl_->release_scratch(std::move(previous_scratch), frame.lane);
  std::exception_ptr error;
  try { for (std::size_t index = 0; index < count; ++index) { throw_if_benchmark_cancelled(impl_->cancellation); callback(index); } }
  catch (...) { error = std::current_exception(); }
  if (retire && !borrowed_scratch) {
   try { retire(frame.lane); } catch (...) { if (!error) error = std::current_exception(); }
  }
  if (error) std::rethrow_exception(error);
  return;
 }
 if (!count) return;
 const auto capacity = std::min(count, workers() * 2);
 std::vector<Impl::Job> jobs(capacity);
 Impl::WorkGroup group(*this, retire ? &retire : nullptr, retain_scratch);
 // The free/completed chain contains only these stable records. A member's
 // original callback index travels in its record, without an indexed closure.
 for (auto& job : jobs) { job.completed_next = group.completed; group.completed = &job; }
 std::size_t next = 0;
 {
  std::unique_lock lock(impl_->mutex);
  while (next < count && !group.failure) {
   impl_->check_admission();
   while (next < count && group.completed) {
    auto& job = *group.take_completed();
    job.stage = stage;
    job.resources = resources(next);
    job.work = &callback;
    job.indexed = true;
    job.index = next++;
    job.retire = group.retire;
    job.done = false;
    job.failure = {};
    group.attach(job);
    ++impl_->pending;
    impl_->push(job);
   }
   impl_->changed.notify_all();
   if (next < count) impl_->wait(lock, [&] { return group.completed || group.failure || impl_->stopping || impl_->failure || impl_->cancellation.requested(); });
  }
 }
 group.finish();
}
void BenchmarkCompilePipeline::with_unused_workspace(const BenchmarkAllowance& allowance, std::uint64_t live_bytes, const std::function<void()>& callback) {
 const auto* frame = Impl::Frame::current;
 if (frame && &frame->owner == impl_.get()) throw std::logic_error("benchmark workspace lending cannot wait inside a CPU lane");
 if (!allowance.credits_ || allowance.credits_->owner != impl_->admission) throw std::invalid_argument("benchmark workspace offer requires owned credits");
 {
  const auto offer = [&] {
   const std::lock_guard lock(impl_->mutex);
   impl_->check_admission();
   return Admission::OfferScope(allowance.credits_, live_bytes);
  }();
  callback();
 }
 const std::lock_guard lock(impl_->mutex);
 impl_->check_admission();
}
void BenchmarkCompilePipeline::cooperate() {
 auto* frame = Impl::Frame::current;
 if (!frame || &frame->owner != impl_.get()) throw std::logic_error("benchmark cooperative yield requires a CPU lane");
 throw_if_benchmark_cancelled(impl_->cancellation);
 (void)impl_->consume(frame->lane, false);
 throw_if_benchmark_cancelled(impl_->cancellation);
}
void BenchmarkCompilePipeline::retire_workspace(const void* owner) noexcept {
 std::unique_lock lock(impl_->mutex);
 const auto settled = [&] {
  return std::ranges::none_of(impl_->lanes, [&](const auto& lane) { return lane.retiring == owner || lane.owns(owner); });
 };
 impl_->wait(lock, settled);
 for (std::size_t lane = 0; lane < impl_->lanes.size(); ++lane) {
  auto& state = impl_->lanes[lane];
  if (state.idle.owner() != owner) continue;
  impl_->wait(lock, [&] { return !state.retiring; });
  if (state.idle.owner() != owner) continue;
  auto retired = std::move(state.idle);
  state.idle = {};
  state.retiring = owner;
  lock.unlock();
  impl_->release_scratch(std::move(retired), lane);
  lock.lock();
 }
 impl_->wait(lock, settled);
}
void BenchmarkCompilePipeline::write_remaining(BenchmarkSplitWriter& writer, const PreparedBenchmarkSplit& split, std::span<const std::size_t> slots) {
 bool registered;
 { const std::lock_guard lock(impl_->mutex); registered = impl_->images.registered(writer); }
 if (!registered) register_split(writer, split);
 if (slots.empty()) return;
 Impl::WorkGroup group(*this);
 const auto capacity = std::min(slots.size(), workers() * 2);
 std::size_t next = 0;
 {
  std::unique_lock lock(impl_->mutex);
  while (next < slots.size() && !group.failure) {
   impl_->check_admission();
   // Completion removes a member and returns its capacity individually. Stable
   // canonical pixel slots already own their records; no duplicate job list.
   while (group.take_completed()) {}
   while (next < slots.size() && group.outstanding < capacity) {
    auto& slot = impl_->images.writer_slot(writer, slots[next++]);
    auto& job = slot.job;
    if (job.done && job.failure) std::rethrow_exception(job.failure);
    if (slot.submitted && !job.done) { group.attach(job); continue; }
    if (writer.image_complete(slot.index)) continue;
    group.attach(job);
    impl_->images.admit(slot, false);
   }
   impl_->changed.notify_all();
   if (next < slots.size()) impl_->wait(lock, [&] { return group.completed || group.failure || impl_->stopping || impl_->failure || impl_->cancellation.requested(); });
  }
 }
 group.finish();
}
void BenchmarkCompilePipeline::membership_ready() { const std::lock_guard lock(impl_->mutex); impl_->membership_pending = false; }
BenchmarkCompilePipeline::Impl::Source& BenchmarkCompilePipeline::Impl::ImageState::source(const std::filesystem::path& root) {
 auto [position, inserted] = sources_.try_emplace(root);
 if (inserted) position->second.root = root;
 return position->second;
}
const BenchmarkCompilePipeline::Impl::Source* BenchmarkCompilePipeline::Impl::ImageState::find(const std::filesystem::path& root) const {
 const auto found = sources_.find(root);
 return found == sources_.end() ? nullptr : &found->second;
}
void BenchmarkCompilePipeline::Impl::ImageState::register_split(BenchmarkSplitWriter& writer, const PreparedBenchmarkSplit& split, std::size_t workers) {
 const std::lock_guard lock(execution_.mutex);
 if (admitted_) throw std::logic_error("benchmark membership changed after readiness admission");
 writer.prepare_lanes(workers);
 auto& writer_slots = writers_[&writer];
 writer_slots.resize(split.images.size());
 std::vector<Source*> bound;
 bound.reserve(split.sources.size());
 for (const auto& value : split.sources) {
  auto& owner = source(value.root);
  bound.push_back(&owner);
  if (std::ranges::find(owner.writers, &writer) == owner.writers.end()) owner.writers.push_back(&writer);
 }
 for (std::size_t i = 0; i < split.images.size(); ++i) {
  const auto& image = split.images[i];
  auto& source = *bound.at(image.source_index);
  auto [position, inserted] = source.slots.try_emplace(image.source_image_id);
  if (!inserted) throw std::runtime_error("benchmark pixel membership repeats a physical image");
  auto& slot = position->second;
  slot.source = &source;
  slot.writer = &writer;
  slot.index = i;
  slot.image_id = image.source_image_id;
  slot.generation = source.image_generation(image.source_image_id);
  if (const auto found = source.geometry.find(image.source_image_id); found != source.geometry.end() && found->second.generation == slot.generation)
   slot.payload = found->second.input;
  slot.job.work = &slot;
  writer_slots[i] = &slot;
 }
}
void BenchmarkCompilePipeline::Impl::ImageState::admit(Slot& slot, bool independent) {
 slot.submitted = true;
 auto& job = slot.job;
 job.stage = BenchmarkStage::Header;
 job.resources = slot.payload && !slot.payload->encoded().empty() ? BenchmarkResources{} : BenchmarkResources::handles(1);
 job.parent = slot.payload ? slot.payload->allowance() : BenchmarkAllowance{};
 job.done = false;
 job.failure = {};
 job.finish_started = false;
 job.independent = independent;
 admitted_ = admitted_ || independent;
 ++execution_.pending;
 ++slot.source->pending;
 execution_.push(job);
}
void BenchmarkCompilePipeline::Impl::ImageState::retire_image(const std::filesystem::path& root, std::uint64_t id) {
 auto& execution = execution_;
 std::unique_lock lock(execution.mutex);
 auto& value = source(root);
 auto [replacement, inserted] = value.replacements.try_emplace(id, 0);
 (void)inserted;
 const auto generation = ++value.counter;
 replacement->second = generation;
 value.geometry.erase(id);
 const auto found = value.slots.find(id);
 if (found == value.slots.end()) return;
 auto& slot = found->second;
 slot.retiring = true;
 auto* detached = execution.fail_group(slot.job.group, execution.retired_failure, false);
 if (slot.job.queued) {
  execution.remove(slot.job);
  slot.job.next = detached;
  detached = &slot.job;
 }
 lock.unlock();
 execution.settle_list(detached, execution.retired_failure);
 lock.lock();
 execution.changed.notify_all();
 execution.wait(lock, [&] { return !slot.submitted || slot.job.done; });
 slot.submitted = false;
 slot.job.failure = {};
 slot.generation = generation;
 lock.unlock();
 slot.writer->invalidate_image(slot.index);
 lock.lock();
 slot.retiring = false;
 lock.unlock();
 execution.changed.notify_all();
}
void BenchmarkCompilePipeline::Impl::ImageState::geometry_ready(Source& source, std::uint64_t id, BenchmarkSourceGeneration generation, std::pair<std::uint32_t, std::uint32_t> dimensions) {
 if (execution_.stopping || source.retiring || source.image_generation(id) != generation) return;
 const auto slot = source.slots.find(id);
 if (slot != source.slots.end() && slot->second.retiring) return;
 if (!dimensions.first || !dimensions.second) throw std::invalid_argument("benchmark geometry is empty");
 auto [position, inserted] = source.geometry.emplace(id, Source::Geometry{generation, dimensions.first, dimensions.second, {}});
 if (!inserted && (position->second.width != dimensions.first || position->second.height != dimensions.second)) throw std::runtime_error("benchmark source generation has contradictory geometry");
 position->second.generation = generation;
}
std::optional<BenchmarkImageGeometry> BenchmarkCompilePipeline::Impl::ImageState::geometry(const std::filesystem::path& root, std::uint64_t id) const {
 const std::lock_guard lock(execution_.mutex);
 const auto* source = find(root);
 if (!source || source->retiring) return std::nullopt;
 const auto found = source->geometry.find(id);
 if (found == source->geometry.end() || found->second.generation != source->image_generation(id)) return std::nullopt;
 return BenchmarkImageGeometry{source->root, id, found->second.generation, found->second.width, found->second.height};
}
void BenchmarkCompilePipeline::Impl::ImageState::publish(Source& source, std::uint64_t id, BenchmarkSourceGeneration generation, BenchmarkSourcePublication publication,
 std::optional<std::pair<std::uint32_t, std::uint32_t>> dimensions, bool defer_pixels, std::uint64_t attempt, std::shared_ptr<const BenchmarkEncodedImage> payload) {
 auto& execution = execution_;
 Job* accepted = nullptr;
 {
  const std::lock_guard lock(execution.mutex);
  // Check captured attempt before consulting borrowed writers or reporting a
  // new attempt's failures to an old producer. Shutdown also closes tickets.
  if (attempt != attempt_ || execution.stopping) return;
  if (execution.failure) std::rethrow_exception(execution.failure);
  if (source.retiring || generation != source.image_generation(id)) return;
  if (dimensions) geometry_ready(source, id, generation, *dimensions);
  if (payload) {
   geometry_ready(source, id, generation, {payload->header().width, payload->header().height});
   source.geometry.at(id).input = payload->storage() == BenchmarkEncodedImage::Storage::HeaderOnly && !payload->allowance() ? payload : payload->header_only();
  }
  const auto found = source.slots.find(id);
  if (found == source.slots.end() || found->second.submitted || found->second.retiring) return;
  auto& slot = found->second;
  slot.generation = generation;
  slot.publication = std::move(publication);
  if (payload && payload->charged_bytes()) {
   // Admit input/work/output together before queueing retained bytes. Under
   // pressure, pooled input yields to its published file; a charged warm map
   // yields to its generation-bound header fact and the protected path read.
   const BenchmarkResources workspace{slot.writer->pixel_workspace_bytes(payload->header(), 0), 0};
   if (execution.admission->fits(workspace, payload->allowance().credits_.get()))
    slot.pixel_allowance = execution.charge(workspace, payload->allowance());
   else payload = payload->file_backing() ? payload->file_backing() : source.geometry.at(id).input;
  }
  slot.payload = payload ? std::move(payload) : source.geometry.contains(id) ? source.geometry.at(id).input : nullptr;
  admit(slot, true);
  accepted = &slot.job;
 }
 execution.changed.notify_all();
 const auto* frame = Frame::current;
 if (execution.cpus.size() == 1 && !defer_pixels && (!frame || &frame->owner != &execution)) {
  std::unique_lock lock(execution.mutex);
  execution.wait(lock, [&] { return attempt != attempt_ || accepted->done; });
  if (execution.failure) std::rethrow_exception(execution.failure);
 }
}
void BenchmarkCompilePipeline::Impl::ImageState::retire_source(const std::filesystem::path& root) {
 auto& execution = execution_;
 Job* retired = nullptr;
 std::unique_lock lock(execution.mutex);
 auto& source = this->source(root);
 source.retiring = true;
 source.generation = ++source.counter;
 source.replacements.clear();
 source.geometry.clear();
 // Every canonical source slot already owns its intrusive job handle. Visit
 // only affected work; unrelated stage queues and writer slots stay untouched.
 for (auto& [id, slot] : source.slots) {
  (void)id;
  if (!slot.job.queued) continue;
  execution.remove(slot.job);
  slot.job.next = retired;
  retired = &slot.job;
 }
 lock.unlock();
 execution.settle_list(retired, execution.retired_failure);
 lock.lock();
 execution.changed.notify_all();
 execution.wait(lock, [&] { return source.pending == 0; });
 for (auto& [id, slot] : source.slots) {
  (void)id;
  slot.submitted = false;
  slot.job.failure = {};
  slot.generation = source.generation;
 }
 lock.unlock();
 // The source flag remains closed across callbacks, which may throw. Completed
 // siblings in other sources are never invalidated by this owner.
 for (auto* writer : source.writers) writer->invalidate_source(root);
 lock.lock();
 source.retiring = false;
 lock.unlock();
 execution.changed.notify_all();
}
void BenchmarkCompilePipeline::register_split(BenchmarkSplitWriter& writer, const PreparedBenchmarkSplit& split) { impl_->images.register_split(writer, split, workers()); }
BenchmarkSourceGeneration BenchmarkCompilePipeline::source_generation(const std::filesystem::path& root) {
 const std::lock_guard lock(impl_->mutex);
 return impl_->images.source(root).generation;
}
BenchmarkSourceGeneration BenchmarkCompilePipeline::image_generation(const std::filesystem::path& root, std::uint64_t id) {
 const std::lock_guard lock(impl_->mutex);
 return impl_->images.source(root).image_generation(id);
}
void BenchmarkCompilePipeline::retire_image(const std::filesystem::path& root, std::uint64_t id) { impl_->images.retire_image(root, id); }
void BenchmarkCompilePipeline::retire_source(const std::filesystem::path& root) { impl_->images.retire_source(root); }
void BenchmarkCompilePipeline::geometry_ready(const BenchmarkImageGeometry& fact) {
 const std::lock_guard lock(impl_->mutex);
 impl_->images.geometry_ready(impl_->images.source(fact.root), fact.image_id, fact.generation, {fact.width, fact.height});
}
std::shared_ptr<const BenchmarkEncodedImage> BenchmarkCompilePipeline::image_input(const std::filesystem::path& root, std::uint64_t id) const {
 const std::lock_guard lock(impl_->mutex);
 const auto* source = impl_->images.find(root);
 if (!source || source->retiring) return {};
 const auto found = source->geometry.find(id);
 if (found == source->geometry.end() || found->second.generation != source->image_generation(id)) return {};
 return found->second.input;
}
std::optional<BenchmarkImageGeometry> BenchmarkCompilePipeline::geometry(const std::filesystem::path& root, std::uint64_t id) const { return impl_->images.geometry(root, id); }
struct BenchmarkSourcePublication::State {
 std::weak_ptr<BenchmarkCompilePipeline::Impl> execution;
 BenchmarkCompilePipeline::Impl::Source* source;
 std::shared_ptr<const ArtifactLease> custody;
 std::uint64_t attempt;
 BenchmarkSourceGeneration generation;
 std::optional<std::pair<std::uint64_t, BenchmarkSourceGeneration>> replacement;
 bool defer_pixels;
 [[nodiscard]] BenchmarkSourceGeneration image_generation(std::uint64_t id) const { return replacement && replacement->first == id ? replacement->second : generation; }
};
BenchmarkSourcePublication BenchmarkCompilePipeline::source_publication(const std::filesystem::path& root, std::shared_ptr<const ArtifactLease> custody, std::optional<std::uint64_t> repaired_image, bool defer_pixels) {
 const std::lock_guard lock(impl_->mutex);
 impl_->check_admission();
 auto& source = impl_->images.source(root);
 return BenchmarkSourcePublication(std::make_shared<BenchmarkSourcePublication::State>(impl_, &source, std::move(custody), impl_->images.attempt(), source.generation,
  repaired_image ? std::optional(std::pair{*repaired_image, source.image_generation(*repaired_image)}) : std::nullopt, defer_pixels));
}
void BenchmarkSourcePublication::operator()(const CachedImageReady& ready) const {
 if (!state_) return;
 if (auto execution = state_->execution.lock()) execution->images.publish(*state_->source, ready.image_id, state_->image_generation(ready.image_id), *this,
  ready.dimensions, ready.defer_pixels || state_->defer_pixels, state_->attempt, ready.payload);
}
bool BenchmarkSourcePublication::consume(const CachedImageReady& ready) const {
 if (!state_) return false;
 auto execution = state_->execution.lock();
 if (!execution) return false;
 {
  const std::lock_guard lock(execution->mutex);
  if (state_->attempt != execution->images.attempt()) throw std::runtime_error("benchmark repaired image attempt retired");
  if (!state_->source->slots.contains(ready.image_id)) return false;
 }
 execution->images.publish(*state_->source, ready.image_id, state_->image_generation(ready.image_id), *this, ready.dimensions, false, state_->attempt, ready.payload);
 std::unique_lock lock(execution->mutex);
 const auto retired = [&] {
  return execution->stopping || state_->attempt != execution->images.attempt() ||
   state_->source->retiring || state_->source->image_generation(ready.image_id) != state_->image_generation(ready.image_id);
 };
 execution->wait(lock, [&] { return retired() || state_->source->slots.at(ready.image_id).job.done; });
 if (retired()) throw std::runtime_error("benchmark repaired image generation retired");
 if (const auto failure = state_->source->slots.at(ready.image_id).job.failure) std::rethrow_exception(failure);
 return true;
}
void BenchmarkSourcePublication::geometry_ready(std::uint64_t id, std::pair<std::uint32_t, std::uint32_t> dimensions) const {
 if (!state_) return;
 if (auto execution = state_->execution.lock()) {
  const std::lock_guard lock(execution->mutex);
  if (state_->attempt != execution->images.attempt()) return;
  execution->images.geometry_ready(*state_->source, id, state_->image_generation(id), dimensions);
 }
}
void BenchmarkCompilePipeline::drain() {
 std::unique_lock lock(impl_->mutex);
 impl_->wait(lock, [&] { return impl_->pending == 0; });
 if (impl_->failure) std::rethrow_exception(impl_->failure);
}
void BenchmarkCompilePipeline::retire_attempt() noexcept {
 Impl::Job* discarded = nullptr;
 {
  const std::lock_guard lock(impl_->mutex);
  impl_->stopping = true;
  for (auto& queue : impl_->ready) {
   while (auto* job = queue.pop()) {
    job->next = discarded;
    discarded = job;
   }
  }
 }
 impl_->settle_list(discarded, impl_->retired_failure);
 impl_->changed.notify_all();
 {
  std::unique_lock lock(impl_->mutex);
  impl_->wait(lock, [&] { return impl_->pending == 0 && std::ranges::none_of(impl_->lanes, [](const auto& lane) { return lane.busy(); }); });
  for (std::size_t lane = 0; lane < impl_->lanes.size(); ++lane) {
   auto& state = impl_->lanes[lane];
   auto retired = std::move(state.idle);
   state.idle = {};
   state.retiring = retired.owner();
   lock.unlock();
   impl_->release_scratch(std::move(retired), lane);
   lock.lock();
  }
  impl_->wait(lock, [&] { return std::ranges::none_of(impl_->lanes, [](const auto& lane) { return lane.busy(); }); });
  impl_->images.close();
  impl_->membership_pending = true;
  impl_->metadata_streak = impl_->cursor = 0;
  impl_->failure = {};
  impl_->stopping = false;
 }
}
}  // namespace mmltk::backend::data::benchmark_internal
