#include "src/controller/runtime/local_run.h"
#include <stdexcept>
#include <utility>
namespace mmltk::controller::direct {
LocalRun::~LocalRun() noexcept { StopAndJoin(); }
void LocalRun::Start(Job job) {
 if (!job.work) throw std::invalid_argument("local run work is unavailable");
 std::jthread retired;
 {
  std::unique_lock lock(mutex_);
  if (active_) throw contracts::BusyError("operation is already active");
  if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id()) throw contracts::BusyError("operation observer is still active");
  retired = std::move(worker_);
 }
 if (retired.joinable()) retired.join();
 std::unique_lock lock(mutex_);
 if (active_) throw contracts::BusyError("operation is already active");
 auto control = std::make_shared<RunControl>();
 const bool configure_policy = job.policy.has_value();
 std::jthread candidate{[this, control, policy = std::move(job.policy), work = std::move(job.work), failure = std::move(job.failure)](const std::stop_token stop) mutable noexcept {
  std::optional<mmltk::common::system::ScopedExecutionPolicy> placement;
  if (policy) {
   try {
    placement.emplace(*policy);
   } catch (...) { control->startup_failure = std::current_exception(); }
   control->initialized.release();
  }
  control->ready.acquire();
  if (!control->launch.load(std::memory_order_acquire)) return;
  Notification notification;
  try {
   notification = work(stop);
  } catch (...) {
   if (failure) {
    try {
     notification = failure(std::current_exception());
    } catch (...) {}
   }
  }
  Finish(control);
  if (notification) {
   try {
    notification();
   } catch (...) {}
  }
 }};
 if (configure_policy) {
  control->initialized.acquire();
  if (control->startup_failure) {
   control->ready.release();
   candidate.join();
   std::rethrow_exception(control->startup_failure);
  }
 }
 control->stop = candidate.get_stop_source();
 current_.store(control, std::memory_order_release);
 try {
  if (job.prepare) job.prepare();
 } catch (...) {
  auto expected = control;
  static_cast<void>(current_.compare_exchange_strong(expected, std::shared_ptr<RunControl>{}, std::memory_order_acq_rel, std::memory_order_acquire));
  control->ready.release();
  lock.unlock();
  candidate.join();
  throw;
 }
 active_ = true;
 worker_ = std::move(candidate);
 control->launch.store(true, std::memory_order_release);
 control->ready.release();
}
bool LocalRun::Stop() noexcept { return CurrentStopSource().request_stop(); }
std::stop_source LocalRun::CurrentStopSource() const noexcept {
 const auto control = current_.load(std::memory_order_acquire);
 return control ? control->stop : std::stop_source{std::nostopstate};
}
bool LocalRun::active() const noexcept {
 std::scoped_lock lock(mutex_);
 return active_;
}
void LocalRun::Finish(const std::shared_ptr<RunControl>& control) noexcept {
 std::scoped_lock lock(mutex_);
 auto expected = control;
 if (current_.compare_exchange_strong(expected, std::shared_ptr<RunControl>{}, std::memory_order_acq_rel, std::memory_order_acquire)) active_ = false;
}
void LocalRun::StopAndJoin() noexcept {
 static_cast<void>(Stop());
 std::jthread worker;
 {
  std::scoped_lock lock(mutex_);
  if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id()) return;
  worker = std::move(worker_);
 }
 if (worker.joinable()) worker.join();
 current_.store({}, std::memory_order_release);
}
}  // namespace mmltk::controller::direct
