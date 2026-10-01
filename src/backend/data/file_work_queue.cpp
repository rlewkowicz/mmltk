#include "src/backend/data/detail/file_work_queue.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/system/cpu_affinity.h"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
namespace mmltk::backend::data::detail {
struct FileWorkQueue::Impl {
 enum class State { Free, Queued, Waiting, Running, Completing };
 struct Record {
  Operation operation;
  CompileExecution::CompletionSource completion;
  State state = State::Free;
  // A borrowed runtime dispatch may still be queued after local cancellation.
  // This prepared record keeps only its scheduler custody alive until dispatch;
  // the cancelled operation and its context have already settled independently.
  std::shared_ptr<Impl> dispatch_owner;
  std::unique_ptr<mmltk::common::concurrency::WorkerPool::BorrowedTask> dispatch;
 };
 std::mutex mutex;
 std::condition_variable changed;
 std::vector<Record> records;
 std::vector<std::jthread> services;
 CompileExecution* execution = nullptr;
 mmltk::common::concurrency::WorkerPool* pool = nullptr;
 RuntimeReadiness runtime_readiness;
 std::array<std::optional<CompileExecution::Producer>, static_cast<std::size_t>(CompileWorkClass::Count)> producers;
 std::array<std::size_t, static_cast<std::size_t>(CompileWorkClass::Count)> class_pending{};
 std::array<std::size_t, static_cast<std::size_t>(CompileWorkClass::Count)> queued{};
 std::unique_ptr<CompileExecution::ServiceDemand> demand;
 std::unique_ptr<CompileExecution::Readiness> readiness;
 const std::exception_ptr cancelled = std::make_exception_ptr(std::runtime_error("file operation cancelled before admission"));
 std::size_t pending = 0, cursor = 0, notifying = 0;
 std::size_t service_capacity = 0, busy_services = 0;
 std::uint64_t generation = 0;
 bool stopping = false, shutdown = false;
 explicit Impl(std::size_t capacity) : records(capacity) {
  if (!capacity) throw std::invalid_argument("file queue requires prepared records");
 }
 void check_admission() const {
  if (stopping) throw std::runtime_error("file queue admission after stop");
  if (execution)
   if (const auto error = execution->interruption()) std::rethrow_exception(error);
 }
 // Lock order is queue then execution admission. Readiness delivery takes only
 // the queue lock, after the execution owner has released its admission lock.
 void update_demand() noexcept {
  if (!demand) return;
  CompileExecution::ServiceDemand::Classes classes{};
  if (!stopping && !shutdown && busy_services < service_capacity)
   for (std::size_t kind = 0; kind < classes.size(); ++kind) classes[kind] = queued[kind] != 0;
  demand->update(classes);
 }
 void claim(std::size_t index) noexcept {
  records[index].state = State::Waiting;
  --queued[static_cast<std::size_t>(records[index].operation.service_class)];
  cursor = (index + 1) % records.size();
 }
 void finish(std::size_t index, std::exception_ptr error) noexcept {
  auto operation = std::move(records[index].operation);
  auto completion = records[index].completion;
  // Only the record's completion owner gets here. An admitted syscall has
  // returned, and all callbacks finish outside both queue and admission locks.
  if (operation.completed) try {
    operation.completed(error);
   } catch (...) {
    if (!error) error = std::current_exception();
   }
  const auto service_class = static_cast<std::size_t>(operation.service_class);
  auto* const context = operation.context;
  operation = {};
  if (!runtime_readiness.available) completion.settle(error);
  {
   const std::lock_guard lock(mutex);
   records[index].state = State::Free;
   --pending;
   if (!--class_pending[service_class]) producers[service_class].reset();
   if (runtime_readiness.available) ++notifying;
   ++generation;
  }
  changed.notify_all();
  if (execution) execution->notify_admission_change();
  if (runtime_readiness.available) {
   runtime_readiness.available(runtime_readiness.context, context);
   completion.settle(error);
   {
    const std::lock_guard lock(mutex);
    --notifying;
   }
   changed.notify_all();
  }
 }
 void perform(std::size_t index, std::optional<CompileExecution::ServiceGrant> grant = {}) noexcept {
  std::exception_ptr error;
  try {
   if (grant) mmltk::common::system::set_thread_affinity({grant->cpu()});
   {
    const std::lock_guard lock(mutex);
    if (stopping) std::rethrow_exception(cancelled);
    // This short transition commits physical admission. Cancellation either
    // precedes it or joins the admitted call through its actual return.
    records[index].state = State::Running;
   }
   if (records[index].operation.work)
    records[index].operation.work();
   else
    records[index].operation.call(records[index].operation.context);
  } catch (...) { error = std::current_exception(); }
  // A callback retains the dispatcher and producer, but not its CPU grant.
  grant.reset();
  {
   const std::lock_guard lock(mutex);
   records[index].state = State::Completing;
  }
  finish(index, error);
 }
 void dispatch(std::size_t index) noexcept {
  {
   const std::lock_guard lock(mutex);
   if (records[index].state != State::Queued) return;
   claim(index);
  }
  perform(index);
 }
 void service() {
  for (;;) {
   std::size_t selected = records.size();
   std::optional<CompileExecution::ServiceGrant> grant;
   std::exception_ptr error;
   {
    std::unique_lock lock(mutex);
    for (;;) {
     if (stopping || shutdown) return;
     const auto observed = generation;
     error = {};
     try {
      grant = demand->try_grant();
     } catch (...) { error = std::current_exception(); }
     for (std::size_t turn = 0; turn < records.size(); ++turn) {
      const auto candidate = (cursor + turn) % records.size();
      const auto& record = records[candidate];
      if (record.state == State::Queued && (error || (grant && record.operation.service_class == grant->work_class()))) {
       selected = candidate;
       claim(selected);
       ++busy_services;
       update_demand();
       break;
      }
     }
     if (selected != records.size()) break;
     // Readiness publication and this predicate share the queue lock. A failed
     // grant leaves both the record and its demand armed until a real change.
     changed.wait(lock, [&] { return stopping || shutdown || generation != observed; });
    }
   }
   if (error) {
    {
     const std::lock_guard lock(mutex);
     records[selected].state = State::Completing;
    }
    finish(selected, error);
   } else
    perform(selected, std::move(grant));
   {
    const std::lock_guard lock(mutex);
    --busy_services;
    update_demand();
    ++generation;
   }
   changed.notify_all();
  }
 }
};
FileWorkQueue::FileWorkQueue(CompileExecution& execution, std::size_t records, std::size_t services) : impl_(std::make_shared<Impl>(records)) {
 if (!services || services > execution.workers()) throw std::invalid_argument("file service capacity exceeds compile CPU budget");
 impl_->execution = &execution;
 impl_->service_capacity = services;
 impl_->demand = std::make_unique<CompileExecution::ServiceDemand>(execution);
 impl_->readiness = std::make_unique<CompileExecution::Readiness>(execution, [weak = std::weak_ptr<Impl>(impl_)](CompileExecution::Readiness::Changes) {
  if (auto owner = weak.lock()) {
   {
    const std::lock_guard lock(owner->mutex);
    ++owner->generation;
   }
   owner->changed.notify_all();
  }
 });
 try {
  for (std::size_t i = 0; i < services; ++i) impl_->services.emplace_back([owner = impl_] { owner->service(); });
 } catch (...) {
  {
   const std::lock_guard lock(impl_->mutex);
   impl_->shutdown = true;
  }
  impl_->changed.notify_all();
  impl_->services.clear();
  impl_->readiness.reset();
  throw;
 }
}
FileWorkQueue::FileWorkQueue(mmltk::common::concurrency::WorkerPool& pool, std::size_t records, RuntimeReadiness readiness) : impl_(std::make_shared<Impl>(records)) {
 impl_->pool = &pool;
 impl_->runtime_readiness = readiness;
 for (std::size_t index = 0; index < records; ++index) {
  auto& record = impl_->records[index];
  record.dispatch = std::make_unique<mmltk::common::concurrency::WorkerPool::BorrowedTask>(&record, index, [](void* context, std::size_t selected) {
   auto owner = std::move(static_cast<Impl::Record*>(context)->dispatch_owner);
   owner->dispatch(selected);
  });
 }
}
FileWorkQueue::~FileWorkQueue() {
 cancel();
 join();
 {
  const std::lock_guard lock(impl_->mutex);
  impl_->shutdown = true;
 }
 impl_->changed.notify_all();
 impl_->services.clear();
 // Unregister while this owner is still retained; a final weak callback must
 // never destroy its own Readiness registration on the notifier thread.
 impl_->readiness.reset();
}
std::optional<CompileExecution::Completion> FileWorkQueue::try_submit(Operation operation) {
 if (!operation.call && !operation.work) throw std::invalid_argument("empty borrowed file operation");
 const auto service_class = static_cast<std::size_t>(operation.service_class);
 if (service_class >= impl_->class_pending.size()) throw std::invalid_argument("invalid file operation service class");
 std::unique_lock lock(impl_->mutex);
 impl_->check_admission();
 const auto position = std::ranges::find(impl_->records, Impl::State::Free, &Impl::Record::state);
 if (position == impl_->records.end()) return {};
 // Prepare every fallible owner before publishing the record. In particular,
 // completion allocation cannot leave an otherwise empty service class live.
 CompileExecution::CompletionSource completion{impl_->execution};
 std::optional<CompileExecution::Producer> producer;
 if (!impl_->class_pending[service_class] && impl_->execution) producer.emplace(impl_->execution->open_class(operation.service_class));
 position->completion = std::move(completion);
 auto result = position->completion.completion();
 position->operation = std::move(operation);
 position->state = Impl::State::Queued;
 if (impl_->pool) {
  position->dispatch_owner = impl_;
  try {
   impl_->pool->enqueue_borrowed(*position->dispatch);
  } catch (...) {
   position->dispatch_owner.reset();
   operation = std::move(position->operation);
   position->state = Impl::State::Free;
   lock.unlock();
   throw;
  }
 }
 if (producer) impl_->producers[service_class] = std::move(producer);
 ++impl_->class_pending[service_class];
 ++impl_->pending;
 ++impl_->queued[service_class];
 impl_->update_demand();
 ++impl_->generation;
 lock.unlock();
 impl_->changed.notify_all();
 return result;
}
bool FileWorkQueue::has_capacity() const {
 const std::lock_guard lock(impl_->mutex);
 impl_->check_admission();
 return impl_->pending < impl_->records.size();
}
CompileExecution::Completion FileWorkQueue::submit(Operation operation) {
 if (impl_->execution && impl_->execution->on_lane()) throw std::logic_error("blocking file admission inside a compile lane");
 for (;;) {
  if (auto completion = try_submit(operation)) return std::move(*completion);
  std::unique_lock lock(impl_->mutex);
  impl_->changed.wait(lock, [&] {
   return impl_->stopping || (impl_->execution && impl_->execution->interruption()) ||
          std::ranges::any_of(impl_->records, [](const Impl::Record& record) { return record.state == Impl::State::Free; });
  });
 }
}
void FileWorkQueue::join() {
 std::unique_lock lock(impl_->mutex);
 impl_->changed.wait(lock, [&] { return !impl_->pending && !impl_->notifying; });
}
void FileWorkQueue::cancel() noexcept {
 {
  const std::lock_guard lock(impl_->mutex);
  impl_->stopping = true;
  impl_->update_demand();
 }
 // Claimed records settle on their service thread; queued records can settle
 // here even while every service is in a physical call. Local stop withdraws
 // only this queue's demand and never interrupts the execution owner.
 impl_->changed.notify_all();
 if (impl_->execution) impl_->execution->notify_admission_change();
 for (std::size_t index = 0; index < impl_->records.size(); ++index) {
  {
   const std::lock_guard lock(impl_->mutex);
   auto& record = impl_->records[index];
   if (record.state != Impl::State::Queued) continue;
   record.state = Impl::State::Completing;
   --impl_->queued[static_cast<std::size_t>(record.operation.service_class)];
  }
  impl_->finish(index, impl_->cancelled);
 }
}
}  // namespace mmltk::backend::data::detail
