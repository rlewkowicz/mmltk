#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::controller::contracts {
inline constexpr std::size_t kWorkspaceResourceCapacity = 256U;
inline constexpr std::size_t kWorkspacePathCapacity = 1024U;
namespace detail {
[[nodiscard]] constexpr bool workspace_text_valid(const std::string_view value) noexcept {
 return !value.empty() && std::ranges::all_of(value, [](const unsigned char character) { return character >= 0x20U && character != 0x7FU; });
}
template <std::size_t Capacity>
[[nodiscard]] constexpr bool workspace_storage_valid(const std::array<char, Capacity>& storage, const std::uint16_t size, const std::uint64_t revision) noexcept {
 return size != 0U && size <= storage.size() && revision != 0U && workspace_text_valid({storage.data(), size}) &&
        std::ranges::all_of(storage.begin() + static_cast<std::ptrdiff_t>(size), storage.end(), [](const char character) { return character == '\0'; });
}
template <std::size_t Capacity>
struct WorkspaceValue final {
 std::array<char, Capacity> storage{};
 std::uint16_t size = 0U;
 std::uint64_t revision = 0U;
 [[nodiscard]] bool valid() const noexcept { return workspace_storage_valid(storage, size, revision); }
 [[nodiscard]] std::string_view view() const noexcept { return valid() ? std::string_view{storage.data(), size} : std::string_view{}; }
 [[nodiscard]] static WorkspaceValue From(const std::string_view value, const std::uint64_t revision) noexcept {
  WorkspaceValue result{};
  if (value.size() > result.storage.size() || !workspace_text_valid(value) || revision == 0U) return result;
  result.size = static_cast<std::uint16_t>(value.size());
  std::copy_n(value.data(), result.size, result.storage.data());
  result.revision = revision;
  return result;
 }
 auto operator<=>(const WorkspaceValue&) const = default;
};
}  // namespace detail
using WorkspaceResource = detail::WorkspaceValue<kWorkspaceResourceCapacity>;
using WorkspacePath = detail::WorkspaceValue<kWorkspacePathCapacity>;
namespace detail {
MMLTK_REFLECT_FIELDS(WorkspaceValue<kWorkspaceResourceCapacity>)
MMLTK_REFLECT_FIELDS(WorkspaceValue<kWorkspacePathCapacity>)
}  // namespace detail
}  // namespace mmltk::controller::contracts
