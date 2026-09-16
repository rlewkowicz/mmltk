#pragma once
#include <compare>
#include <cstdint>
namespace mmltk::common::types {
template <typename Tag>
class StrongId final {
   public:
    constexpr StrongId() noexcept = default;
    explicit constexpr StrongId(const std::uint64_t value) noexcept : value_(value) {}
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return value_ != 0U; }
    auto operator<=>(const StrongId&) const = default;

   private:
    std::uint64_t value_ = 0U;
};
}  // namespace mmltk::common::types
