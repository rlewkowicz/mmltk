
#pragma once

#include <atomic>
#include <utility>

namespace spdlog {
namespace details {
struct null_mutex {
    void lock() const {}
    void unlock() const {}
};

struct null_atomic_int {
    int value{0};
    null_atomic_int() = default;

    explicit null_atomic_int(int new_value) : value(new_value) {}

    int load(std::memory_order = std::memory_order_relaxed) const {
        return value;
    }

    void store(int new_value, std::memory_order = std::memory_order_relaxed) {
        value = new_value;
    }

    int exchange(int new_value, std::memory_order = std::memory_order_relaxed) {
        std::swap(new_value, value);
        return new_value;
    }
};

}  
}  
