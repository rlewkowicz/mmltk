#pragma once
#include <concepts>
#include <cstddef>
#include <type_traits>
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::frameworks::reflection {
// A destination declaration is the complete projection inventory. Source field
// identity and exact type compatibility are resolved once at compile time.
template <class Destination, class Source>
[[nodiscard]] constexpr Destination project_record(const Source& source) {
 Destination result{};
 visit_materialized_members<Destination>([&]<class Target>(const auto&) {
  constexpr auto name = materialized_member_name<Target::pointer>();
  constexpr auto count = [=] {
   std::size_t found = 0U;
   visit_materialized_members<Source>([&]<class Field>(const auto&) {
    if constexpr (materialized_member_name<Field::pointer>() == name) ++found;
   });
   return found;
  }();
  static_assert(count == 1U, "record projection requires exactly one matching source declaration");
  visit_materialized_members<Source>([&]<class Field>(const auto&) {
   if constexpr (materialized_member_name<Field::pointer>() == name) {
    static_assert(std::same_as<typename Target::member_type, typename Field::member_type>);
    result.*Target::pointer = source.*Field::pointer;
   }
  });
 });
 return result;
}
}  // namespace mmltk::frameworks::reflection
