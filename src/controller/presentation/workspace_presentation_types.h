#pragma once
#include <compare>
#include <cstdint>
#include <limits>
namespace mmltk::controller::presentation {
struct WorkspaceContentIdentity final {
 std::uint64_t session = 0U;
 std::uint64_t sequence = 0U;
 [[nodiscard]] constexpr bool valid() const noexcept { return session != 0U || sequence != 0U; }
 constexpr auto operator<=>(const WorkspaceContentIdentity&) const = default;
};
enum class WorkspacePresentationLayer : std::uint64_t {
 Primary = 0U,
 Invalid = std::numeric_limits<std::uint64_t>::max(),
};
}  // namespace mmltk::controller::presentation
