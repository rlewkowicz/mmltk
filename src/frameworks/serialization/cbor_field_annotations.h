#pragma once
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>
#include "src/frameworks/reflection/field_policy.h"
namespace mmltk::frameworks::serialization {
// Field annotations belong to the typed CBOR boundary.  They describe values,
// never action routing, composition, presentation, or transport lifecycle.
template <auto Value>
struct DefaultValue final {
 static constexpr auto value = Value;
};
template <class>
inline constexpr bool is_default_value_annotation = false;
template <auto Value>
inline constexpr bool is_default_value_annotation<DefaultValue<Value>> = true;
struct MinItems final : mmltk::frameworks::reflection::Annotation {
 std::size_t value;
 constexpr explicit MinItems(const std::size_t input) noexcept : value(input) {}
 constexpr bool operator==(const MinItems&) const noexcept = default;
};
struct CanonicalNonzeroUint64 final : mmltk::frameworks::reflection::Annotation {
 constexpr bool operator==(const CanonicalNonzeroUint64&) const noexcept = default;
};
template <std::size_t Size>
struct ExactString final : mmltk::frameworks::reflection::Annotation {
 char value[Size]{};
 consteval ExactString(const char (&source)[Size]) {
  for (std::size_t index = 0U; index < Size; ++index) value[index] = source[index];
 }
 [[nodiscard]] constexpr std::string_view view() const noexcept { return {value, Size - 1U}; }
 constexpr bool operator==(const ExactString&) const noexcept = default;
};
template <std::size_t Size>
ExactString(const char (&)[Size]) -> ExactString<Size>;
template <class>
inline constexpr bool is_exact_string_annotation = false;
template <std::size_t Size>
inline constexpr bool is_exact_string_annotation<ExactString<Size>> = true;
[[nodiscard]] constexpr bool is_canonical_nonzero_uint64(const std::string_view value) noexcept {
 if (value.empty() || value.front() == '0') return false;
 std::uint64_t parsed = 0U;
 const auto [cursor, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
 return error == std::errc{} && cursor == value.data() + value.size() && parsed != 0U;
}
}  // namespace mmltk::frameworks::serialization
