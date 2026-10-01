#include "src/backend/data/benchmark/detail/benchmark_cancellation.h"
#include "src/backend/data/detail/compile_execution.h"
#include <stdexcept>
#include <utility>
namespace mmltk::backend::data::benchmark_internal {
BenchmarkCompileCancellation::BenchmarkCompileCancellation(mmltk::common::concurrency::CancellationObservation external) noexcept : external_(external) {}
void BenchmarkCompileCancellation::cancel_locked() noexcept {
 internal_.store(true, std::memory_order_relaxed);
 // Notification only takes the execution admission lock; it never joins work
 // or calls back synchronously into this owner. Detachment uses this same lock.
 if (execution_) execution_->notify_admission_change();
}
void BenchmarkCompileCancellation::cancel() noexcept {
 const std::lock_guard lock(mutex_);
 cancel_locked();
}
void BenchmarkCompileCancellation::fail(std::exception_ptr error) noexcept {
 const std::lock_guard lock(mutex_);
 if (!failure_) failure_ = std::move(error);
 cancel_locked();
}
void BenchmarkCompileCancellation::rethrow_failure() const {
 std::exception_ptr error;
 {
  const std::lock_guard lock(mutex_);
  error = failure_;
 }
 if (error) std::rethrow_exception(error);
}
bool BenchmarkCompileCancellation::cancelled() const noexcept {
 // Execution checks this while holding its admission lock. Never acquire the
 // binding mutex here: notifications take those locks in the opposite order.
 return external_.requested() || internal_.load(std::memory_order_relaxed);
}
int BenchmarkCompileCancellation::descriptor() const noexcept { return external_.descriptor(); }
BenchmarkCompileCancellation::ExecutionBinding BenchmarkCompileCancellation::bind_execution(detail::CompileExecution& execution) { return ExecutionBinding(*this, execution); }
BenchmarkCompileCancellation::ExecutionBinding::ExecutionBinding(BenchmarkCompileCancellation& owner, detail::CompileExecution& execution) : owner_(owner) {
 const std::lock_guard lock(owner_.mutex_);
 if (owner_.execution_) throw std::logic_error("benchmark cancellation already has an execution binding");
 owner_.execution_ = &execution;
 if (owner_.cancelled()) execution.notify_admission_change();
}
BenchmarkCompileCancellation::ExecutionBinding::~ExecutionBinding() {
 // Wait for an in-flight notification, then withdraw the borrowed target before
 // execution can begin destruction. No execution/producer/observer join here.
 const std::lock_guard lock(owner_.mutex_);
 owner_.execution_ = nullptr;
}
}  // namespace mmltk::backend::data::benchmark_internal
