
#pragma once

#include <spdlog/details/circular_q.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace spdlog {
namespace details {

template <typename T>
class mpmc_blocking_queue {
   public:
    using item_type = T;
    explicit mpmc_blocking_queue(size_t max_items) : q_(max_items) {}

    void enqueue(T&& item) {
        enqueue_impl<notify_inside_lock_>(std::move(item));
        notify_push_after_unlock();
    }

    void enqueue_nowait(T&& item) {
        enqueue_nowait_impl<notify_inside_lock_>(std::move(item));
        notify_push_after_unlock();
    }

    void enqueue_if_have_room(T&& item) {
        const bool pushed = enqueue_if_have_room_impl<notify_inside_lock_>(std::move(item));
        if (pushed) {
            notify_push_after_unlock();
        } else {
            ++discard_counter_;
        }
    }

    bool dequeue_for(T& popped_item, std::chrono::milliseconds wait_duration) {
        if (!dequeue_for_impl<notify_inside_lock_>(popped_item, wait_duration)) {
            return false;
        }
        notify_pop_after_unlock();
        return true;
    }

    void dequeue(T& popped_item) {
        dequeue_impl<notify_inside_lock_>(popped_item);
        notify_pop_after_unlock();
    }

    size_t overrun_counter() {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return q_.overrun_counter();
    }

    size_t discard_counter() {
        return discard_counter_.load(std::memory_order_relaxed);
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return q_.size();
    }

    void reset_overrun_counter() {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        q_.reset_overrun_counter();
    }

    void reset_discard_counter() {
        discard_counter_.store(0, std::memory_order_relaxed);
    }

   private:
#ifdef __MINGW32__
    static constexpr bool notify_inside_lock_ = true;
#else
    static constexpr bool notify_inside_lock_ = false;
#endif

    void notify_push_after_unlock() {
        if (!notify_inside_lock_) {
            push_cv_.notify_one();
        }
    }

    void notify_pop_after_unlock() {
        if (!notify_inside_lock_) {
            pop_cv_.notify_one();
        }
    }

    template <bool NotifyInsideLock>
    void notify_push_locked() {
        if (NotifyInsideLock) {
            push_cv_.notify_one();
        }
    }

    template <bool NotifyInsideLock>
    void notify_pop_locked() {
        if (NotifyInsideLock) {
            pop_cv_.notify_one();
        }
    }

    template <bool NotifyInsideLock, typename WaitFn>
    void enqueue_locked(T&& item, WaitFn&& wait_for_room) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        wait_for_room(lock);
        q_.push_back(std::move(item));
        notify_push_locked<NotifyInsideLock>();
    }

    template <bool NotifyInsideLock, typename WaitFn>
    bool dequeue_locked(T& popped_item, WaitFn&& wait_for_item) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (!wait_for_item(lock)) {
            return false;
        }
        popped_item = std::move(q_.front());
        q_.pop_front();
        notify_pop_locked<NotifyInsideLock>();
        return true;
    }

    template <bool NotifyInsideLock>
    void enqueue_impl(T&& item) {
        enqueue_locked<NotifyInsideLock>(std::move(item), [this](std::unique_lock<std::mutex>& lock) {
            pop_cv_.wait(lock, [this] { return !this->q_.full(); });
        });
    }

    template <bool NotifyInsideLock>
    void enqueue_nowait_impl(T&& item) {
        enqueue_locked<NotifyInsideLock>(std::move(item), [](std::unique_lock<std::mutex>&) {});
    }

    template <bool NotifyInsideLock>
    bool enqueue_if_have_room_impl(T&& item) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (q_.full()) {
            return false;
        }
        q_.push_back(std::move(item));
        notify_push_locked<NotifyInsideLock>();
        return true;
    }

    template <bool NotifyInsideLock>
    bool dequeue_for_impl(T& popped_item, std::chrono::milliseconds wait_duration) {
        return dequeue_locked<NotifyInsideLock>(popped_item, [this, wait_duration](std::unique_lock<std::mutex>& lock) {
            return push_cv_.wait_for(lock, wait_duration, [this] { return !this->q_.empty(); });
        });
    }

    template <bool NotifyInsideLock>
    void dequeue_impl(T& popped_item) {
        (void)dequeue_locked<NotifyInsideLock>(popped_item, [this](std::unique_lock<std::mutex>& lock) {
            push_cv_.wait(lock, [this] { return !this->q_.empty(); });
            return true;
        });
    }

    std::mutex queue_mutex_;
    std::condition_variable push_cv_;
    std::condition_variable pop_cv_;
    spdlog::details::circular_q<T> q_;
    std::atomic<size_t> discard_counter_{0};
};
}  
}  
