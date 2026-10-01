#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
namespace mmltk::backend::data::detail {
// Dirty updates wait for the observation deadline. Boundaries are immediate
// cumulative facts (including repair), replaceable under saturation. Lifecycle
// records are finite owner transitions; warnings remain best effort.
enum class CompileProgressDeliveryMode { Dirty, Boundary, Lifecycle, Warning };
// One compile-owned observer worker. The supplied mutex also protects the
// owner's facts. Only the worker materializes replaceable observations; a
// semantic boundary captures its exact facts immediately. No producer waits
// for observer capacity. Exhausting indispensable boundaries fails delivery.
template <class Value>
class CompileProgressDelivery final {
public:
 struct Counts {
  std::uint64_t produced = 0, materialized = 0, coalesced = 0, delivered = 0;
  std::size_t pending = 0, high_water = 0;
 };
 using Materialize = std::function<Value()>;
 using Observer = std::function<void(const Value&)>;
 using Failure = std::function<void(std::exception_ptr)>;
 using Diagnostic = std::function<void(const Counts&)>;
 CompileProgressDelivery(std::mutex& mutex, Materialize materialize, Observer observer, Failure failed = {}, Diagnostic diagnostic = {})
     : mutex_(mutex), materialize_(std::move(materialize)), observer_(std::move(observer)), failed_(std::move(failed)), diagnostic_(std::move(diagnostic)) {
  if (observer_) worker_ = std::jthread([this] { drain(); });
 }
 ~CompileProgressDelivery() {
  try {
   flush();
  } catch (...) {}
  {
   const std::lock_guard lock(mutex_);
   stopping_ = true;
  }
  changed_.notify_all();
  if (worker_.joinable()) worker_.join();
 }
 CompileProgressDelivery(const CompileProgressDelivery&) = delete;
 CompileProgressDelivery& operator=(const CompileProgressDelivery&) = delete;
 // Caller holds the owner's mutex. Materialize must not acquire it again.
 void changed(CompileProgressDeliveryMode mode = CompileProgressDeliveryMode::Dirty) {
  const bool boundary = mode != CompileProgressDeliveryMode::Dirty;
  const bool nonfatal = mode == CompileProgressDeliveryMode::Warning;
  if (!observer_) return;
  if (failure_) {
   if (!nonfatal) std::rethrow_exception(failure_);
   return;
  }
  if (diagnostic_) ++counts_.produced;
  if (boundary && nonfatal && size_ == pending_.size()) {
   if (diagnostic_) ++counts_.coalesced;
   return;
  }
  const auto sequence = ++submitted_;
  if (diagnostic_ && dirty_) ++counts_.coalesced;
  if (boundary) {
   if (size_ == pending_.size()) {
    // Cumulative repair/source facts and warnings cannot consume lifecycle capacity.
    for (std::size_t offset = 0; offset < size_; ++offset) {
     if (pending_[(read_ + offset) % pending_.size()]->mode == CompileProgressDeliveryMode::Lifecycle) continue;
     for (std::size_t next = offset + 1; next < size_; ++next) pending_[(read_ + next - 1) % pending_.size()] = std::move(pending_[(read_ + next) % pending_.size()]);
     pending_[(read_ + size_ - 1) % pending_.size()].reset();
     --size_;
     if (diagnostic_) ++counts_.coalesced;
     break;
    }
   }
   if (size_ == pending_.size()) {
    failure_ = std::make_exception_ptr(std::runtime_error("compile progress lifecycle capacity exhausted"));
    changed_.notify_all();
    std::rethrow_exception(failure_);
   }
   try {
    pending_[(read_ + size_) % pending_.size()].emplace(Item{materialize_(), sequence, mode});
   } catch (...) {
    if (nonfatal) {
     --submitted_;
     return;
    }
    failure_ = std::current_exception();
    changed_.notify_all();
    throw;
   }
   ++size_;
   dirty_ = false;
   if (diagnostic_) { ++counts_.materialized; }
  } else if (!dirty_) {
   dirty_ = true;
   deadline_ = Clock::now() + std::chrono::milliseconds{100};
  }
  if (diagnostic_) counts_.high_water = std::max(counts_.high_water, size_ + static_cast<std::size_t>(dirty_));
  changed_.notify_all();
 }
 // This is an explicit lifecycle barrier, never a task's progress callback.
 void flush() {
  if (!observer_) return;
  std::unique_lock lock(mutex_);
  const auto through = submitted_;
  flush_through_ = through;
  changed_.notify_all();
  changed_.wait(lock, [&] { return failure_ ? failure_notified_ : delivered_ >= through; });
  if (failure_) std::rethrow_exception(failure_);
 }

private:
 using Clock = std::chrono::steady_clock;
 struct Item {
  Value value;
  std::uint64_t sequence;
  CompileProgressDeliveryMode mode;
 };
 void drain() noexcept {
  std::unique_lock lock(mutex_);
  for (;;) {
   if (failure_) {
    const auto failure = failure_;
    lock.unlock();
    if (failed_) {
     try {
      failed_(failure);
     } catch (...) {}
    }
    lock.lock();
    failure_notified_ = true;
    changed_.notify_all();
    return;
   }
   if (!size_ && !dirty_) {
    if (stopping_) return;
    changed_.wait(lock, [&] { return stopping_ || failure_ || size_ || dirty_; });
    continue;
   }
   if (!size_ && dirty_ && flush_through_ <= delivered_) {
    if (changed_.wait_until(lock, deadline_, [&] { return failure_ || size_ || flush_through_ > delivered_; })) continue;
   }
   std::optional<Item> item;
   try {
    if (size_) {
     item = std::move(pending_[read_]);
     pending_[read_].reset();
     read_ = (read_ + 1) % pending_.size();
     --size_;
    } else {
     item.emplace(Item{materialize_(), submitted_, CompileProgressDeliveryMode::Dirty});
     dirty_ = false;
     if (diagnostic_) ++counts_.materialized;
    }
   } catch (...) {
    failure_ = std::current_exception();
    continue;
   }
   lock.unlock();
   std::exception_ptr failure;
   try {
    observer_(item->value);
   } catch (...) {
    if (item->mode != CompileProgressDeliveryMode::Warning) failure = std::current_exception();
   }
   lock.lock();
   delivered_ = item->sequence;
   if (failure) failure_ = failure;
   if (diagnostic_) {
    ++counts_.delivered;
    counts_.pending = size_ + static_cast<std::size_t>(dirty_);
    const auto counts = counts_;
    lock.unlock();
    try {
     diagnostic_(counts);
    } catch (...) {}
    lock.lock();
   }
   changed_.notify_all();
  }
 }
 std::mutex& mutex_;
 Materialize materialize_;
 Observer observer_;
 Failure failed_;
 Diagnostic diagnostic_;
 std::condition_variable changed_;
 // Both compile owners flush between runs. Their ordered lifecycle is under
 // ten records; the remaining capacity retains immediate repair/warning detail
 // until saturation, then cumulative state replaces older optional records.
 std::array<std::optional<Item>, 64> pending_{};
 std::size_t read_ = 0, size_ = 0;
 std::uint64_t submitted_ = 0, delivered_ = 0, flush_through_ = 0;
 Counts counts_{};
 Clock::time_point deadline_{};
 bool dirty_ = false, stopping_ = false, failure_notified_ = false;
 std::exception_ptr failure_;
 std::jthread worker_;
};
}  // namespace mmltk::backend::data::detail
