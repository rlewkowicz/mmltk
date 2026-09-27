#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
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
struct BenchmarkAllowance::Credits {
 std::shared_ptr<BenchmarkCompilePipeline::Admission> owner;
 BenchmarkResources resources;
 std::shared_ptr<Credits> parent;
 std::size_t available = 0, borrowed = 0;
 bool charged = false;
 Credits(std::shared_ptr<BenchmarkCompilePipeline::Admission> value, BenchmarkResources demand, std::shared_ptr<Credits> producing)
  : owner(std::move(value)), resources(demand), parent(std::move(producing)), available(demand.continuation_descriptors) {}
 ~Credits();
};
struct BenchmarkCompilePipeline::Admission {
 std::mutex mutex;
 std::condition_variable changed;
 std::uint64_t generation = 0;
 std::uint64_t target = 0, bytes = 0, handle_bytes = 0;
 std::size_t descriptor_capacity = 0, descriptors = 0, committed = 0;
 std::size_t cpu_capacity = 0, active = 0, external_cpus = 0, waiters = 0;
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
 [[nodiscard]] bool fits_bytes(BenchmarkResources value) const {
  return value.retained_handles || !value.bytes || (value.bytes > target ? bytes == 0 : bytes <= target - value.bytes);
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
  return value.descriptors + value.continuation_descriptors <= descriptor_room(value, parent) && fits_bytes(value);
 }
 static bool covers(const Credits& credit, BenchmarkResources demand) {
  const auto held = credit.resources;
  return demand.bytes <= held.bytes && demand.descriptors <= held.descriptors && demand.cpu_workers <= held.cpu_workers && (!demand.bytes || demand.retained_handles == held.retained_handles) &&
   (!demand.producer || held.producer) && demand.continuation_descriptors <= credit.available;
 }
 void charge(Credits& credit) {
  using mmltk::common::math::checked_add;
  const auto value = credit.resources;
  const auto claim = checked_add(value.descriptors, value.continuation_descriptors, "benchmark continuation admission overflow");
  const auto borrowed = credit.parent ? std::min(claim, credit.parent->available) : 0;
  auto& storage = value.retained_handles ? handle_bytes : bytes;
  const auto next_bytes = checked_add(storage, value.bytes, "benchmark byte admission overflow");
  const auto next_descriptors = checked_add(descriptors, value.descriptors, "benchmark descriptor admission overflow");
  const auto next_commitment = checked_add(committed - borrowed, value.continuation_descriptors, "benchmark continuation admission overflow");
  const auto next_cpus = checked_add(external_cpus, value.cpu_workers, "benchmark CPU admission overflow");
  storage = next_bytes;
  descriptors = next_descriptors;
  committed = next_commitment;
  external_cpus = next_cpus;
  credit.borrowed = borrowed;
  if (credit.parent) credit.parent->available -= borrowed;
  credit.charged = true;
 }
 void release(const Credits& credit) noexcept {
  { const std::lock_guard lock(mutex);
   const auto value = credit.resources;
   (value.retained_handles ? handle_bytes : bytes) -= value.bytes;
   descriptors -= value.descriptors;
   external_cpus -= value.cpu_workers;
   // Children hold this credit, so its own children have all returned before
   // destruction. Return the borrowed promise atomically with physical release.
   committed = committed - credit.available + credit.borrowed;
   if (credit.parent) credit.parent->available += credit.borrowed;
   ++generation;
  }
  changed.notify_all();
 }
 struct Waiter {
  Admission& owner;
  explicit Waiter(Admission& value) : owner(value) { ++owner.waiters; owner.changed.notify_all(); }
  ~Waiter() { --owner.waiters; owner.changed.notify_all(); }
  Waiter(const Waiter&) = delete;
  Waiter& operator=(const Waiter&) = delete;
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
  explicit WorkGroup(BenchmarkCompilePipeline& value, const std::function<void(std::size_t)>* scratch = nullptr) : pipeline(value), owner(*value.impl_), retire(scratch) {}
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
  BenchmarkAllowance allowance;
  std::variant<const std::function<void(std::size_t)>*, Slot*> work{static_cast<const std::function<void(std::size_t)>*>(nullptr)};
  [[nodiscard]] Slot* pixel() const { const auto* value = std::get_if<Slot*>(&work); return value ? *value : nullptr; }
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
  struct Geometry { BenchmarkSourceGeneration generation; std::uint32_t width, height; };
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
   std::optional<std::pair<std::uint32_t, std::uint32_t>>, bool, std::uint64_t attempt);
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
   scratch = identity;
   owner.lanes[lane].active = this;
   if (!parent) ++owner.admission->active;
   current = this;
   entered = true;
  }
  void leave() noexcept {
   if (!entered) return;
   allowance = {};
   { const std::lock_guard lock(owner.mutex);
    owner.lanes[lane].active = parent;
    if (!parent) { --owner.admission->active; ++owner.admission->generation; }
    current = previous;
    entered = false;
   }
   owner.changed.notify_all();
  }
 };
 std::vector<Lane> lanes;
 StorageReservationPool storage{".", {}};
 std::vector<int> cpus;
 std::shared_ptr<BenchmarkCompilePipeline::Admission> admission = std::make_shared<Admission>();
 std::mutex& mutex = admission->mutex;
 std::condition_variable& changed = admission->changed;
 std::array<Job*, stage_count> heads{}, tails{};
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
 BenchmarkAllowance charge(BenchmarkResources resources, const BenchmarkAllowance& parent = {}) {
  auto credits = std::make_shared<Credits>(admission, resources, parent.credits_);
  admission->charge(*credits);
  return BenchmarkAllowance(std::move(credits));
 }
 void check_admission() const {
  if (failure) std::rethrow_exception(failure);
  throw_if_benchmark_cancelled(cancellation);
  if (stopping) throw std::logic_error("benchmark admission after shutdown");
 }
 void push(Job& job) {
  const auto stage = static_cast<std::size_t>(job.stage);
  job.next = nullptr;
  job.previous = tails[stage];
  job.queued = true;
  if (tails[stage]) tails[stage]->next = &job;
  else heads[stage] = &job;
  tails[stage] = &job;
 }
 void remove(Job& job) {
  const auto stage = static_cast<std::size_t>(job.stage);
  if (job.previous) job.previous->next = job.next; else heads[stage] = job.next;
  if (job.next) job.next->previous = job.previous; else tails[stage] = job.previous;
  job.next = job.previous = nullptr;
  job.queued = false;
 }
 Job* pop(std::size_t stage) { auto* job = heads[stage]; remove(*job); return job; }
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
std::uint64_t BenchmarkAllowance::bytes() const noexcept { return credits_ ? credits_->resources.bytes : 0; }
std::size_t BenchmarkAllowance::descriptors() const noexcept { return credits_ ? credits_->resources.descriptors : 0; }
thread_local BenchmarkCompilePipeline::Impl::Frame* BenchmarkCompilePipeline::Impl::Frame::current = nullptr;
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
 // A failure walks the bounded live membership once. Successful completion and
 // dispatch are O(1); there is no all-members predicate on each wake.
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
 if (slot) { slot->input.reset(); slot->publication = {}; }
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
 if (retire) pipeline.retire_workspace(retire);
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
   const bool pressure = admission->waiters || std::ranges::any_of(heads, [&](auto* candidate) {
    if (!candidate || candidate->allowance || admission->fits_bytes(candidate->resources)) return false;
    const void* identity = scratch_owner(*candidate);
    return !identity || std::ranges::none_of(lanes, [&](const auto& value) {
     return !value.busy() && value.idle.owner() == identity && Admission::covers(*value.idle.allowance.credits_, candidate->resources);
    });
   });
   if (pressure)
    for (std::size_t i = 0; i < lanes.size(); ++i) {
     if (lanes[i].busy() || !lanes[i].idle.owner()) continue;
     take_idle(i);
     return true;
   }
   const auto eligible = [&](std::size_t stage) {
    const auto* candidate = heads[stage];
    if (!candidate) return false;
    const bool cancelled_group = !candidate->independent && candidate->group && candidate->group->withdrawn;
    const auto* slot = candidate->pixel();
    if (cancelled_group || (slot && (slot->retiring || slot->source->retiring)) || (discard && !candidate->finish_started)) return true;
    if (admission->active + admission->external_cpus >= cpus.size() + (local.active ? 1 : 0)) return false;
    const void* identity = scratch_owner(*candidate);
    // A cooperative child must not mutate a decoder/parser that an outer frame
    // still borrows, or steal that frame's grant during pressure retirement.
    if (local.owns(identity)) return false;
    const bool reusable = candidate->stage != BenchmarkStage::Header && identity && local.idle.owner() == identity && Admission::covers(*local.idle.allowance.credits_, candidate->resources);
    return candidate->allowance || reusable || admission->fits(candidate->resources);
   };
   if (membership_pending && metadata_streak < 2 && eligible(0)) {
    job = pop(0);
    ++metadata_streak;
   } else {
    for (std::size_t count = 0; count < stage_count; ++count) {
     const auto stage = (cursor + count) % stage_count;
     if (!eligible(stage)) continue;
     job = pop(stage);
     cursor = (stage + 1) % stage_count;
     metadata_streak = 0;
     break;
    }
   }
   if (!job) {
    if (admission->waiters || std::ranges::any_of(heads, [](auto* head) { return head != nullptr; }))
     for (std::size_t i = 0; i < lanes.size(); ++i) {
      if (!lanes[i].busy() && lanes[i].idle.owner()) { take_idle(i); return true; }
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
    try { job->allowance = charge(job->resources); } catch (...) { job->failure = std::current_exception(); }
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
    slot->input = slot->writer->prepare_pixel(slot->index, lane, std::move(slot->publication), frame.allowance);
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
StorageReservationPool& BenchmarkCompilePipeline::storage() noexcept { return impl_->storage; }
std::span<const int> BenchmarkCompilePipeline::cpus() const noexcept { return impl_->cpus; }
std::uint64_t BenchmarkCompilePipeline::transient_target() const noexcept { return impl_->admission->target; }
std::size_t BenchmarkCompilePipeline::descriptor_limit() const noexcept { return impl_->admission->descriptor_capacity; }
std::uint64_t BenchmarkCompilePipeline::admission_generation() const {
 const std::lock_guard lock(impl_->mutex);
 return impl_->admission->generation;
}
void BenchmarkCompilePipeline::wait_for_admission_change(std::uint64_t observed) {
 const auto* frame = Impl::Frame::current;
 if (frame && &frame->owner == impl_.get()) throw std::logic_error("benchmark CPU lane cannot wait for source admission");
 std::unique_lock lock(impl_->mutex);
 Admission::Waiter waiter(*impl_->admission);
 impl_->wait(lock, [&] {
  impl_->check_admission();
  return impl_->admission->generation != observed;
 });
}
void BenchmarkCompilePipeline::notify_admission_change() noexcept {
 { const std::lock_guard lock(impl_->mutex); ++impl_->admission->generation; }
 impl_->changed.notify_all();
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
  Admission::Waiter waiter(ledger);
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
  Admission::Waiter waiter(ledger);
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
 impl_->admission->require(resources, true);
 auto* parent = Impl::Frame::current;
 if (parent && &parent->owner == impl_.get()) {
  { const std::lock_guard lock(impl_->mutex);
   if (!Admission::covers(*parent->allowance.credits_, resources)) throw std::logic_error("nested benchmark chunks exceed their parent's allowance");
  }
  if (!count) return;
  Impl::Frame frame(*impl_, parent->lane);
  bool borrowed_scratch;
  {
   const std::lock_guard lock(impl_->mutex);
   borrowed_scratch = retire && impl_->lanes[parent->lane].owns(&retire);
   frame.enter(parent->allowance, retire ? static_cast<const void*>(&retire) : parent->scratch);
  }
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
 Impl::WorkGroup group(*this, retire ? &retire : nullptr);
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
    job.resources = resources;
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
  slot.job.work = &slot;
  writer_slots[i] = &slot;
 }
}
void BenchmarkCompilePipeline::Impl::ImageState::admit(Slot& slot, bool independent) {
 slot.submitted = true;
 auto& job = slot.job;
 job.stage = BenchmarkStage::Header;
 job.resources = BenchmarkResources::handles(1);
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
 auto [position, inserted] = source.geometry.emplace(id, Source::Geometry{generation, dimensions.first, dimensions.second});
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
 std::optional<std::pair<std::uint32_t, std::uint32_t>> dimensions, bool defer_pixels, std::uint64_t attempt) {
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
  const auto found = source.slots.find(id);
  if (found == source.slots.end() || found->second.submitted || found->second.retiring) return;
  auto& slot = found->second;
  slot.generation = generation;
  slot.publication = std::move(publication);
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
  ready.dimensions, ready.defer_pixels || state_->defer_pixels, state_->attempt);
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
  for (std::size_t stage = 0; stage < stage_count; ++stage) {
   while (impl_->heads[stage]) {
    auto* job = impl_->pop(stage);
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
