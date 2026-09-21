#pragma once
#include <array>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include "src/controller/contracts/workflows.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::controller::contracts::settings_vocabulary {
template <class T>
struct is_optional : std::false_type {};
template <class T>
struct is_optional<std::optional<T>> : std::true_type {
 using value_type = T;
};
template <class T>
struct is_vector : std::false_type {};
template <class T, class Allocator>
struct is_vector<std::vector<T, Allocator>> : std::true_type {
 using value_type = T;
};
template <class T>
struct is_array : std::false_type {};
template <class T, std::size_t Size>
struct is_array<std::array<T, Size>> : std::true_type {
 using value_type = T;
 static constexpr std::size_t size = Size;
};
template <class T>
inline constexpr bool is_leaf_v = [] {
 using U = std::remove_cvref_t<T>;
 return std::is_arithmetic_v<U> || std::is_enum_v<U> || std::same_as<U, std::string> || std::same_as<U, std::string_view> ||
        std::same_as<U, std::filesystem::path> || is_optional<U>::value || is_vector<U>::value || is_array<U>::value;
}();
// Storage aggregates may contain leaf-shaped runtime values (for example a
// vector of request records) that are persisted as a unit but are not valid
// browser mutations. Keep that distinction in the vocabulary owner so schema
// publication, mutation admission, and capacity accounting cannot diverge.
template <class T>
inline constexpr bool is_mutable_leaf_v = [] {
 using U = std::remove_cvref_t<T>;
 if constexpr (is_optional<U>::value) {
  return is_mutable_leaf_v<typename is_optional<U>::value_type>;
 } else if constexpr (is_vector<U>::value || is_array<U>::value) {
  using Element = typename U::value_type;
  return std::is_arithmetic_v<Element> || std::is_enum_v<Element> || std::same_as<Element, std::string> || std::same_as<Element, std::filesystem::path>;
 } else {
  return std::is_arithmetic_v<U> || std::is_enum_v<U> || std::same_as<U, std::string> || std::same_as<U, std::filesystem::path>;
 }
}();
template <class Declaration>
inline constexpr bool is_persistence_metadata_v = [] {
 std::size_t count = 0U;
 Declaration::VisitAnnotations(
  [&]<class Annotation>(const Annotation&) { count += std::same_as<std::remove_cvref_t<Annotation>, reflection::PersistenceMetadata> ? 1U : 0U; });
 if (count > 1U) throw "settings declaration has duplicate persistence-metadata annotations";
 return count == 1U;
}();
template <class Declaration, class T>
inline constexpr bool is_mutable_member_v = is_mutable_leaf_v<T> && !is_persistence_metadata_v<Declaration>;
template <bool MutableOnly, class T>
[[nodiscard]] consteval std::size_t count_leaves() {
 using U = std::remove_cvref_t<T>;
 if constexpr (mmltk::frameworks::reflection::kOpaqueRelationStorage<U>) {
  return 0U;
 } else if constexpr (is_leaf_v<U>) {
  return (!MutableOnly || is_mutable_leaf_v<U>) ? 1U : 0U;
 } else {
  std::size_t count = 0U;
  mmltk::frameworks::reflection::visit_materialized_bases<U>([&]<class Base>() { count += count_leaves<MutableOnly, Base>(); });
  mmltk::frameworks::reflection::visit_materialized_members<U>([&]<class Declaration>(const auto&) {
   using Member = typename Declaration::member_type;
   if constexpr (is_leaf_v<Member>)
    count += (!MutableOnly || is_mutable_member_v<Declaration, Member>) ? 1U : 0U;
   else
    count += count_leaves<MutableOnly, Member>();
  });
  return count;
 }
}
template <class T>
[[nodiscard]] consteval std::size_t leaf_count() {
 return count_leaves<false, T>();
}
template <class T>
[[nodiscard]] consteval std::size_t mutable_leaf_count() {
 return count_leaves<true, T>();
}
// This is the only reflection expansion for the settings aggregate vocabulary.
// It flattens inherited members and feeds every serializer, decoder, validator,
// mutator, descriptor audit, and generator consumer from the same member owner.
template <class T, class Visitor>
constexpr void for_each_member(T&& object, Visitor&& visitor) {
 using U = std::remove_cvref_t<T>;
 static_assert(!is_leaf_v<U>);
 if constexpr (mmltk::frameworks::reflection::kOpaqueRelationStorage<U>) return;
 mmltk::frameworks::reflection::visit_materialized_bases<U>(
  [&]<class Base>() { for_each_member(static_cast<std::conditional_t<std::is_const_v<std::remove_reference_t<T>>, const Base, Base>&>(object), visitor); });
 mmltk::frameworks::reflection::visit_materialized_members<U>([&]<class Declaration>(const auto& fact) {
  if constexpr (requires { Declaration::pointer; }) std::invoke(visitor, fact.member_name, object.*Declaration::pointer);
 });
}
template <class T, class Visitor>
constexpr void for_each_member_pair(const T& left, const T& right, Visitor&& visitor) {
 static_assert(!is_leaf_v<T>);
 if constexpr (mmltk::frameworks::reflection::kOpaqueRelationStorage<T>) return;
 mmltk::frameworks::reflection::visit_materialized_bases<T>(
  [&]<class Base>() { for_each_member_pair(static_cast<const Base&>(left), static_cast<const Base&>(right), visitor); });
 mmltk::frameworks::reflection::visit_materialized_members<T>([&]<class Declaration>(const auto& fact) {
  if constexpr (requires { Declaration::pointer; }) std::invoke(visitor, fact.member_name, left.*Declaration::pointer, right.*Declaration::pointer);
 });
}
template <class Enum, class Visitor>
 requires std::is_enum_v<Enum>
