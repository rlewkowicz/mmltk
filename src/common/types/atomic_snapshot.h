#pragma once

#include <atomic>

namespace mmltk::common::types {

template <typename Value>
[[nodiscard]] Value atomic_snapshot(const std::atomic<Value>& value, const std::memory_order order = std::memory_order_relaxed) noexcept {
    return value.load(order);
}

}  // namespace mmltk::common::types
