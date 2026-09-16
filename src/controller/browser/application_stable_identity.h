#pragma once
#include <cstdint>
#include <string_view>
#include "src/common/types/stable_identity.h"
namespace mmltk::controller::browser {
[[nodiscard]] constexpr std::uint64_t application_stable_id(const std::string_view owner, const std::string_view member = {}) noexcept {
    std::uint64_t value = mmltk::common::types::append(mmltk::common::types::kOffset, owner);
    if (!member.empty()) {
        value = mmltk::common::types::append_byte(value, static_cast<unsigned char>('.'));
        value = mmltk::common::types::append(value, member);
    }
    return mmltk::common::types::nonzero(value);
}
[[nodiscard]] constexpr std::uint64_t application_field_stable_id(const std::uint64_t endpoint, const std::string_view field) noexcept {
    return mmltk::common::types::nonzero(mmltk::common::types::append(endpoint, field));
}
[[nodiscard]] constexpr std::uint64_t application_settings_field_stable_id(const std::string_view path) noexcept { return application_stable_id(path); }
}  // namespace mmltk::controller::browser
