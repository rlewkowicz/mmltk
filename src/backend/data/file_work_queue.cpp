#include "src/backend/data/detail/file_work_queue.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/system/cpu_affinity.h"
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <stop_token>
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
 };
 std::mutex mutex;
 std::condition_variable changed;
 std::vector<Record> records;
 std::vector<std::jthread> services;
 CompileExecution* execution = nullptr;
 mmltk::common::concurrency::WorkerPool* pool = nullptr;
 std::optional<CompileExecution::Producer> producer;
 std::stop_source stop;
 const std::exception_ptr cancelled = std::make_exception_ptr(std::runtime_error("file operation cancelled before admission"));
 std::size_t pending = 0, cursor = 0;
 bool stopping = false, shutdown = false;
 explicit Impl(std::size_t capacity) : records(capacity) { if (!capacity) throw std::invalid_argument("file queue requires prepared records"); }
 void finish(std::size_t index, std::exception_ptr error) noexcept {
  auto operation = std::move(records[index].operation);
  auto completion = records[index].completion;
  // Only the record's completion owner gets here. An admitted syscall has
  // returned, and all callbacks finish outside both queue and admission locks.
  if (operation.completed) try { operation.completed(error); } catch (...) { if (!error) error = std::current_exception(); }
  operation = {};
  completion.settle(error);
  {
   const std::lock_guard lock(mutex);
   records[index].state = State::Free;
   if (!--pending) producer.reset();
  }
  changed.notify_all();
  if (execution) execution->notify_admission_change();
 }
 void perform(std::size_t index) noexcept {
  std::exception_ptr error;
  try {
   std::optional<CompileExecution::ServiceGrant> grant;
   if (execution) {
    grant.emplace(execution->service_grant(CompileWorkClass::Output, stop.get_token()));
    mmltk::common::system::set_thread_affinity({grant->cpu()});
   }
   {
    const std::lock_guard lock(mutex);
    if (stopping) std::rethrow_exception(cancelled);
    // This short transition commits physical admission. Cancellation either
    // precedes it or joins the admitted call through its actual return.
    records[index].state = State::Running;
   }
   if (records[index].operation.work) records[index].operation.work();
   else records[index].operation.call(records[index].operation.context);
  } catch (...) { error = std::current_exception(); }
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
   records[index].state = State::Waiting;
  }
  perform(index);
 }
 void service() {
  for (;;) {
   std::size_t selected = records.size();
   {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] {
     if (stopping || shutdown) return true;
     for (std::size_t turn = 0; turn < records.size(); ++turn) {
      const auto candidate = (cursor + turn) % records.size();
      if (records[candidate].state == State::Queued) {
       selected = candidate; records[candidate].state = State::Waiting;
       cursor = (candidate + 1) % records.size(); return true;
      }
     }
     return false;
    });
    if (selected == records.size()) return;
   }
   perform(selected);
  }
 }
};
FileWorkQueue::FileWorkQueue(CompileExecution& execution, std::size_t records, std::size_t services) : impl_(std::make_shared<Impl>(records)) {
 if (!services || services > execution.workers()) throw std::invalid_argument("file service capacity exceeds compile CPU budget");
 impl_->execution = &execution;
 try { for (std::size_t i = 0; i < services; ++i) impl_->services.emplace_back([owner = impl_] { owner->service(); }); }
 catch (...) { { const std::lock_guard lock(impl_->mutex); impl_->shutdown = true; } impl_->changed.notify_all(); impl_->services.clear(); throw; }
}
FileWorkQueue::FileWorkQueue(mmltk::common::concurrency::WorkerPool& pool, std::size_t records) : impl_(std::make_shared<Impl>(records)) { impl_->pool = &pool; }
FileWorkQueue::~FileWorkQueue() {
 cancel(); join();
 { const std::lock_guard lock(impl_->mutex); impl_->shutdown = true; }
 impl_->changed.notify_all(); impl_->services.clear();
}
std::optional<CompileExecution::Completion> FileWorkQueue::try_submit(Operation operation) {
 if (!operation.call && !operation.work) throw std::invalid_argument("empty borrowed file operation");
 std::unique_lock lock(impl_->mutex);
 if (impl_->stopping) throw std::runtime_error("file queue admission after stop");
 const auto position = std::ranges::find(impl_->records, Impl::State::Free, &Impl::Record::state);
 if (position == impl_->records.end()) return {};
 // Prepare every fallible owner before publishing the record. In particular,
 // completion allocation cannot leave an otherwise empty Output class live.
 CompileExecution::CompletionSource completion{impl_->execution};
 std::optional<CompileExecution::Producer> producer;
 if (!impl_->pending && impl_->execution) producer.emplace(impl_->execution->open_class(CompileWorkClass::Output));
 position->completion = std::move(completion);
 auto result = position->completion.completion();
 position->operation = std::move(operation); position->state = Impl::State::Queued;
 const auto index = static_cast<std::size_t>(position - impl_->records.begin());
 if (impl_->pool) {
  position->dispatch_owner = impl_;
  try {
   impl_->pool->enqueue_borrowed(&*position, index, [](void* context, std::size_t record) {
    auto owner = std::move(static_cast<Impl::Record*>(context)->dispatch_owner);
    owner->dispatch(record);
   });
  } catch (...) {
   position->dispatch_owner.reset(); operation = std::move(position->operation); position->state = Impl::State::Free;
   lock.unlock(); throw;
  }
 }
 if (producer) impl_->producer = std::move(producer);
 ++impl_->pending;
 lock.unlock(); impl_->changed.notify_one(); return result;
}
CompileExecution::Completion FileWorkQueue::submit(Operation operation) {
 if (impl_->execution && impl_->execution->on_lane()) throw std::logic_error("blocking file admission inside a compile lane");
 for (;;) {
  if (auto completion = try_submit(operation)) return std::move(*completion);
  std::unique_lock lock(impl_->mutex);
  impl_->changed.wait(lock,[&] { return impl_->stopping || std::ranges::any_of(impl_->records,[](const Impl::Record& record) { return record.state == Impl::State::Free; }); });
 }
}
void FileWorkQueue::join() { std::unique_lock lock(impl_->mutex); impl_->changed.wait(lock, [&] { return !impl_->pending; }); }
void FileWorkQueue::cancel() noexcept {
 { const std::lock_guard lock(impl_->mutex); impl_->stopping = true; }
 // Never request stop while holding the queue mutex: its callback wakes the
 // execution owner. Waiting records settle on their service thread; queued
 // records can settle here even while every service is in a physical call.
 impl_->stop.request_stop();
 impl_->changed.notify_all();
 for (std::size_t index = 0; index < impl_->records.size(); ++index) {
  {
   const std::lock_guard lock(impl_->mutex);
   auto& record = impl_->records[index];
   if (record.state != Impl::State::Queued) continue;
   record.state = Impl::State::Completing;
  }
  impl_->finish(index, impl_->cancelled);
 }
}
}
