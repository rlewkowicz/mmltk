#include "mmltk/runtime/async_runtime.h"

namespace mmltk::runtime {

TaskCancellation::TaskCancellation() : state_(std::make_shared<State>()) {}

TaskCancellation::TaskCancellation(std::shared_ptr<State> state) : state_(std::move(state)) {}

void TaskCancellation::cancel() const noexcept {
    if (state_) {
        state_->cancelled.store(true, std::memory_order_relaxed);
    }
}

bool TaskCancellation::cancelled() const noexcept {
    return state_ && state_->cancelled.load(std::memory_order_relaxed);
}

bool TaskCancellation::finished() const noexcept {
    return state_ && state_->finished.load(std::memory_order_relaxed);
}

void UiCallbackQueue::post(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_.push_back(std::move(callback));
    }
    wake();
}

std::size_t UiCallbackQueue::drain(std::size_t max_callbacks) {
    std::deque<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (max_callbacks == 0 || max_callbacks >= callbacks_.size()) {
            callbacks.swap(callbacks_);
        } else {
            for (std::size_t index = 0; index < max_callbacks; ++index) {
                callbacks.push_back(std::move(callbacks_.front()));
                callbacks_.pop_front();
            }
        }
    }

    for (auto& callback : callbacks) {
        callback();
    }
    return callbacks.size();
}

bool UiCallbackQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return callbacks_.empty();
}

void UiCallbackQueue::set_wake_callback(std::function<void()> callback) {
    if (!callback) {
        wake_callback_.store(nullptr, std::memory_order_release);
        return;
    }
    wake_callback_.store(std::make_shared<const std::function<void()>>(std::move(callback)), std::memory_order_release);
}

void UiCallbackQueue::wake() noexcept {
    const std::shared_ptr<const std::function<void()>> callback = wake_callback_.load(std::memory_order_acquire);
    if (!callback || !*callback) {
        return;
    }
    try {
        (*callback)();
    } catch (...) {
        return;
    }
}

BackgroundExecutor::BackgroundExecutor(std::size_t worker_count, std::vector<int> cpu_affinity,
                                       std::string thread_name_prefix)
    : worker_pool_(worker_count, std::move(cpu_affinity), std::move(thread_name_prefix)) {}

std::size_t BackgroundExecutor::size() const noexcept {
    return worker_pool_.size();
}

void BackgroundExecutor::wait_idle() {
    worker_pool_.wait_idle();
}

}  
