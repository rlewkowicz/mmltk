#include "src/backend/data/benchmark/detail/benchmark_image_publication.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_image_decoder.h"
#include "src/backend/data/detail/file_work_queue.h"
#include <optional>
#include <algorithm>
#include <stdexcept>
#include <utility>
#include <nlohmann/json.hpp>
namespace mmltk::backend::data::benchmark_internal {
using detail::CompileChange;
using detail::CompileExecution;
using detail::CompileResources;
BenchmarkImagePublication::Payload::~Payload() {
 allowance = {};
 output = {};
 if (pool) {
  {
   const std::lock_guard lock(pool->mutex);
   ++pool->free;
  }
  if (pool->execution) pool->execution->notify_change(CompileChange::Local);
 }
}
BenchmarkImagePublication::BenchmarkImagePublication(std::filesystem::path output_root, mmltk::common::concurrency::CancellationObservation cancellation, detail::StorageReservationPool* storage,
 CompileAllowance parent, BenchmarkCompilePipeline* execution, int directory, bool retain_encoded, BenchmarkArchive& reader, const BenchmarkTraceSink& trace)
    : execution_(execution),
      directory_(directory),
      retain_encoded_(retain_encoded),
      storage_(output_root, {}, storage),
      cancellation_(cancellation),
      parent_(std::move(parent)),
      reader_(reader),
      trace_(trace),
      writes_(execution ? std::clamp<std::size_t>(execution->execution().workers() * 2, 2, 8) : 1, execution ? &execution->execution() : nullptr, [this] {
       if (execution_ && execution_->execution().resource_pressure()) reader_.suspend(parent_);
      }) {
 buffers_->free = execution ? std::clamp<std::size_t>(execution->execution().workers() * 2, 2, 8) : 1;
 buffers_->execution = execution ? &execution->execution() : nullptr;
 if (execution)
  readiness_ = std::make_unique<CompileExecution::Readiness>(
   execution->execution(), CompileChange::Resources | CompileChange::Pressure | CompileChange::Local, [this](CompileExecution::Readiness::Changes changes) {
   if (!changes.retry && !includes(changes.causes, CompileChange::Local | CompileChange::Interruption)) return;
   {
    const std::lock_guard lock(resource_mutex_);
    ++resource_generation_;
   }
   resource_changed_.notify_all();
  });
}
BenchmarkImagePublication::~BenchmarkImagePublication() {
 try {
  reader_.suspend(parent_);
  writes_.drain();
 } catch (...) {}
}
BenchmarkImagePublication::Input BenchmarkImagePublication::acquire(std::size_t size) {
 if (!size || size > BenchmarkEncodedImage::normal_window_bytes) throw InvalidImageError("selected benchmark archive image has an invalid size");
 // Reserve a publication record before retaining another encoded input.
 writes_.make_room();
 for (;;) {
  const auto observed = execution_ ? execution_->execution().admission_generation() : 0;
  writes_.reap();
  throw_if_benchmark_cancelled(cancellation_);
  std::unique_lock lock(buffers_->mutex);
  if (buffers_->free) {
   --buffers_->free;
   break;
  }
  lock.unlock();
  reader_.suspend(parent_);
  if (!execution_) throw std::logic_error("standalone image input did not retire synchronously");
  execution_->execution().wait_for_admission_change(observed);
  if (execution_)
   if (auto failure = execution_->execution().interruption()) std::rethrow_exception(failure);
 }
 Input input;
 try {
  input = std::make_shared<Payload>();
 } catch (...) {
  const std::lock_guard lock(buffers_->mutex);
  ++buffers_->free;
  throw;
 }
 input->pool = buffers_;
 if (execution_) {
  auto& owner = execution_->execution();
  std::unique_ptr<detail::CompileResourceWait> pressure;
  try {
   for (;;) {
    std::uint64_t observed;
    {
     const std::lock_guard lock(resource_mutex_);
     observed = resource_generation_;
    }
    bool retry_after_quiesce = false;
    {
     CompileExecution::Readiness::Attempt attempt(readiness_.get());
     pressure.reset();
     writes_.reap();
     const bool active_decoder = reader_.retained_workspace_bytes() != 0;
     // The output descriptor and pinned input are unaccepted local custody.
     // A paused decoder joins that same input lineage nonblockingly before
     // either owner can wait behind a newly ready exclusive consumer.
     auto output = owner.try_reserve(CompileResources::handles(1, true), parent_);
     if (output) {
      input->output = std::move(*output);
      auto granted = owner.try_reserve({.bytes = BenchmarkEncodedImage::normal_window_bytes,
                                        .continuation_descriptors = active_decoder ? 0U : 1U,
                                        .supplied_workspace = true,
                                        .workspace_class = detail::CompileWorkClass::Extraction,
                                        .workspace_window = detail::CompileWorkspaceWindow::SharedSource},
       parent_);
      if (granted) {
       input->allowance = std::move(*granted);
       if (!active_decoder) trace_benchmark_event(trace_, "benchmark.images.decoder_admission", [&] { return nlohmann::json{{"member", reader_.member()}, {"bytes", size}}; });
       if (active_decoder || reader_.try_resume(0, input->allowance)) {
        input->restored_decoder = !active_decoder;
        attempt.commit();
        break;
       }
      }
     }
     reader_.suspend(parent_);
     input->allowance = {};
     input->output = {};
     // Self-induced pressure must quiesce even before a resource waiter exists.
     trace_benchmark_event(trace_, "benchmark.images.input_wait", [&] { return nlohmann::json{{"member", reader_.member()}, {"bytes", size}}; });
     pressure = owner.defer_resources();
     // Retiring our live decoder can itself make the complete draw fit. Probe
     // once again; subsequent failed draws wait only for external readiness.
     retry_after_quiesce = active_decoder;
    }
    if (retry_after_quiesce) continue;
    std::unique_lock lock(resource_mutex_);
    resource_changed_.wait(lock, [&] { return resource_generation_ != observed || owner.interruption(); });
    if (auto failure = owner.interruption()) std::rethrow_exception(failure);
   }
  } catch (...) {
   reader_.suspend(parent_);
   throw;
  }
  const auto bytes = input->allowance.workspace();
  input->encoded = {reinterpret_cast<std::uint8_t*>(bytes.data()), size};
 } else {
  input->standalone.resize(size);
  input->encoded = input->standalone;
 }
 return input;
}
void BenchmarkImagePublication::read(const Input& input, const BenchmarkArchive::MemberPosition* position) {
 // A failed lease may have retired the decoder. Its restoration must then
 // belong to the admitted input, including an exceptional input under a small
 // configured budget. Retire this ancestry before handing bytes to a writer.
 try {
  if (position && !reader_.seek(*position, cancellation_)) throw BenchmarkArchiveError("unaccepted archive image disappeared from its opened generation");
  reader_.read_into(input->encoded, cancellation_);
 } catch (...) {
  if (input->restored_decoder) reader_.suspend(parent_);
  throw;
 }
 if (input->restored_decoder) reader_.suspend(parent_);
}
void BenchmarkImagePublication::submit(std::uint64_t id, Input admitted_input, BenchmarkImageHeader header, CachedImageReadySink ready, Settled settled) {
 throw_if_benchmark_cancelled(cancellation_);
 if (!has_complete_image_markers(admitted_input->encoded)) throw InvalidImageError("selected archive image " + std::to_string(id) + " is not a complete JPEG or PNG");
 std::optional<detail::FileWorkQueue::Reservation> record;
 if (execution_) {
  auto& owner = execution_->execution();
  for (;;) {
   const auto observed = owner.admission_generation();
   record = execution_->files().try_reserve_record();
   if (record) break;
   // A full queue has accepted nothing. Keep input while records alone are
   // busy; under resource pressure release and replay this exact opened member.
   reader_.suspend(parent_);
   trace_benchmark_event(trace_, "benchmark.images.file_wait", [&] { return nlohmann::json{{"image_id", id}, {"member", reader_.member()}}; });
   if (owner.resource_pressure()) {
    const auto position = reader_.member_position();
    const auto size = admitted_input->encoded.size();
    admitted_input.reset();
    for (;;) {
     const auto changed = owner.admission_generation();
     if (execution_->files().has_capacity()) break;
     owner.wait_for_admission_change(changed);
    }
    admitted_input = acquire(size);
    read(admitted_input, &position);
   } else
    owner.wait_for_admission_change(observed);
  }
 }
 struct Publication {
  Input input;
  BenchmarkEncodedImage::Ptr file, fact;
 };
 auto publication = std::make_shared<Publication>();
 publication->input = std::move(admitted_input);
 auto write = [this, id, header, publication] {
  const auto& input = publication->input;
  publication->file = BenchmarkEncodedImage::publish(directory_, id, input->encoded, header, cancellation_, storage_, execution_ ? &execution_->execution() : nullptr, input->allowance);
 };
 auto completed = [this, id, header, publication, ready = std::move(ready)](std::exception_ptr failure) {
  auto input = std::exchange(publication->input, {});
  input->output.retire_descriptors();
  if (failure) std::rethrow_exception(failure);
  publication->fact = publication->file->header_only();
  // Published file backing lets synchronous consumers release an exceptional
  // input before reserving their own decoder workspace.
  auto payload = retain_encoded_ && !input->allowance.exceptional_workspace() && (!execution_ || !execution_->execution().resource_pressure())
                  ? BenchmarkEncodedImage::pooled(input->allowance, input, input->encoded, std::move(publication->file))
                  : std::move(publication->file);
  input.reset();
  if (ready) ready({id, std::pair{header.width, header.height}, execution_ != nullptr, std::move(payload)});
 };
 auto settlement = [id, header, publication, settled = std::move(settled)](std::exception_ptr failure) { settled({id, std::pair{header.width, header.height}, true, publication->fact}, failure); };
 if (record) {
  // Both operation and controller callback allocations precede acceptance.
  std::function<void(std::exception_ptr)> on_settled = std::move(settlement);
  auto physical = record->submit({.completed = std::move(completed), .work = std::move(write), .bytes = publication->input->encoded.size()});
  writes_.push(std::move(physical), std::move(on_settled));
 } else {
  std::exception_ptr failure;
  try {
   write();
   completed({});
  } catch (...) { failure = std::current_exception(); }
  settlement(failure);
 }
}
void BenchmarkImagePublication::finish() { writes_.drain(); }
void BenchmarkImagePublication::reap() { writes_.reap(); }
}  // namespace mmltk::backend::data::benchmark_internal
