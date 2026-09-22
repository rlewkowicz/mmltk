#pragma once
#include <cstddef>
#include <string_view>
namespace mmltk::common::types {
// Recognize one complete scalar without imposing replacement or text policy.
[[nodiscard]] constexpr std::size_t utf8_prefix_length(const std::string_view text) noexcept {
 if (text.empty()) return 0U;
 const auto first = static_cast<unsigned char>(text.front());
 if (first < 0x80U) return 1U;
 std::size_t length = 0U;
 if (first >= 0xc2U && first <= 0xdfU)
  length = 2U;
 else if (first >= 0xe0U && first <= 0xefU)
  length = 3U;
 else if (first >= 0xf0U && first <= 0xf4U)
  length = 4U;
 if (length == 0U || text.size() < length) return 0U;
 for (std::size_t index = 1U; index < length; ++index) {
  const auto next = static_cast<unsigned char>(text[index]);
  if (next < 0x80U || next > 0xbfU) return 0U;
  if (index == 1U && ((first == 0xe0U && next < 0xa0U) || (first == 0xedU && next > 0x9fU) || (first == 0xf0U && next < 0x90U) || (first == 0xf4U && next > 0x8fU))) return 0U;
 }
 return length;
}
[[nodiscard]] constexpr bool valid_utf8(std::string_view text) noexcept {
 while (!text.empty()) {
  const auto length = utf8_prefix_length(text);
  if (length == 0U) return false;
  text.remove_prefix(length);
 }
 return true;
}
}  // namespace mmltk::common::types
