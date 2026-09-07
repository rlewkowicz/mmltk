#pragma once

#include <cstdint>
#include <limits>

namespace mmltk::common::types {

[[nodiscard]] constexpr bool advance_generation(std::uint64_t& generation) noexcept {
    if (generation == std::numeric_limits<std::uint64_t>::max()) { return false; }
    ++generation;
    return true;
}

}  // namespace mmltk::common::types
