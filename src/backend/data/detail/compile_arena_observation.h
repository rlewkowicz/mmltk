#pragma once
#include <cstddef>
#include <cstdint>
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::backend::data::detail {
struct CompileArenaObservation {
 std::uint64_t occupied_bytes = 0, requested_bytes = 0, alignment_loss = 0;
 std::uint64_t ordinary_free_bytes = 0, largest_ordinary_extent = 0, unregistered_bytes = 0;
 std::uint64_t completion_occupied = 0, shared_occupied = 0, compaction_occupied = 0;
 std::size_t backing_regions = 0, live_allocations = 0;
 std::uint64_t ordinary_capacity = 0, largest_ordinary_capacity = 0;
 std::uint64_t lease_acquisitions = 0, allocation_high_water = 0, occupied_high_water = 0;
};
MMLTK_REFLECT_FIELDS(CompileArenaObservation)
}  // namespace mmltk::backend::data::detail
