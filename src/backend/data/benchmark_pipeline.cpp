#include "detail/benchmark_pipeline.h"
#include <algorithm>
#include <stdexcept>
#include "src/common/system/cpu_affinity.h"
namespace mmltk::backend::data::benchmark_internal {
BenchmarkCompilePipeline::BenchmarkCompilePipeline(std::size_t workers, std::span<const int> cpus) : workers_(std::max<std::size_t>(1, workers)), pixels_(workers_ == 1 ? 0 : std::max<std::size_t>(1, workers_ / 3)) {
 auto available = cpus.empty() ? mmltk::common::system::allowed_cpu_set() : std::vector<int>(cpus.begin(), cpus.end());
 if (available.size() < workers_) throw std::invalid_argument("benchmark worker budget exceeds assigned CPUs");
 available.resize(workers_);
 acquisition_cpus_.assign(available.begin(), available.begin() + static_cast<std::ptrdiff_t>(workers_ - pixels_));
 if (pixels_) {
  std::vector<int> pixel_cpus(available.end() - static_cast<std::ptrdiff_t>(pixels_), available.end());
  pool_ = std::make_unique<mmltk::common::concurrency::WorkerPool>(pixels_, std::move(pixel_cpus), "bench_pixel", pixels_);
  try {
   // Exactly P borrowed consumers use the pool's preallocated P queue entries.
   // Image admission never submits work or allocates a per-image closure.
   for (std::size_t lane = 0; lane < pixels_; ++lane)
    pool_->enqueue_borrowed(this, lane, [](void* owner, std::size_t index) { static_cast<BenchmarkCompilePipeline*>(owner)->consume(index); });
  } catch (...) {
   stop();
   pool_.reset();
   throw;
  }
 }
}
BenchmarkCompilePipeline::~BenchmarkCompilePipeline() {
 // The pool joins while slot records and borrowed writers still exist. The
 // enclosing compile scope declares writers before this pipeline.
 stop();
 pool_.reset();
}
std::size_t BenchmarkCompilePipeline::acquisition_workers() const noexcept { return workers_ - pixels_; }
std::size_t BenchmarkCompilePipeline::pixel_workers() const noexcept { return std::max<std::size_t>(1, pixels_); }
std::span<const int> BenchmarkCompilePipeline::acquisition_cpus() const noexcept { return acquisition_cpus_; }
void BenchmarkCompilePipeline::register_split(BenchmarkSplitWriter& writer, const PreparedBenchmarkSplit& split) {
 const std::lock_guard lock(mutex_);
 if (admitted_) throw std::logic_error("benchmark membership changed after readiness admission");
 for (std::size_t i = 0; i < split.images.size(); ++i) {
  const auto& image = split.images[i];
  auto& source = slots_[split.sources.at(image.source_index).root.string()];
  if (!source.emplace(image.source_image_id, Slot{&writer, i}).second) throw std::runtime_error("benchmark pixel membership repeats a physical image");
 }
}
void BenchmarkCompilePipeline::image_ready(const CachedImageReady& ready) {
 Slot* slot = nullptr;
 {
  const std::lock_guard lock(mutex_);
  if (failure_) std::rethrow_exception(failure_);
  if (stopping_) throw std::logic_error("benchmark pixel readiness after shutdown");
  const auto source = slots_.find(ready.root.string());
  if (source == slots_.end()) return;
  const auto found = source->second.find(ready.image_id);
  if (found == source->second.end() || found->second.submitted) return;
  admitted_ = true;
  slot = &found->second;
  slot->submitted = true;
  ++pending_;
  if (pool_) {
   slot->custody = ready.custody;
   if (ready_tail_) ready_tail_->next = slot;
   else ready_head_ = slot;
   ready_tail_ = slot;
  }
 }
 if (pool_) {
  work_ready_.notify_one();
  return;
 }
 // One CPU cannot overlap acquisition with decoding. Preserve inline progress.
 execute(*slot, 0, ready.custody);
 const std::lock_guard lock(mutex_);
 if (failure_) std::rethrow_exception(failure_);
}
void BenchmarkCompilePipeline::execute(Slot& slot, std::size_t lane, std::shared_ptr<const ArtifactLease> custody) noexcept {
 std::exception_ptr failure;
 try { slot.writer->write_pixel(slot.index, lane); }
 catch (const BenchmarkImageReadError&) {
  // Failed source pixels remain unfinished for the existing bounded repair.
 } catch (...) { failure = std::current_exception(); }
 // Retirement is acknowledged only after the generation lease is released.
 custody.reset();
 {
  const std::lock_guard lock(mutex_);
  if (failure && !failure_) failure_ = failure;
  --pending_;
 }
 if (failure) work_ready_.notify_all();
 changed_.notify_all();
}
void BenchmarkCompilePipeline::consume(std::size_t lane) noexcept {
 for (;;) {
  Slot* slot;
  std::shared_ptr<const ArtifactLease> custody;
  bool discard;
  {
   std::unique_lock lock(mutex_);
   work_ready_.wait(lock, [&] { return stopping_ || failure_ || ready_head_; });
   if (!ready_head_) return;
   slot = ready_head_;
   ready_head_ = slot->next;
   if (!ready_head_) ready_tail_ = nullptr;
   slot->next = nullptr;
   custody = std::move(slot->custody);
   discard = stopping_ || static_cast<bool>(failure_);
  }
  if (!discard) execute(*slot, lane, std::move(custody));
  else {
   // A fatal write stops queued work, but every accepted lease still retires.
   custody.reset();
   { const std::lock_guard lock(mutex_); --pending_; }
   changed_.notify_all();
  }
 }
}
void BenchmarkCompilePipeline::stop() noexcept {
 Slot* queued;
 {
  const std::lock_guard lock(mutex_);
  stopping_ = true;
  queued = ready_head_;
  ready_head_ = ready_tail_ = nullptr;
 }
 work_ready_.notify_all();
 // Scope unwind can originate outside pixel work before cancellation is set.
 // Discard its queued work now; only already claimed readers need joining.
 std::size_t retired = 0;
 while (queued) {
  auto* slot = queued;
  queued = slot->next;
  slot->next = nullptr;
  slot->custody.reset();
  ++retired;
 }
 {
  const std::lock_guard lock(mutex_);
  pending_ -= retired;
 }
 changed_.notify_all();
}
void BenchmarkCompilePipeline::drain() {
 std::unique_lock lock(mutex_);
 changed_.wait(lock, [&] { return pending_ == 0; });
 if (failure_) std::rethrow_exception(failure_);
}

}  // namespace mmltk::backend::data::benchmark_internal
