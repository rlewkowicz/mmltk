#pragma once
#include "src/backend/data/detail/compile_execution.h"
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
namespace mmltk::common::concurrency { class WorkerPool; }
namespace mmltk::backend::data::detail {
// Prepared borrowed records own physical operation completion. The caller keeps
// context, descriptors and output spans alive until the returned completion.
// Compiler instances draw service grants from CompileExecution; runtime instances
// use the already-owned reader pool and never create compile class reservations.
// Unstarted compile records stay selectable by class until a dispatcher and its
// grant are both available. Completion callbacks keep dispatcher/producer custody
// after the physical call releases its CPU grant.
class FileWorkQueue final {
public:
 struct Operation {
  void* context = nullptr;
  void (*call)(void*) = nullptr;
  std::function<void(std::exception_ptr)> completed;
  std::function<void()> work;
  CompileWorkClass service_class = CompileWorkClass::Output;
 };
 FileWorkQueue(CompileExecution&, std::size_t records, std::size_t services = 1);
 FileWorkQueue(mmltk::common::concurrency::WorkerPool&, std::size_t records);
 ~FileWorkQueue();
 FileWorkQueue(const FileWorkQueue&) = delete;
 FileWorkQueue& operator=(const FileWorkQueue&) = delete;
 // A returned completion transfers custody: completed runs exactly once, after
 // physical return (or cancellation before the call), then the completion settles.
 // Empty means temporarily full. A throw means rejected. Neither outcome calls
 // completed or retains the operation, a pending record, or a new class producer;
 // the caller still owns settlement and may retry only the full outcome.
 // Execution interruption is checked before either acceptance or the full result.
 [[nodiscard]] std::optional<CompileExecution::Completion> try_submit(Operation);
 [[nodiscard]] CompileExecution::Completion submit(Operation);
 void join();
 // Settle unstarted operations and interrupt local service admission. Running
 // calls keep their context until physical return; join waits for that custody.
 void cancel() noexcept;
private:
 struct Impl;
 std::shared_ptr<Impl> impl_;
};
}
