#pragma once

#include <cstdint>

namespace mmltk::controller::contracts {

struct DiagnosticContext final {
    std::uint64_t capacity_width = 0U;
    std::uint64_t capacity_height = 0U;
    std::uint64_t staging_bytes = 0U;
    std::uint64_t surface_high = 0U;
    std::uint64_t surface_low = 0U;
    std::uint64_t selection_generation = 0U;
    std::uint64_t frame_revision = 0U;
    std::uint64_t condition = 0U;
    std::uint64_t outcome = 0U;
};

}  // namespace mmltk::controller::contracts
