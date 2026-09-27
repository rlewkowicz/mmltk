#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
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
namespace mmltk::backend::data::benchmark_internal {
namespace {
constexpr std::size_t stage_count = static_cast<std::size_t>(BenchmarkStage::Count);
thread_local const void* active_pipeline = nullptr;
thread_local std::size_t active_lane = 0;
thread_local BenchmarkResources active_resources{};
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
struct BenchmarkCompilePipeline::Impl {
 bool consume(BenchmarkCompilePipeline&, std::size_t lane, bool wait);
 struct Job;
 struct Batch { std::span<Job*> jobs; std::exception_ptr failure; };
 struct Job {
  BenchmarkStage stage = BenchmarkStage::Metadata;
  BenchmarkResources resources{};
  Allowance allowance;
  const std::function<void(std::size_t)>* callback = nullptr;
  Job* next = nullptr;
  Job* previous = nullptr;
  Batch* batch = nullptr;
  const std::function<void(std::size_t)>* retire = nullptr;
  bool queued = false;
  void* pixel = nullptr;
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
  std::shared_ptr<const ArtifactLease> custody;
  std::shared_ptr<BenchmarkPixelInput> input;
  Allowance input_allowance;
 };
 struct Source {
  std::filesystem::path root;
  BenchmarkSourceGeneration generation = 1, counter = 1;
  std::unordered_map<std::uint64_t, BenchmarkSourceGeneration> replacements;
  BenchmarkSourceGeneration image_generation(std::uint64_t id) const { const auto found = replacements.find(id); return found == replacements.end() ? generation : found->second; }
  bool retiring = false;
  std::size_t pending = 0;
  std::unordered_map<std::uint64_t, Slot> slots;
  std::unordered_map<std::uint64_t, BenchmarkImageGeometry> geometry;
 };
 struct IdleScratch {
  BenchmarkSplitWriter* writer = nullptr;
  const std::function<void(std::size_t)>* retire = nullptr;
  Allowance allowance;
  const void* owner() const { return writer ? static_cast<const void*>(writer) : retire; }
  void release(std::size_t lane) { if (writer) writer->retire_scratch(lane); else if (retire) (*retire)(lane); allowance = {}; }
 };
 std::vector<IdleScratch> idle;
 std::vector<bool> busy;
 std::vector<const void*> retiring_scratch;
 StorageReservationPool storage{".", {}};
 std::vector<int> cpus;
 std::uint64_t target = 0, bytes = 0, handle_bytes = 0;
 std::size_t descriptor_capacity = 0, descriptors = 0;
 std::mutex mutex;
 std::condition_variable changed;
 std::array<Job*, stage_count> heads{}, tails{};
 std::unordered_map<std::string, Source> sources;
 std::unordered_map<BenchmarkSplitWriter*, std::vector<Slot*>> writers;
 std::size_t cursor = 0, metadata_streak = 0, pending = 0, active = 0, external_cpus = 0, resource_waiters = 0;
 bool membership_pending = true, stopping = false, shutdown = false, admitted = false;
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
 bool fits_bytes(BenchmarkResources value) const {
  if (value.retained_handles || !value.bytes) return true;
  const auto workspace = bytes;
  return value.bytes > target ? workspace == 0 : workspace <= target - value.bytes;
 }
 bool fits(BenchmarkResources value) const {
  if (value.cpu_workers && (value.cpu_workers >= cpus.size() || external_cpus + active > cpus.size() - value.cpu_workers)) return false;
  const auto ceiling = value.producer ? descriptor_capacity - std::min<std::size_t>(8, descriptor_capacity / 4) : descriptor_capacity;
  if (value.continuation_descriptors > ceiling || value.descriptors > ceiling - value.continuation_descriptors || descriptors > ceiling - value.continuation_descriptors - value.descriptors) return false;
  // Control records remain charged and bounded by descriptor admission. They
  // cannot be evicted while a dependent stream runs. Only transient workspace
  // competes for the soft target; exactly one oversized workspace can run.
  return fits_bytes(value);
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
 void fail_batch(Job& job, std::exception_ptr error) {
  if (!job.batch || job.batch->failure) return;
  job.batch->failure = error;
  for (auto* sibling : job.batch->jobs) {
   if (!sibling->queued || sibling->pixel) continue;
   remove(*sibling);
   sibling->failure = error;
   sibling->done = true;
   --pending;
  }
 }
 [[nodiscard]] Source& source(const std::filesystem::path& root) {
  auto [position, inserted] = sources.try_emplace(root.string());
  if (inserted) position->second.root = root;
  return position->second;
 }
};
struct BenchmarkCompilePipeline::Credits {
 std::shared_ptr<Impl> owner;
 BenchmarkResources resources;
 Credits(std::shared_ptr<Impl> value, BenchmarkResources demand) : owner(std::move(value)), resources(demand) {}
 ~Credits() {
  {
   const std::lock_guard lock(owner->mutex);
   if (resources.retained_handles) owner->handle_bytes -= resources.bytes;
   else owner->bytes -= resources.bytes;
   owner->descriptors -= resources.descriptors;
   owner->external_cpus -= resources.cpu_workers;
  }
  owner->changed.notify_all();
 }
};
std::uint64_t BenchmarkCompilePipeline::Allowance::bytes() const noexcept { return credits_ ? credits_->resources.bytes : 0; }
std::shared_ptr<ArtifactLease> BenchmarkCompilePipeline::Allowance::retain(ArtifactLease lease) const {
 return std::shared_ptr<ArtifactLease>(new ArtifactLease(std::move(lease)), [credits = credits_](ArtifactLease* value) { (void)credits; delete value; });
}
bool BenchmarkCompilePipeline::Impl::consume(BenchmarkCompilePipeline& owner, std::size_t lane, bool wait) {
 auto& state = *this;
 Impl::Job* job = nullptr;
 Impl::IdleScratch retired;
 std::size_t retired_lane = lane;
 {
  std::unique_lock lock(state.mutex);
  const auto select = [&] {
   if (state.shutdown) return true;
   if (wait && state.busy[lane]) return false;
   const bool discard = state.stopping || state.failure || state.cancellation.requested();
   const bool pressure = state.resource_waiters || std::ranges::any_of(state.heads, [&](auto* pending) {
    if (!pending || pending->allowance || state.fits_bytes(pending->resources)) return false;
    const auto* slot = static_cast<Impl::Slot*>(pending->pixel);
    const void* owner = slot && pending->stage == BenchmarkStage::Pixels ? static_cast<const void*>(slot->writer) : pending->retire;
    return !owner || std::ranges::none_of(state.idle, [&](const auto& scratch) { return scratch.owner() == owner && scratch.allowance.bytes() >= pending->resources.bytes; });
   });
   if (pressure)
    for (std::size_t i = 0; i < state.idle.size(); ++i) {
     if (state.busy[i] || !state.idle[i].owner()) continue;
     retired = std::move(state.idle[i]);
     state.idle[i] = {};
     retired_lane = i;
     state.busy[i] = true;
     state.retiring_scratch[i] = retired.owner();
     return true;
    }
   const auto eligible = [&](std::size_t stage) {
    const auto* candidate = state.heads[stage];
    const bool cancelled_batch = candidate && candidate->batch && candidate->batch->failure;
    if (!candidate || ((!discard || candidate->finish_started) && !cancelled_batch && state.active + state.external_cpus >= state.cpus.size() + (wait ? 0 : 1))) return false;
    const auto* slot = static_cast<Impl::Slot*>(candidate->pixel);
    const void* owner = slot && candidate->stage == BenchmarkStage::Pixels ? static_cast<const void*>(slot->writer) : candidate->retire;
    const bool reuse = owner && state.idle[lane].owner() == owner && state.idle[lane].allowance.bytes() >= candidate->resources.bytes;
    return cancelled_batch || (discard && !candidate->finish_started) || candidate->allowance || reuse || state.fits(candidate->resources);
   };
   if (state.membership_pending && state.metadata_streak < 2 && eligible(0)) {
    job = state.pop(0);
    ++state.metadata_streak;
   } else {
    for (std::size_t count = 0; count < stage_count; ++count) {
     const auto stage = (state.cursor + count) % stage_count;
     if (!eligible(stage)) continue;
     job = state.pop(stage);
     state.cursor = (stage + 1) % stage_count;
     state.metadata_streak = 0;
     break;
    }
   }
   if (!job) {
    // Idle capacity remains charged and reusable. Resource pressure retires
    // only idle lanes, before waiting; no runnable consumer waits on its own
    // previously retained scratch or on another idle oversized allocation.
    if (state.resource_waiters || std::ranges::any_of(state.heads, [](auto* head) { return head != nullptr; }))
     for (std::size_t i = 0; i < state.idle.size(); ++i) {
      if (!state.busy[i] && state.idle[i].owner()) {
       retired = std::move(state.idle[i]);
       state.idle[i] = {};
       retired_lane = i;
       state.busy[i] = true;
       state.retiring_scratch[i] = retired.owner();
       return true;
      }
     }
    return false;
   }
   const auto* pixel_slot = static_cast<Impl::Slot*>(job->pixel);
   const void* scratch_owner = pixel_slot && job->stage == BenchmarkStage::Pixels ? static_cast<const void*>(pixel_slot->writer) : job->retire;
   if (scratch_owner && state.idle[lane].owner()) {
    if (!discard && state.idle[lane].owner() == scratch_owner && state.idle[lane].allowance.bytes() >= job->resources.bytes) {
     job->allowance = std::move(state.idle[lane].allowance);
     state.idle[lane] = {};
    } else {
     retired = std::move(state.idle[lane]);
     state.idle[lane] = {};
     state.retiring_scratch[lane] = retired.owner();
    }
   }
   if ((!discard || job->finish_started) && !(job->batch && job->batch->failure) && !job->allowance) {
    // Allocation precedes accounting: construction failure has no debt.
    try {
     auto credits = std::make_shared<Credits>(owner.impl_, job->resources);
     if (job->resources.retained_handles) state.handle_bytes += job->resources.bytes;
     else state.bytes += job->resources.bytes;
     state.descriptors += job->resources.descriptors;
     state.external_cpus += job->resources.cpu_workers;
     job->allowance = Allowance(std::move(credits));
    } catch (...) { job->failure = std::current_exception(); }
   }
   state.busy[lane] = true;
   if (wait) ++state.active;
   return true;
  };
  if (wait) state.changed.wait(lock, select);
  else if (!select()) return false;
 }
 if (retired.owner()) {
  retired.release(retired_lane);
  { const std::lock_guard lock(state.mutex); state.retiring_scratch[retired_lane] = nullptr; }
  state.changed.notify_all();
  if (!job) {
   { const std::lock_guard lock(state.mutex); state.busy[retired_lane] = false; }
   state.changed.notify_all();
   return true;
  }
 }
 if (!job) return false;
 std::exception_ptr error = job->failure;
 bool continue_pixels = false, recoverable = false;
 std::uint64_t pixel_bytes = 0;
 auto* slot = static_cast<Impl::Slot*>(job->pixel);
 try {
  bool discarded;
  {
   const std::lock_guard lock(state.mutex);
   discarded = ((state.stopping || state.failure) && !job->finish_started) || (slot && (slot->source->retiring || slot->retiring)) || (job->batch && job->batch->failure);
  }
  if (!discarded && !error) {
   throw_if_benchmark_cancelled(state.cancellation);
   active_resources = job->allowance.credits_->resources;
   if (!slot) (*job->callback)(lane);
   else if (job->stage == BenchmarkStage::Header) {
    slot->input = slot->writer->prepare_pixel(slot->index, lane);
    if (slot->input) {
     const auto dimensions = slot->writer->header_dimensions(slot->index);
     owner.geometry_ready({slot->source->root, slot->image_id, slot->generation, dimensions->first, dimensions->second});
     pixel_bytes = slot->writer->pixel_workspace_bytes(*slot->input);
     continue_pixels = true;
    }
   } else slot->writer->write_pixel(slot->index, lane, slot->input);
  } else if (discarded && !slot) {
   const std::lock_guard lock(state.mutex);
   error = state.failure ? state.failure : state.retired_failure;
  }
 } catch (const BenchmarkImageReadError&) {
  error = std::current_exception();
  recoverable = slot != nullptr;
  // Header and pixel completion remain distinct; bounded source repair owns
  // an undecodable body. All other failures keep first-failure semantics.
 } catch (...) { error = std::current_exception(); }
 active_resources = {};
 if (slot && continue_pixels) slot->input_allowance = std::move(job->allowance);
 if (((slot && job->stage == BenchmarkStage::Pixels) || job->retire) && job->allowance) {
  const std::lock_guard lock(state.mutex);
  state.idle[lane].writer = slot ? slot->writer : nullptr;
  state.idle[lane].retire = job->retire;
  state.idle[lane].allowance = std::move(job->allowance);
 }
 job->allowance = {};
 if (slot && !continue_pixels) {
  slot->input.reset();
  slot->input_allowance = {};
  slot->custody.reset();
 }
 {
  const std::lock_guard lock(state.mutex);
  if (error) {
   state.fail_batch(*job, error);
   if (slot && !recoverable && !job->batch && !state.failure) state.failure = error;
  }
  if (wait) { --state.active; state.busy[lane] = false; }
  if (continue_pixels) {
   job->finish_started = state.stopping || static_cast<bool>(state.failure);
   job->stage = BenchmarkStage::Pixels;
   job->resources = {pixel_bytes, 0};
   state.push(*job);
  } else {
   job->failure = error;
   job->done = true;
   --state.pending;
   if (slot) --slot->source->pending;
  }
 }
 state.changed.notify_all();
 return true;
}
BenchmarkCompilePipeline::BenchmarkCompilePipeline(std::size_t workers, std::span<const int> cpus, BenchmarkExecutionLimits limits,
 mmltk::common::concurrency::CancellationObservation cancellation) : impl_(std::make_shared<Impl>()) {
 auto& state = *impl_;
 state.cpus = cpus.empty() ? mmltk::common::system::allowed_cpu_set() : std::vector<int>(cpus.begin(), cpus.end());
 workers = std::max<std::size_t>(1, workers);
 if (state.cpus.size() < workers) throw std::invalid_argument("benchmark worker budget exceeds assigned CPUs");
 state.cpus.resize(workers);
 state.idle.resize(workers);
 state.busy.resize(workers);
 state.retiring_scratch.resize(workers);
 state.target = limits.transient_bytes ? limits.transient_bytes : std::min<std::uint64_t>(2ULL << 30, std::max<std::uint64_t>(256ULL << 20, workers * (64ULL << 20)));
 const auto headroom = descriptor_headroom();
 state.descriptor_capacity = limits.descriptors ? std::min(limits.descriptors, headroom) : headroom;
 if (state.descriptor_capacity < 2) throw std::invalid_argument("benchmark descriptor admission requires completion headroom");
 state.cancellation = cancellation;
 state.pool = std::make_unique<mmltk::common::concurrency::WorkerPool>(workers, state.cpus, "bench_cpu", workers);
 if (state.pool->size() != workers) throw std::invalid_argument("benchmark CPU assignment contains repeated CPUs");
 try {
  for (std::size_t lane = 0; lane < workers; ++lane) state.pool->enqueue_detached([this, lane] {
   auto& state = *impl_;
   active_pipeline = &state;
   active_lane = lane;
   while (state.consume(*this, lane, true)) {}
   active_pipeline = nullptr;
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
 if (active_pipeline != impl_.get()) throw std::logic_error("benchmark lane requested outside CPU work");
 return active_lane;
}
StorageReservationPool& BenchmarkCompilePipeline::storage() noexcept { return impl_->storage; }
std::span<const int> BenchmarkCompilePipeline::cpus() const noexcept { return impl_->cpus; }
std::uint64_t BenchmarkCompilePipeline::transient_target() const noexcept { return impl_->target; }
std::size_t BenchmarkCompilePipeline::descriptor_limit() const noexcept { return impl_->descriptor_capacity; }
std::optional<BenchmarkCompilePipeline::Allowance> BenchmarkCompilePipeline::try_reserve(BenchmarkResources resources) {
 const std::lock_guard lock(impl_->mutex);
 if (impl_->failure) std::rethrow_exception(impl_->failure);
 throw_if_benchmark_cancelled(impl_->cancellation);
 if (impl_->stopping) throw std::logic_error("benchmark resource admission after shutdown");
 if (!impl_->fits(resources)) return std::nullopt;
 auto credits = std::make_shared<Credits>(impl_, resources);
 if (resources.retained_handles) impl_->handle_bytes += resources.bytes;
 else impl_->bytes += resources.bytes;
 impl_->descriptors += resources.descriptors;
 impl_->external_cpus += resources.cpu_workers;
 return Allowance(std::move(credits));
}
BenchmarkCompilePipeline::Allowance BenchmarkCompilePipeline::reserve(BenchmarkResources resources) {
 if (active_pipeline == impl_.get()) throw std::logic_error("benchmark CPU lane cannot wait for resource credits");
 if (resources.cpu_workers >= workers() && resources.cpu_workers) throw std::invalid_argument("external benchmark work must retain a CPU for consumers");
 const auto ceiling = impl_->descriptor_capacity - (resources.producer ? std::min<std::size_t>(8, impl_->descriptor_capacity / 4) : 0);
 if (resources.continuation_descriptors > ceiling || resources.descriptors > ceiling - resources.continuation_descriptors)
  throw InsufficientBenchmarkResources("benchmark operation exceeds descriptor admission");
 for (;;) {
  if (auto result = try_reserve(resources)) return std::move(*result);
  std::unique_lock lock(impl_->mutex);
  ++impl_->resource_waiters;
  impl_->changed.notify_all();
  impl_->wait(lock, [&] { return impl_->stopping || impl_->failure || impl_->cancellation.requested() || impl_->fits(resources); });
  --impl_->resource_waiters;
 }
}
std::pair<std::size_t, BenchmarkCompilePipeline::Allowance> BenchmarkCompilePipeline::reserve_transfers(std::size_t requested, std::size_t fixed_descriptors, std::uint64_t per_connection, std::uint64_t fixed_bytes) {
 if (!requested) throw std::invalid_argument("benchmark transfer concurrency must be positive");
 if (active_pipeline == impl_.get()) throw std::logic_error("benchmark CPU lane cannot wait for transfers");
 const auto ceiling = impl_->descriptor_capacity - std::min<std::size_t>(8, impl_->descriptor_capacity / 4);
 if (fixed_descriptors > ceiling || ceiling - fixed_descriptors < 3) throw InsufficientBenchmarkResources("benchmark transfer minimum exceeds descriptor admission");
 const auto demand = [&](std::size_t count) {
  return BenchmarkResources{mmltk::common::math::checked_add(fixed_bytes, mmltk::common::math::checked_multiply(per_connection, static_cast<std::uint64_t>(count), "benchmark transfer workspace overflow"), "benchmark transfer workspace overflow"), fixed_descriptors + 3 * count, true};
 };
 const auto minimum = demand(1);
 for (;;) {
  std::unique_lock lock(impl_->mutex);
  if (impl_->failure) std::rethrow_exception(impl_->failure);
  throw_if_benchmark_cancelled(impl_->cancellation);
  if (impl_->stopping) throw std::logic_error("benchmark transfer admission after shutdown");
  if (impl_->fits(minimum)) {
   auto count = std::min(requested, (ceiling - impl_->descriptors - fixed_descriptors) / 3);
   const auto workspace = impl_->bytes;
   if (per_connection && fixed_bytes < impl_->target && workspace <= impl_->target - fixed_bytes)
    count = std::min(count, std::max<std::size_t>(1, (impl_->target - fixed_bytes - workspace) / per_connection));
   else count = 1;
   const auto resources = demand(count);
   auto credits = std::make_shared<Credits>(impl_, resources);
   impl_->bytes += resources.bytes;
   impl_->descriptors += resources.descriptors;
   return {count, Allowance(std::move(credits))};
  }
  ++impl_->resource_waiters;
  impl_->changed.notify_all();
  impl_->wait(lock, [&] { return impl_->stopping || impl_->failure || impl_->cancellation.requested() || impl_->fits(minimum); });
  --impl_->resource_waiters;
 }
}
void BenchmarkCompilePipeline::run(BenchmarkStage stage, BenchmarkResources resources, const std::function<void(std::size_t)>& callback, Allowance allowance) {
 if (allowance.credits_ && allowance.credits_->owner != impl_) throw std::invalid_argument("benchmark allowance belongs to another compile");
 if (allowance && (resources.bytes > allowance.credits_->resources.bytes || resources.descriptors > allowance.credits_->resources.descriptors))
  throw std::invalid_argument("benchmark job exceeds its transferred allowance");
 if (active_pipeline == impl_.get()) {
  // A chunk may call a local helper, but may never enqueue children and wait
  // with its lane/allowance held. Its parent already owns the full allowance.
  if (resources.cpu_workers || (!allowance && (resources.bytes > active_resources.bytes || resources.descriptors > active_resources.descriptors)))
   throw std::logic_error("nested benchmark work exceeds its parent's allowance");
  callback(active_lane);
  return;
 }
 Impl::Job job;
 job.stage = stage;
 job.resources = resources;
 job.callback = &callback;
 job.allowance = std::move(allowance);
 {
  const std::lock_guard lock(impl_->mutex);
  if (impl_->failure) std::rethrow_exception(impl_->failure);
  if (impl_->stopping) throw std::logic_error("benchmark CPU admission after shutdown");
  const auto ceiling = impl_->descriptor_capacity - (resources.producer ? std::min<std::size_t>(8, impl_->descriptor_capacity / 4) : 0);
  if (resources.cpu_workers || resources.continuation_descriptors > ceiling || resources.descriptors > ceiling - resources.continuation_descriptors)
   throw InsufficientBenchmarkResources("benchmark job exceeds descriptor admission");
  ++impl_->pending;
  impl_->push(job);
 }
 impl_->changed.notify_all();
 std::unique_lock lock(impl_->mutex);
 impl_->wait(lock, [&] { return job.done; });
 if (job.failure) std::rethrow_exception(job.failure);
 if (impl_->failure) std::rethrow_exception(impl_->failure);
 throw_if_benchmark_cancelled(impl_->cancellation);
}
void BenchmarkCompilePipeline::for_each(BenchmarkStage stage, std::size_t count, BenchmarkResources resources, const std::function<void(std::size_t)>& callback, const std::function<void(std::size_t)>& retire) {
 const auto ceiling = impl_->descriptor_capacity - (resources.producer ? std::min<std::size_t>(8, impl_->descriptor_capacity / 4) : 0);
 if (resources.cpu_workers || resources.continuation_descriptors > ceiling || resources.descriptors > ceiling - resources.continuation_descriptors)
  throw InsufficientBenchmarkResources("benchmark chunk exceeds descriptor admission");
 if (active_pipeline == impl_.get()) {
  if (resources.bytes > active_resources.bytes || resources.descriptors > active_resources.descriptors)
   throw std::logic_error("nested benchmark chunks exceed their parent's allowance");
  for (std::size_t index = 0; index < count; ++index) callback(index);
  return;
 }
 // Source partitions are bounded records. Recycle these borrowed queue records
 // after every batch instead of allocating one closure/future per input row.
 const auto capacity = std::min(count, workers() * 2);
 std::vector<Impl::Job> jobs(capacity);
 std::vector<std::function<void(std::size_t)>> calls(capacity);
 std::vector<Impl::Job*> members(capacity);
 for (std::size_t i = 0; i < capacity; ++i) members[i] = &jobs[i];
 struct RetireWorkspace { BenchmarkCompilePipeline& owner; const void* key; ~RetireWorkspace() { if (key) owner.retire_workspace(key); } } retiring{*this, retire ? &retire : nullptr};
 for (std::size_t first = 0; first < count; first += capacity) {
  const auto size = std::min(capacity, count - first);
  Impl::Batch batch{std::span(members).first(size), {}};
  {
   const std::lock_guard lock(impl_->mutex);
   if (impl_->failure) std::rethrow_exception(impl_->failure);
   if (impl_->stopping) throw std::logic_error("benchmark CPU admission after shutdown");
   for (std::size_t i = 0; i < size; ++i) {
    calls[i] = [&, index = first + i](std::size_t) { callback(index); };
    jobs[i].stage = stage;
    jobs[i].resources = resources;
    jobs[i].callback = &calls[i];
    jobs[i].batch = &batch;
    jobs[i].retire = retire ? &retire : nullptr;
    jobs[i].done = false;
    jobs[i].failure = {};
   }
   for (std::size_t i = 0; i < size; ++i) { ++impl_->pending; impl_->push(jobs[i]); }
  }
  impl_->changed.notify_all();
  std::unique_lock lock(impl_->mutex);
  impl_->wait(lock, [&] { return std::all_of(jobs.begin(), jobs.begin() + static_cast<std::ptrdiff_t>(size), [](const auto& job) { return job.done; }); });
  if (batch.failure) std::rethrow_exception(batch.failure);
  for (std::size_t i = 0; i < size; ++i) if (jobs[i].failure) std::rethrow_exception(jobs[i].failure);
  if (impl_->failure) std::rethrow_exception(impl_->failure);
  throw_if_benchmark_cancelled(impl_->cancellation);
 }
}
void BenchmarkCompilePipeline::cooperate() {
 if (active_pipeline != impl_.get()) throw std::logic_error("benchmark cooperative yield requires a CPU lane");
 const auto resources = active_resources;
 struct Restore { BenchmarkResources resources; ~Restore() { active_resources = resources; } } restore{resources};
 throw_if_benchmark_cancelled(impl_->cancellation);
 (void)impl_->consume(*this, active_lane, false);
 throw_if_benchmark_cancelled(impl_->cancellation);
}
void BenchmarkCompilePipeline::retire_workspace(const void* owner) noexcept {
 std::unique_lock lock(impl_->mutex);
 // A pressure drainer may already be releasing this workspace on an idle lane.
 impl_->wait(lock, [&] { return std::ranges::find(impl_->retiring_scratch, owner) == impl_->retiring_scratch.end(); });
 for (std::size_t lane = 0; lane < impl_->idle.size(); ++lane) {
  if (impl_->idle[lane].owner() != owner) continue;
  auto retired = std::move(impl_->idle[lane]);
  impl_->idle[lane] = {};
  lock.unlock();
  retired.release(lane);
  lock.lock();
 }
 impl_->wait(lock, [&] { return std::ranges::find(impl_->retiring_scratch, owner) == impl_->retiring_scratch.end(); });
 lock.unlock();
 impl_->changed.notify_all();
}
void BenchmarkCompilePipeline::write_remaining(BenchmarkSplitWriter& writer, const PreparedBenchmarkSplit& split, std::span<const std::size_t> slots) {
 bool registered;
 { const std::lock_guard lock(impl_->mutex); registered = impl_->writers.contains(&writer); }
 if (!registered) register_split(writer, split);
 std::vector<Impl::Job*> jobs;
 jobs.reserve(std::min(slots.size(), workers() * 2));
 for (std::size_t first = 0; first < slots.size(); first += workers() * 2) {
  jobs.clear();
  const auto size = std::min(workers() * 2, slots.size() - first);
  std::unique_lock lock(impl_->mutex);
  for (std::size_t i = 0; i < size; ++i) jobs.push_back(&impl_->writers.at(&writer).at(slots[first + i])->job);
  impl_->wait(lock, [&] { return std::ranges::all_of(jobs, [](auto* job) { return !static_cast<Impl::Slot*>(job->pixel)->submitted || job->done; }); });
  if (impl_->failure) std::rethrow_exception(impl_->failure);
  for (auto* job : jobs) if (job->failure) std::rethrow_exception(job->failure);
  Impl::Batch batch{jobs, {}};
  for (auto* job : jobs) {
   auto& slot = *static_cast<Impl::Slot*>(job->pixel);
   job->done = true;
   if (writer.image_complete(slot.index)) continue;
   slot.submitted = true;
   job->batch = &batch;
   job->done = false;
   job->finish_started = false;
   job->stage = BenchmarkStage::Header;
   job->resources = BenchmarkResources::handles(1);
   ++impl_->pending;
   ++slot.source->pending;
   impl_->push(*job);
  }
  impl_->changed.notify_all();
  impl_->wait(lock, [&] { return std::ranges::all_of(jobs, [](auto* job) { return job->done; }); });
  for (auto* job : jobs) job->batch = nullptr;
  if (batch.failure) std::rethrow_exception(batch.failure);
  if (impl_->failure) std::rethrow_exception(impl_->failure);
  throw_if_benchmark_cancelled(impl_->cancellation);
 }
}
void BenchmarkCompilePipeline::membership_ready() { const std::lock_guard lock(impl_->mutex); impl_->membership_pending = false; }
void BenchmarkCompilePipeline::register_split(BenchmarkSplitWriter& writer, const PreparedBenchmarkSplit& split) {
 const std::lock_guard lock(impl_->mutex);
 if (impl_->admitted) throw std::logic_error("benchmark membership changed after readiness admission");
 writer.prepare_lanes(workers());
 auto& writer_slots = impl_->writers[&writer];
 writer_slots.resize(split.images.size());
 for (std::size_t i = 0; i < split.images.size(); ++i) {
  const auto& image = split.images[i];
  auto& source = impl_->source(split.sources.at(image.source_index).root);
  auto [position, inserted] = source.slots.try_emplace(image.source_image_id);
  if (!inserted) throw std::runtime_error("benchmark pixel membership repeats a physical image");
  auto& slot = position->second;
  slot.source = &source;
  slot.writer = &writer;
  slot.index = i;
  slot.image_id = image.source_image_id;
  slot.generation = source.image_generation(image.source_image_id);
  slot.job.pixel = &slot;
  writer_slots[i] = &slot;
 }
}
BenchmarkSourceGeneration BenchmarkCompilePipeline::source_generation(const std::filesystem::path& root) {
 const std::lock_guard lock(impl_->mutex);
 return impl_->source(root).generation;
}
BenchmarkSourceGeneration BenchmarkCompilePipeline::image_generation(const std::filesystem::path& root, std::uint64_t id) {
 const std::lock_guard lock(impl_->mutex);
 return impl_->source(root).image_generation(id);
}
void BenchmarkCompilePipeline::retire_image(const std::filesystem::path& root, std::uint64_t id) {
 std::unique_lock lock(impl_->mutex);
 auto& source = impl_->source(root);
 // Allocate the replacement entry before withdrawing anything.
 auto [replacement, inserted] = source.replacements.try_emplace(id, 0);
 (void)inserted;
 const auto generation = ++source.counter;
 replacement->second = generation;
 const auto found = source.slots.find(id);
 if (found == source.slots.end()) { source.geometry.erase(id); return; }
 auto& slot = found->second;
 slot.retiring = true;
 if (slot.job.batch) impl_->fail_batch(slot.job, impl_->retired_failure);
 if (slot.job.queued) {
  impl_->remove(slot.job);
  lock.unlock();
  slot.input.reset(); slot.input_allowance = {}; slot.custody.reset(); slot.job.allowance = {};
  lock.lock();
  slot.job.done = true;
  --impl_->pending;
  --source.pending;
 }
 impl_->changed.notify_all();
 impl_->wait(lock, [&] { return !slot.submitted || slot.job.done; });
 source.geometry.erase(id);
 slot.submitted = false;
 slot.job.failure = {};
 slot.generation = generation;
 lock.unlock();
 slot.writer->invalidate_image(slot.index);
 lock.lock();
 slot.retiring = false;
 lock.unlock();
 impl_->changed.notify_all();
}
void BenchmarkCompilePipeline::geometry_ready(const BenchmarkImageGeometry& fact) {
 if (!fact.width || !fact.height) throw std::invalid_argument("benchmark geometry is empty");
 const std::lock_guard lock(impl_->mutex);
 auto& source = impl_->source(fact.root);
 if (source.retiring || source.image_generation(fact.image_id) != fact.generation) return;
 const auto slot = source.slots.find(fact.image_id);
 if (slot != source.slots.end() && slot->second.retiring) return;
 auto [position, inserted] = source.geometry.emplace(fact.image_id, fact);
 if (!inserted && (position->second.width != fact.width || position->second.height != fact.height)) throw std::runtime_error("benchmark source generation has contradictory geometry");
 position->second.generation = fact.generation;
}
std::optional<BenchmarkImageGeometry> BenchmarkCompilePipeline::geometry(const std::filesystem::path& root, std::uint64_t id) const {
 const std::lock_guard lock(impl_->mutex);
 const auto source = impl_->sources.find(root.string());
 if (source == impl_->sources.end() || source->second.retiring) return std::nullopt;
 const auto found = source->second.geometry.find(id);
 return found == source->second.geometry.end() || found->second.generation != source->second.image_generation(id) ? std::nullopt : std::optional(found->second);
}
void BenchmarkCompilePipeline::image_ready(const CachedImageReady& ready) {
 if (ready.dimensions) geometry_ready({ready.root, ready.image_id, ready.generation, ready.dimensions->first, ready.dimensions->second});
 Impl::Job* accepted = nullptr;
 {
  const std::lock_guard lock(impl_->mutex);
  if (impl_->failure) std::rethrow_exception(impl_->failure);
  if (impl_->stopping) throw std::logic_error("benchmark pixel readiness after shutdown");
  const auto source = impl_->sources.find(ready.root.string());
  if (source == impl_->sources.end() || source->second.retiring || ready.generation != source->second.image_generation(ready.image_id)) return;
  const auto found = source->second.slots.find(ready.image_id);
  if (found == source->second.slots.end() || found->second.submitted || found->second.retiring) return;
  auto& slot = found->second;
  slot.submitted = true;
  slot.generation = ready.generation;
  slot.custody = ready.custody;
  slot.job.stage = BenchmarkStage::Header;
  slot.job.resources = BenchmarkResources::handles(1);
  slot.job.done = false;
  slot.job.failure = {};
  slot.job.finish_started = false;
  impl_->admitted = true;
  ++impl_->pending;
  ++slot.source->pending;
  impl_->push(slot.job);
  accepted = &slot.job;
 }
 impl_->changed.notify_all();
 if (workers() == 1 && !ready.defer_pixels && active_pipeline != impl_.get()) {
  std::unique_lock lock(impl_->mutex);
  impl_->wait(lock, [&] { return accepted->done; });
  if (impl_->failure) std::rethrow_exception(impl_->failure);
 }
}
void BenchmarkCompilePipeline::retire_source(const std::filesystem::path& root) {
 Impl::Job* retired = nullptr;
 std::vector<BenchmarkSplitWriter*> writers;
 {
  std::unique_lock lock(impl_->mutex);
  auto& source = impl_->source(root);
  for (const auto& [id, slot] : source.slots) {
   (void)id;
   if (std::ranges::find(writers, slot.writer) == writers.end()) writers.push_back(slot.writer);
  }
  source.retiring = true;
  source.generation = ++source.counter;
  source.replacements.clear();
  // Remove only this source from intrusive queues; no task can re-admit its
  // old generation after the closure. Active header continuations settle too.
  for (std::size_t stage = 0; stage < stage_count; ++stage) {
   for (auto* job = impl_->heads[stage]; job;) {
    auto* next = job->next;
    auto* slot = static_cast<Impl::Slot*>(job->pixel);
    if (slot && slot->source == &source) { impl_->fail_batch(*job, impl_->retired_failure); impl_->remove(*job); job->next = retired; retired = job; }
    job = next;
   }
  }
  lock.unlock();
  while (retired) {
   auto* job = retired;
   retired = job->next;
   auto* slot = static_cast<Impl::Slot*>(job->pixel);
   slot->input.reset(); slot->input_allowance = {}; slot->custody.reset(); job->allowance = {};
   lock.lock();
   job->done = true; --impl_->pending; --source.pending;
   lock.unlock();
  }
  lock.lock();
  impl_->changed.notify_all();
  impl_->wait(lock, [&] { return source.pending == 0; });
  source.geometry.clear();
  for (auto& [id, slot] : source.slots) {
   (void)id;
   slot.submitted = false;
   slot.job.failure = {};
   slot.generation = source.generation;
  }
  lock.unlock();
  for (auto* writer : writers) writer->invalidate_source(root);
  lock.lock();
  source.retiring = false;
 }
 impl_->changed.notify_all();
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
 while (discarded) {
  auto* job = discarded;
  discarded = job->next;
  job->allowance = {};
  auto* slot = static_cast<Impl::Slot*>(job->pixel);
  if (slot) { slot->input.reset(); slot->input_allowance = {}; slot->custody.reset(); }
  {
   const std::lock_guard lock(impl_->mutex);
   job->done = true;
   if (!slot) job->failure = impl_->failure ? impl_->failure : impl_->retired_failure;
   --impl_->pending;
   if (slot) --slot->source->pending;
  }
 }
 impl_->changed.notify_all();
 {
  std::unique_lock lock(impl_->mutex);
  impl_->wait(lock, [&] { return impl_->pending == 0 && std::ranges::none_of(impl_->busy, [](bool busy) { return busy; }); });
  for (std::size_t lane = 0; lane < impl_->idle.size(); ++lane) {
   auto retired = std::move(impl_->idle[lane]);
   impl_->idle[lane] = {};
   lock.unlock();
   retired.release(lane);
   lock.lock();
  }
  impl_->wait(lock, [&] { return std::ranges::none_of(impl_->busy, [](bool busy) { return busy; }); });
  for (auto& [name, source] : impl_->sources) { (void)name; source.slots.clear(); }
  impl_->writers.clear();
  impl_->admitted = false;
  impl_->membership_pending = true;
  impl_->metadata_streak = impl_->cursor = 0;
  impl_->failure = {};
  impl_->stopping = false;
 }
}
}  // namespace mmltk::backend::data::benchmark_internal
