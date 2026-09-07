#pragma once

#include <cstddef>

namespace mmltk::frameworks::gpu::detail {

// Generic fixed-storage safety bound. The consuming application supplies the
// exact active capacity from its own physical-resource topology.
struct TerminalRetirementStorageLimit final {
    static constexpr std::size_t kMaximumSlots = 64U;
};

}  // namespace mmltk::frameworks::gpu::detail
