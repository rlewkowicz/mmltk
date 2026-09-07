#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace mmltk::common::types {

[[nodiscard]] inline std::uint64_t advance_monotonic_identity(const std::uint64_t current, const std::uint64_t step = 1U) {
    if (step == 0U || current > std::numeric_limits<std::uint64_t>::max() - step) throw std::overflow_error("monotonic identity exhausted");
    return current + step;
}

[[nodiscard]] inline std::uint64_t take_monotonic_identity(std::uint64_t& next, const std::uint64_t step = 1U) {
    if (next == 0U || next > std::numeric_limits<std::uint64_t>::max() - step) throw std::overflow_error("monotonic identity exhausted");
    const std::uint64_t result = next;
    next += step;
    return result;
}

[[nodiscard]] constexpr bool advance_generation(std::uint64_t& generation) noexcept {
    if (generation == std::numeric_limits<std::uint64_t>::max()) { return false; }
    ++generation;
    return true;
}

}  // namespace mmltk::common::types
