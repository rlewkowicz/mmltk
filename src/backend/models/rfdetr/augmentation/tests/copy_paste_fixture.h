#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include "src/backend/data/compiled_format.h"
namespace mmltk::backend::models::rfdetr::test_support {
// Explicit pixel fixture, independent of augmentation's support resolver.
// ........
// ..####..
// .##..##.
// .#....#.
// .#....#.
// .##..##.
// ..####..
// ........
inline constexpr std::array ring_runs{mmltk::backend::data::RLEPair{10, 4}, mmltk::backend::data::RLEPair{17, 2}, mmltk::backend::data::RLEPair{21, 2},
                                      mmltk::backend::data::RLEPair{25, 1}, mmltk::backend::data::RLEPair{30, 1}, mmltk::backend::data::RLEPair{33, 1},
                                      mmltk::backend::data::RLEPair{38, 1}, mmltk::backend::data::RLEPair{41, 2}, mmltk::backend::data::RLEPair{45, 2},
                                      mmltk::backend::data::RLEPair{50, 4}};
inline constexpr std::array dot_runs{mmltk::backend::data::RLEPair{27, 1},   // Hole.
                                     mmltk::backend::data::RLEPair{18, 3},   // Inner rim: two survivors.
                                     mmltk::backend::data::RLEPair{9, 2},    // Outer rim: one survivor.
                                     mmltk::backend::data::RLEPair{10, 2},   // Fully hidden.
                                     mmltk::backend::data::RLEPair{0, 1},    // Outside.
                                     mmltk::backend::data::RLEPair{24, 8},   // Three disconnected fragments.
                                     mmltk::backend::data::RLEPair{10, 5}};  // One survivor after four occluded pixels.
inline constexpr std::array<std::uint64_t, 7> identity_survivors{1ULL << 27, 3ULL << 19, 1ULL << 9, 0, 1, (1ULL << 24) | (15ULL << 26) | (1ULL << 31),
                                                                 1ULL << 14};
[[nodiscard]] inline bool fixture_contains(const std::span<const mmltk::backend::data::RLEPair> runs, const int x, const int y) {
    if (x < 0 || x >= 8 || y < 0 || y >= 8) return false;
    const auto pixel = static_cast<std::uint32_t>(y * 8 + x);
    for (const auto run : runs)
        if (pixel >= run.start && pixel - run.start < run.length) return true;
    return false;
}
[[nodiscard]] inline bool ring_paste_contains(const std::array<float, 6>& inverse, bool masked, int x, int y) {
    const float dx = inverse[0] * ((static_cast<float>(x) + .5F) / 8) + inverse[2];
    const float dy = inverse[4] * ((static_cast<float>(y) + .5F) / 8) + inverse[5];
    if (!masked) return dx >= .125F && dx <= .875F && dy >= .125F && dy <= .875F;
    return dx >= 0 && dx <= 1 && dy >= 0 && dy <= 1 &&
           fixture_contains(ring_runs, std::clamp(static_cast<int>(std::nearbyint(dx * 8 - .5F)), 0, 7),
                            std::clamp(static_cast<int>(std::nearbyint(dy * 8 - .5F)), 0, 7));
}
}  // namespace mmltk::backend::models::rfdetr::test_support
