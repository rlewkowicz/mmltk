#pragma once
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <stdexcept>
#include <stop_token>
#include <utility>
namespace mmltk::testsupport {
inline void release_test_promise(std::promise<void>& promise) noexcept {
 try {
  promise.set_value();
 } catch (const std::future_error&) {}
}
// The future stays with its domain owner so a deadline unwinds through that
// scenario's release/stop guard before any joining future is destroyed.
template <class T>
T await_test_future(std::future<T>& future, std::string_view name, std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
 if (future.wait_for(timeout) != std::future_status::ready) throw std::runtime_error(std::string(name) + ": settlement deadline expired");
 return future.get();
}
// Promise futures never join on destruction; their lifetime remains local.
template <class T>
T await_test_promise(std::promise<T>& promise, std::string_view name, std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
 auto future = promise.get_future();
 return await_test_future(future, name, timeout);
}
// Declare after the asynchronous owner/future, while everything captured by
// cleanup is still alive. Cleanup releases dependencies before that owner joins.
template <class Cleanup>
class ScopedTestCleanup final {
public:
 explicit ScopedTestCleanup(Cleanup cleanup) : cleanup_(std::move(cleanup)) {}
 ~ScopedTestCleanup() { cleanup_(); }
 ScopedTestCleanup(const ScopedTestCleanup&) = delete;
 ScopedTestCleanup& operator=(const ScopedTestCleanup&) = delete;

private:
 Cleanup cleanup_;
};
// Stop-aware, reusable admission gate; TestGate instead models one engagement.
class StopGate final {
public:
 void Release() {
  {
   std::scoped_lock lock(mutex_);
   released_ = true;
  }
  condition_.notify_all();
 }
 void Reset() {
  std::scoped_lock lock(mutex_);
  released_ = false;
 }
 [[nodiscard]] bool Wait(std::stop_token stop) {
  std::unique_lock lock(mutex_);
  return condition_.wait(lock, stop, [this] { return released_; });
 }

private:
 std::mutex mutex_;
 std::condition_variable_any condition_;
 bool released_ = false;
};
// One object is one named engagement. Copies of the receipt retain its storage,
// never reset a previous engagement, and never assert from a worker thread.
class TestGate final {
 struct State {
  explicit State(std::string gate_name) : name(std::move(gate_name)) {}
  std::string name;
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t entered = 0;
  std::size_t settled = 0;
  bool released = false;
 };

public:
 class Receipt final {
 public:
  void ArriveAndWait() const {
   std::unique_lock lock(state_->mutex);
   ++state_->entered;
   state_->changed.notify_all();
   state_->changed.wait(lock, [&] { return state_->released; });
   ++state_->settled;
   state_->changed.notify_all();
  }

 private:
  friend class TestGate;
  explicit Receipt(std::shared_ptr<State> state) : state_(std::move(state)) {}
  std::shared_ptr<State> state_;
 };
 explicit TestGate(std::string name) : state_(std::make_shared<State>(std::move(name))) {}
 ~TestGate() { Release(); }
 TestGate(const TestGate&) = delete;
 TestGate& operator=(const TestGate&) = delete;
 [[nodiscard]] Receipt receipt() const { return Receipt{state_}; }
 [[nodiscard]] const std::string& name() const noexcept { return state_->name; }
 [[nodiscard]] bool WaitEntered(std::chrono::milliseconds timeout, std::size_t count = 1) const {
  std::unique_lock lock(state_->mutex);
  return state_->changed.wait_for(lock, timeout, [&] { return state_->entered >= count; });
 }
 [[nodiscard]] bool WaitSettled(std::chrono::milliseconds timeout, std::size_t count = 1) const {
  std::unique_lock lock(state_->mutex);
  return state_->changed.wait_for(lock, timeout, [&] { return state_->settled >= count; });
 }
 void Release() const noexcept {
  {
   std::scoped_lock lock(state_->mutex);
   state_->released = true;
  }
  state_->changed.notify_all();
 }

private:
 std::shared_ptr<State> state_;
};
}  // namespace mmltk::testsupport
