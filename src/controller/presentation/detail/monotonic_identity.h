#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace mmltk::controller::presentation::detail {

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

}  // namespace mmltk::controller::presentation::detail
