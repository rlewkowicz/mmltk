
#pragma once

#include <cassert>
#include <vector>

#include "spdlog/common.h"

namespace spdlog {
namespace details {
template <typename T>
class circular_q {
    size_t max_items_ = 0;
    typename std::vector<T>::size_type head_ = 0;
    typename std::vector<T>::size_type tail_ = 0;
    size_t overrun_counter_ = 0;
    std::vector<T> v_;

   public:
    using value_type = T;

    circular_q() = default;

    explicit circular_q(size_t max_items) : max_items_(max_items + 1), v_(max_items_) {}

    circular_q(const circular_q&) = default;
    circular_q& operator=(const circular_q&) = default;

    circular_q(circular_q&& other) SPDLOG_NOEXCEPT {
        copy_moveable(std::move(other));
    }

    circular_q& operator=(circular_q&& other) SPDLOG_NOEXCEPT {
        copy_moveable(std::move(other));
        return *this;
    }

    void push_back(T&& item) {
        if (max_items_ > 0) {
            v_[tail_] = std::move(item);
            tail_ = (tail_ + 1) % max_items_;

            if (tail_ == head_) {
                head_ = (head_ + 1) % max_items_;
                ++overrun_counter_;
            }
        }
    }

    const T& front() const {
        return v_[head_];
    }

    T& front() {
        return v_[head_];
    }

    size_t size() const {
        if (tail_ >= head_) {
            return tail_ - head_;
        } else {
            return max_items_ - (head_ - tail_);
        }
    }

    const T& at(size_t i) const {
        assert(i < size());
        return v_[(head_ + i) % max_items_];
    }

    void pop_front() {
        head_ = (head_ + 1) % max_items_;
    }

    bool empty() const {
        return tail_ == head_;
    }

    bool full() const {
        if (max_items_ > 0) {
            return ((tail_ + 1) % max_items_) == head_;
        }
        return false;
    }

    size_t overrun_counter() const {
        return overrun_counter_;
    }

    void reset_overrun_counter() {
        overrun_counter_ = 0;
    }

   private:
    void copy_moveable(circular_q&& other) SPDLOG_NOEXCEPT {
        max_items_ = other.max_items_;
        head_ = other.head_;
        tail_ = other.tail_;
        overrun_counter_ = other.overrun_counter_;
        v_ = std::move(other.v_);

        other.max_items_ = 0;
        other.head_ = other.tail_ = 0;
        other.overrun_counter_ = 0;
    }
};
}  
}  
