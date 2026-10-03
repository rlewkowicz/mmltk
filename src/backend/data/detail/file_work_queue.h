#pragma once
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/compile_trace_selection.h"
#include <cstddef>
#include <cstdint>
#include <array>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
namespace mmltk::common::concurrency {
class WorkerPool;
}
namespace mmltk::backend::data::detail {
// Prepared borrowed records own physical operation completion. The caller keeps
// context, descriptors and output spans alive until the returned completion.
// Compiler instances draw service grants from CompileExecution; runtime instances
// use the already-owned reader pool and never create compile class reservations.
// Unstarted compile records stay selectable by class until a dispatcher and its
// grant are both available. Completion callbacks keep dispatcher/producer custody
// after the physical call releases its CPU grant.
class FileWorkQueue final {
 struct Impl;

public:
 struct Operation {
  void* context = nullptr;
  void (*call)(void*) = nullptr;
  std::function<void(std::exception_ptr)> completed{};
  std::function<void()> work{};
  CompileWorkClass service_class = CompileWorkClass::Output;
  std::uint64_t bytes = 0;
 };
 struct Observation {
  std::array<std::size_t, static_cast<std::size_t>(CompileWorkClass::Count)> queued{}, running{}, completing{};
  std::size_t records = 0, capacity = 0, services = 0, busy = 0;
  std::uint64_t admitted = 0, completed = 0, operation = 0, bytes = 0, duration_ns = 0;
  std::uint64_t queue_ns = 0, cpu_ns = 0, callback_ns = 0, minor_faults = 0, major_faults = 0;
  std::uint64_t total_bytes = 0, total_queue_ns = 0, total_physical_ns = 0, total_cpu_ns = 0, total_callback_ns = 0, total_minor_faults = 0, total_major_faults = 0;
  bool incomplete = false;
  CompileWorkClass work_class = CompileWorkClass::Count;
  bool failed = false;
 };
 // Optional effects only, delivered outside queue/admission locks on callers
 // and service workers. Counters and clocks stay disabled with no observer.
 using Observer = CompileTraceSink<const Observation&>;
 struct RuntimeReadiness {
  void* context = nullptr;
  // After physical completion, completed, and actual capacity return. May
  // retry try_submit. The second argument is the retired operation's context;
  // The returned completion and join retain callback and operation-context
  // custody through its return; do not wait for that completion in the callback.
  void (*available)(void*, void*) noexcept = nullptr;
 };
 FileWorkQueue(CompileExecution&, std::size_t records, std::size_t services = 1, Observer = {});
 FileWorkQueue(mmltk::common::concurrency::WorkerPool&, std::size_t records, RuntimeReadiness = {nullptr, nullptr});
 ~FileWorkQueue();
 FileWorkQueue(const FileWorkQueue&) = delete;
 FileWorkQueue& operator=(const FileWorkQueue&) = delete;
 // A returned completion transfers custody: completed runs exactly once, after
 // physical return (or cancellation before the call), then the completion settles.
 // Empty means temporarily full. A throw means rejected. Neither outcome calls
 // completed or retains the operation, a pending record, or a new class producer;
 // the caller still owns settlement and may retry only the full outcome.
 // Execution interruption is checked before either acceptance or the full result.
 // Claim bounded custody before constructing operation captures. An unused
 // reservation returns capacity without accepting or invoking an operation.
 class Reservation final {
 public:
  ~Reservation();
  Reservation(Reservation&&) noexcept;
  Reservation& operator=(Reservation&&) noexcept;
  Reservation(const Reservation&) = delete;
  Reservation& operator=(const Reservation&) = delete;
  [[nodiscard]] CompileExecution::Completion submit(Operation);

 private:
  friend class FileWorkQueue;
  Reservation(std::shared_ptr<Impl>, std::size_t);
  void release() noexcept;
  std::shared_ptr<Impl> owner_;
  std::size_t index_ = 0;
 };
 [[nodiscard]] std::optional<Reservation> try_reserve_record(CompileWorkClass = CompileWorkClass::Output);
 [[nodiscard]] std::optional<CompileExecution::Completion> try_submit(Operation);
 // Advisory only: reserves no record and shares try_submit's terminal policy.
 // Compile callers capture admission_generation before this predicate and wait
 // on that observation; capacity return and cancellation notify the execution.
 [[nodiscard]] bool has_capacity() const;
 [[nodiscard]] CompileExecution::Completion submit(Operation);
 // A synchronous consumer inside a compile file callback lends its dispatcher
 // to queued physical work while waiting, retaining the callback's own record.
 // Ordinary callers retain Completion::wait's behavior.
 void wait(const CompileExecution::Completion&);
 void join();
 // Settle unstarted operations and interrupt local service admission. Running
 // calls keep their context until physical return; join waits for that custody.
 void cancel() noexcept;

private:
 std::shared_ptr<Impl> impl_;
};
MMLTK_REFLECT_FIELDS(FileWorkQueue::Observation)
}  // namespace mmltk::backend::data::detail
