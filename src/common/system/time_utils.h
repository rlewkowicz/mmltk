#pragma once
#include <cstdint>
namespace mmltk::common::system {
[[nodiscard]] std::uint64_t steady_clock_now_ns() noexcept;
}
