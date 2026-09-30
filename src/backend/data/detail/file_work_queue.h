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
class FileWorkQueue final {
public:
 struct Operation {
  void* context = nullptr;
  void (*call)(void*) = nullptr;
  std::function<void(std::exception_ptr)> completed;
 };
 FileWorkQueue(CompileExecution&, std::size_t records, std::size_t services = 1);
 FileWorkQueue(mmltk::common::concurrency::WorkerPool&, std::size_t records);
 ~FileWorkQueue();
 FileWorkQueue(const FileWorkQueue&) = delete;
 FileWorkQueue& operator=(const FileWorkQueue&) = delete;
 [[nodiscard]] std::optional<CompileExecution::Completion> try_submit(Operation);
 void join();
 // Settle unstarted operations and interrupt local service admission. Running
 // calls keep their context until physical return; join waits for that custody.
 void cancel() noexcept;
private:
 struct Impl;
 std::shared_ptr<Impl> impl_;
};
}
