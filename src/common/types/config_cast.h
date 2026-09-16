#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
namespace mmltk::common::types {
[[nodiscard]] inline std::string config_field_cast(const std::filesystem::path& value) { return value.string(); }
[[nodiscard]] inline std::filesystem::path config_field_cast(const std::string& value) { return std::filesystem::path{value}; }
[[nodiscard]] constexpr int config_field_cast(const std::size_t value) noexcept { return static_cast<int>(value); }
[[nodiscard]] constexpr std::size_t config_field_cast(const int value) noexcept { return static_cast<std::size_t>(value); }
}  // namespace mmltk::common::types
