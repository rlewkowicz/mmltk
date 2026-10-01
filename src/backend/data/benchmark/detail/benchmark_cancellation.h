#pragma once
#include "src/common/concurrency/cancellation_observation.h"
#include <atomic>
#include <exception>
#include <mutex>
namespace mmltk::backend::data::detail {
class CompileExecution;
}
namespace mmltk::backend::data::benchmark_internal {
// The session outlives both progress delivery and execution. Only the scoped
// binding may lend its execution target to asynchronous failure notification.
class BenchmarkCompileCancellation final {
public:
 explicit BenchmarkCompileCancellation(mmltk::common::concurrency::CancellationObservation external = {}) noexcept;
 BenchmarkCompileCancellation(const BenchmarkCompileCancellation&) = delete;
 BenchmarkCompileCancellation& operator=(const BenchmarkCompileCancellation&) = delete;
 void cancel() noexcept;
 void fail(std::exception_ptr) noexcept;
 void rethrow_failure() const;
 [[nodiscard]] bool cancelled() const noexcept;
 [[nodiscard]] int descriptor() const noexcept;
 class ExecutionBinding final {
 public:
  ~ExecutionBinding();
  ExecutionBinding(const ExecutionBinding&) = delete;
  ExecutionBinding& operator=(const ExecutionBinding&) = delete;

 private:
  friend class BenchmarkCompileCancellation;
  ExecutionBinding(BenchmarkCompileCancellation&, detail::CompileExecution&);
  BenchmarkCompileCancellation& owner_;
 };
 // Declare immediately after execution, before its source/controller owners.
 // Their joins finish before detachment; execution destruction follows it.
 [[nodiscard]] ExecutionBinding bind_execution(detail::CompileExecution&);

private:
 void cancel_locked() noexcept;
 const mmltk::common::concurrency::CancellationObservation external_;
 std::atomic<bool> internal_{false};
 mutable std::mutex mutex_;
 std::exception_ptr failure_;
 detail::CompileExecution* execution_ = nullptr;
};
}  // namespace mmltk::backend::data::benchmark_internal