constexpr void for_each_enumerator(Visitor&& visitor) {
 for (const auto entry : mmltk::frameworks::reflection::enum_entries<Enum>()) std::invoke(visitor, entry.name, entry.value);
}
[[nodiscard]] constexpr std::pair<std::string_view, std::string_view> split_path(const std::string_view path) noexcept {
 const std::size_t separator = path.find('.');
 return separator == std::string_view::npos ? std::pair{path, std::string_view{}} : std::pair{path.substr(0U, separator), path.substr(separator + 1U)};
}
template <bool MutableOnly, class T, class Leaf>
[[nodiscard]] constexpr std::size_t visit_path_count(T&& object, const std::string_view path, Leaf&& leaf) {
 using U = std::remove_cvref_t<T>;
 if (path.empty() || is_leaf_v<U> || mmltk::frameworks::reflection::kOpaqueRelationStorage<U>) return 0U;
 const auto [head, tail] = split_path(path);
 std::size_t matches = 0U;
 mmltk::frameworks::reflection::visit_materialized_bases<U>([&]<class Base>() {
  matches += visit_path_count<MutableOnly>(static_cast<std::conditional_t<std::is_const_v<std::remove_reference_t<T>>, const Base, Base>&>(object), path, leaf);
 });
 mmltk::frameworks::reflection::visit_materialized_members<U>([&]<class Declaration>(const auto& fact) {
  if constexpr (requires { Declaration::pointer; }) {
   if (fact.member_name != head) return;
   auto&& value = object.*Declaration::pointer;
   using Member = std::remove_cvref_t<decltype(value)>;
   if (tail.empty()) {
    if constexpr (!MutableOnly || is_mutable_member_v<Declaration, Member>) {
     std::invoke(leaf, fact.member_name, value);
     ++matches;
    }
   } else if constexpr (!is_leaf_v<Member>) {
    matches += visit_path_count<MutableOnly>(value, tail, leaf);
   }
  }
 });
 return matches;
}
template <class T, class Leaf>
[[nodiscard]] constexpr bool visit_path(T&& object, const std::string_view path, Leaf&& leaf) {
 return visit_path_count<false>(std::forward<T>(object), path, leaf) == 1U;
}
template <class T, class Leaf>
[[nodiscard]] constexpr bool visit_mutable_path(T&& object, const std::string_view path, Leaf&& leaf) {
 return visit_path_count<true>(std::forward<T>(object), path, leaf) == 1U;
}
}  // namespace mmltk::controller::contracts::settings_vocabulary
