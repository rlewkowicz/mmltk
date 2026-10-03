#include "src/backend/data/detail/file_work_queue.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/system/cpu_affinity.h"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
namespace mmltk::backend::data::detail {
struct FileWorkQueue::Impl {
 static thread_local Impl* current;
 enum class State { Free, Reserved, Queued, Waiting, Running, Completing };
 struct Record {
  Operation operation;
  CompileExecution::CompletionSource completion;
  State state = State::Free;
  CompileWorkClass work_class = CompileWorkClass::Output;
  std::uint64_t observation = 0;
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
 Observer observer;
 std::uint64_t admitted = 0, completed = 0;
 explicit Impl(std::size_t capacity) : records(capacity) {
  if (!capacity) throw std::invalid_argument("file queue requires prepared records");
 }
 void observe(std::string_view event) noexcept {
  if (observer) observe(event, Observation{});
 }
 void observe(std::string_view event, Observation value) noexcept {
  if (!observer) return;
  {
   const std::lock_guard lock(mutex);
   value.records = pending;
   value.capacity = records.size();
   value.services = service_capacity;
   value.busy = busy_services;
   value.admitted = admitted;
   value.completed = completed;
   for (const auto& record : records) {
    const auto kind = static_cast<std::size_t>(record.work_class);
    if (record.state == State::Queued || record.state == State::Waiting) ++value.queued[kind];
    if (record.state == State::Running) ++value.running[kind];
    if (record.state == State::Completing) ++value.completing[kind];
   }
  }
  try {
   observer(event, value);
  } catch (...) {}
 }
 void stop_services() {
  {
   const std::lock_guard lock(mutex);
   shutdown = true;
  }
  changed.notify_all();
  services.clear();
  // Unregister while the caller retains this owner; a final weak callback must
  // never destroy its own Readiness registration on the notifier thread.
  readiness.reset();
  observe("file.service.stopped");
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
  if (readiness) readiness->interests(std::ranges::any_of(classes, [](bool selected) { return selected; }) ? CompileChange::Cpu : CompileChange::None);
 }
 void claim(std::size_t index) noexcept {
  records[index].state = State::Waiting;
  --queued[static_cast<std::size_t>(records[index].operation.service_class)];
  cursor = (index + 1) % records.size();
 }
 void finish(std::size_t index, std::exception_ptr error) noexcept {
  auto operation = std::move(records[index].operation);
  const auto observation = records[index].observation;
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
   if (observer) ++completed;
  }
  changed.notify_all();
  if (execution) execution->notify_change(CompileChange::Records);
  if (observer) observe("file.completed", {.operation = observation, .work_class = static_cast<CompileWorkClass>(service_class), .failed = bool(error)});
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
  auto* dispatcher = std::exchange(current, nullptr);
  std::exception_ptr error;
  std::optional<Observation> observation;
  std::chrono::steady_clock::time_point started;
  if (observer) {
   observation.emplace();
   observation->operation = records[index].observation;
   observation->bytes = records[index].operation.bytes;
   observation->work_class = records[index].operation.service_class;
  }
  try {
   if (grant) mmltk::common::system::set_thread_affinity({grant->cpu()});
   {
    const std::lock_guard lock(mutex);
    if (stopping) std::rethrow_exception(cancelled);
    // This short transition commits physical admission. Cancellation either
    // precedes it or joins the admitted call through its actual return.
    records[index].state = State::Running;
   }
   if (observation) observe("file.running", *observation);
   if (observer) started = std::chrono::steady_clock::now();
   if (records[index].operation.work)
    records[index].operation.work();
   else
    records[index].operation.call(records[index].operation.context);
  } catch (...) { error = std::current_exception(); }
  if (observer && started != std::chrono::steady_clock::time_point{})
   observation->duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
  // A callback retains the dispatcher and producer, but not its CPU grant.
  grant.reset();
  current = dispatcher;
  {
   const std::lock_guard lock(mutex);
   records[index].state = State::Completing;
  }
  if (observation) {
   observation->failed = bool(error);
   observe("file.physical_completed", *observation);
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
 bool dispatch_one(const CompileExecution::Completion* awaited = nullptr) {
  std::size_t selected = records.size();
  std::optional<CompileExecution::ServiceGrant> grant;
  std::exception_ptr error;
  {
   std::unique_lock lock(mutex);
   for (;;) {
    if (stopping || shutdown || (awaited && awaited->ready())) return false;
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
    if (observer && std::ranges::any_of(queued, [](std::size_t count) { return count != 0; })) {
     lock.unlock();
     observe("file.wait_cpu");
     lock.lock();
    }
    // Readiness publication and this predicate share the queue lock. A failed
    // grant leaves both the record and its demand armed until a real change.
    changed.wait(lock, [&] { return stopping || shutdown || generation != observed || (awaited && awaited->ready()); });
   }
  }
  auto* previous = std::exchange(current, this);
  if (error) {
   {
    const std::lock_guard lock(mutex);
    records[selected].state = State::Completing;
   }
   finish(selected, error);
  } else
   perform(selected, std::move(grant));
  current = previous;
  {
   const std::lock_guard lock(mutex);
   --busy_services;
   update_demand();
   ++generation;
  }
  changed.notify_all();
  return true;
 }
 void service() {
  while (dispatch_one()) {}
 }
};
thread_local FileWorkQueue::Impl* FileWorkQueue::Impl::current = nullptr;
FileWorkQueue::FileWorkQueue(CompileExecution& execution, std::size_t records, std::size_t services, Observer observer) : impl_(std::make_shared<Impl>(records)) {
 if (!services || services > execution.workers()) throw std::invalid_argument("file service capacity exceeds compile CPU budget");
 impl_->execution = &execution;
 impl_->service_capacity = services;
 impl_->observer = std::move(observer);
 impl_->demand = std::make_unique<CompileExecution::ServiceDemand>(execution);
 impl_->readiness = std::make_unique<CompileExecution::Readiness>(execution, CompileChange::None, [weak = std::weak_ptr<Impl>(impl_)](CompileExecution::Readiness::Changes) {
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
  impl_->stop_services();
  throw;
 }
 impl_->observe("file.service.started");
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
 impl_->stop_services();
}
void FileWorkQueue::wait(const CompileExecution::Completion& completion) {
 if (Impl::current != impl_.get() || !impl_->execution || completion.ready()) {
  completion.wait();
  return;
 }
 completion.subscribe([weak = std::weak_ptr<Impl>(impl_)](std::exception_ptr) {
  if (auto owner = weak.lock()) {
   {
    const std::lock_guard lock(owner->mutex);
    ++owner->generation;
   }
   owner->changed.notify_all();
  }
 });
 struct ResumeService {
  Impl& owner;
  ~ResumeService() {
   {
    const std::lock_guard lock(owner.mutex);
    ++owner.busy_services;
    owner.update_demand();
    ++owner.generation;
   }
   owner.changed.notify_all();
  }
 } resume{*impl_};
 {
  const std::lock_guard lock(impl_->mutex);
  --impl_->busy_services;
  impl_->update_demand();
  ++impl_->generation;
 }
 impl_->changed.notify_all();
 while (!completion.ready() && impl_->dispatch_one(&completion)) {}
 completion.wait();
}
FileWorkQueue::Reservation::Reservation(std::shared_ptr<Impl> owner, std::size_t index) : owner_(std::move(owner)), index_(index) {}
FileWorkQueue::Reservation::~Reservation() { release(); }
FileWorkQueue::Reservation::Reservation(Reservation&& other) noexcept : owner_(std::move(other.owner_)), index_(other.index_) {}
FileWorkQueue::Reservation& FileWorkQueue::Reservation::operator=(Reservation&& other) noexcept {
 if (this != &other) {
  release();
  owner_ = std::move(other.owner_);
  index_ = other.index_;
 }
 return *this;
}
void FileWorkQueue::Reservation::release() noexcept {
 if (!owner_) return;
 auto owner = std::move(owner_);
 {
  const std::lock_guard lock(owner->mutex);
  auto& record = owner->records[index_];
  record.state = Impl::State::Free;
  --owner->pending;
  const auto kind = static_cast<std::size_t>(record.work_class);
  if (!--owner->class_pending[kind]) owner->producers[kind].reset();
  ++owner->generation;
 }
 owner->changed.notify_all();
 if (owner->execution) owner->execution->notify_change(CompileChange::Records);
}
std::optional<FileWorkQueue::Reservation> FileWorkQueue::try_reserve_record(CompileWorkClass kind) {
 const auto service_class = static_cast<std::size_t>(kind);
 if (service_class >= impl_->class_pending.size()) throw std::invalid_argument("invalid file operation service class");
 std::unique_lock lock(impl_->mutex);
 impl_->check_admission();
 const auto position = std::ranges::find(impl_->records, Impl::State::Free, &Impl::Record::state);
 if (position == impl_->records.end()) {
  lock.unlock();
  impl_->observe("file.wait_records");
  return {};
 }
 CompileExecution::CompletionSource completion{impl_->execution};
 std::optional<CompileExecution::Producer> producer;
 if (!impl_->class_pending[service_class] && impl_->execution) producer.emplace(impl_->execution->open_class(kind));
 position->completion = std::move(completion);
 position->work_class = kind;
 position->state = Impl::State::Reserved;
 if (producer) impl_->producers[service_class] = std::move(producer);
 ++impl_->class_pending[service_class];
 ++impl_->pending;
 return Reservation(impl_, static_cast<std::size_t>(position - impl_->records.begin()));
}
CompileExecution::Completion FileWorkQueue::Reservation::submit(Operation operation) {
 if (!owner_) throw std::logic_error("file operation has no reserved record");
 if (!operation.call && !operation.work) throw std::invalid_argument("empty borrowed file operation");
 auto owner = owner_;
 std::unique_lock lock(owner->mutex);
 owner->check_admission();
 auto& record = owner->records[index_];
 if (operation.service_class != record.work_class) throw std::invalid_argument("file operation changed its reserved service class");
 auto result = record.completion.completion();
 record.operation = std::move(operation);
 record.state = Impl::State::Queued;
 if (owner->pool) {
  record.dispatch_owner = owner;
  try {
   owner->pool->enqueue_borrowed(*record.dispatch);
  } catch (...) {
   record.dispatch_owner.reset();
   operation = std::move(record.operation);
   record.state = Impl::State::Reserved;
   lock.unlock();
   throw;
  }
 }
 if (owner->observer) record.observation = ++owner->admitted;
 ++owner->queued[static_cast<std::size_t>(record.work_class)];
 owner->update_demand();
 ++owner->generation;
 owner_.reset();
 lock.unlock();
 owner->changed.notify_all();
 owner->observe("file.admitted");
 return result;
}
std::optional<CompileExecution::Completion> FileWorkQueue::try_submit(Operation operation) {
 if (!operation.call && !operation.work) throw std::invalid_argument("empty borrowed file operation");
 auto record = try_reserve_record(operation.service_class);
 if (!record) return {};
 return record->submit(std::move(operation));
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
 if (impl_->execution) impl_->execution->notify_change(CompileChange::Records);
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
