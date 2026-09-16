#pragma once
#include <array>
#include <cstddef>
#include <meta>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include "mmltk/frameworks/reflection/materializer.h"
namespace mmltk::frameworks::reflection {
// This is the single reflection utility surface.  Vocabulary owners declare
// their values in their own headers; schema, validation, diagnostics, and
// browser projections consume the same reflected declarations through here.
template <std::meta::info Target>
[[nodiscard]] consteval auto reflected_annotations() {
    return std::define_static_array(std::meta::annotations_of(Target));
}
template <std::meta::info Target, class Predicate>
[[nodiscard]] consteval std::size_t reflected_annotation_count([[maybe_unused]] Predicate predicate) {
    std::size_t count = 0U;
    template for (constexpr auto annotation : reflected_annotations<Target>()) {
        using AnnotationType = std::remove_cvref_t<typename[:std::meta::type_of(annotation):]>;
        if (predicate.template operator()<AnnotationType>()) ++count;
    }
    return count;
}
template <class Enum>
    requires std::is_enum_v<Enum>
struct EnumEntry final {
    std::string_view name;
    Enum value;
};
struct EnumMaterializer final {
    template <class Enum, class Reflection>
        requires std::is_enum_v<Enum>
    [[nodiscard]] consteval auto operator()() const {
        std::array<EnumEntry<Enum>, Reflection::enumerators.size()> result{};
        std::size_t index = 0U;
        template for (constexpr auto enumerator : Reflection::enumerators) {
            result[index++] = {
                std::define_static_string(std::meta::identifier_of(enumerator)),
                         [:enumerator:],
            };
        }
        return result;
    }
};
template <class Enum>
    requires std::is_enum_v<Enum>
inline constexpr auto kReflectedEnumEntries = materialize<Enum>(EnumMaterializer{});
#define MMLTK_REFLECT_ENUM(Type)                                                              \
    [[nodiscard]] consteval const auto& materialized_enum_entries(std::type_identity<Type>) { \
        return ::mmltk::frameworks::reflection::kReflectedEnumEntries<Type>;                  \
    }
template <class Enum>
    requires std::is_enum_v<Enum>
consteval void materialized_enum_entries(std::type_identity<Enum>) = delete;
template <class Enum>
    requires std::is_enum_v<Enum>
[[nodiscard]] consteval auto enum_entries() {
    return materialized_enum_entries(std::type_identity<Enum>{});
}
template <class Enum>
    requires std::is_enum_v<Enum>
[[nodiscard]] constexpr bool enum_contains(const Enum value) noexcept {
    for (const auto entry : enum_entries<Enum>()) {
        if (entry.value == value) return true;
    }
    return false;
}
template <class Enum>
    requires std::is_enum_v<Enum>
[[nodiscard]] constexpr std::string_view enum_name(const Enum value) noexcept {
    for (const auto entry : enum_entries<Enum>()) {
        if (entry.value == value) return entry.name;
    }
    return {};
}
template <class Enum>
    requires std::is_enum_v<Enum>
[[nodiscard]] constexpr std::optional<Enum> try_enum_from_name(const std::string_view name) noexcept {
    for (const auto entry : enum_entries<Enum>()) {
        if (entry.name == name) return entry.value;
    }
    return std::nullopt;
}
template <class Enum>
    requires std::is_enum_v<Enum>
[[nodiscard]] Enum enum_from_name(const std::string_view name) {
    if (const auto value = try_enum_from_name<Enum>(name)) return *value;
    throw std::runtime_error("invalid reflected enum value: " + std::string(name));
}
template <class T>
[[nodiscard]] consteval std::string_view type_name() {
    if constexpr (std::meta::has_identifier(^^T)) { return std::meta::identifier_of(^^T); }
    return std::meta::display_string_of(^^T);
}
template <auto Member>
[[nodiscard]] consteval std::string_view member_name() {
    return std::meta::display_string_of(std::meta::reflect_constant(Member));
}
}  // namespace mmltk::frameworks::reflection
