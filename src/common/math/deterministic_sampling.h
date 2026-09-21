#pragma once
#include <cstdint>
namespace mmltk::common::math {
#if defined(__CUDACC__)
[[nodiscard]] __host__ __device__
#else
[[nodiscard]]
#endif
 inline constexpr std::uint64_t deterministic_mix64(std::uint64_t value) noexcept {
 value += 0x9e3779b97f4a7c15ULL;
 value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
 value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
 return value ^ (value >> 31U);
}
}  // namespace mmltk::common::math
