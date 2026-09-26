#pragma once
#include <meta>
#include <string>
#include <string_view>
#include "mmltk/frameworks/reflection/member_path.h"
#include "src/frameworks/reflection/reflected_descriptors.h"

namespace mmltk::frameworks::reflection {
// Bind only the request, member owner and typed prefix; descriptors retain
// their existing policy, parser, emission and presence contracts.
template <class Request, class Owner = Request, auto... Prefix>
struct CliScope final {
 using request_type = Request;
 using owner_type = Owner;
 template <auto... Members>
 static constexpr auto path = member_path<Prefix..., Members...>;
 template <auto Member>
 using within = CliScope<Request, accessor_value_t<Owner, Member>, Prefix..., Member>;
 template <auto Member>
 [[nodiscard]] static consteval std::string_view name() {
  constexpr auto identity = accessor_member_identity<Request, path<Member>>();
  static_assert(identity.valid(), "CLI spelling requires a canonical reflected member");
  std::string result = "--";
  for (const char value : identity.name()) result += value == '_' ? '-' : value;
  return std::define_static_string(result);
 }
};
}  // namespace mmltk::frameworks::reflection

// Explicit spellings, aliases, negations and all remaining factory arguments
// stay visible at the declaration. Neither macro introduces CLI policy.
#define MMLTK_CLI_NAMED(Scope, Member, ...) \
 ::mmltk::frameworks::reflection::option<typename Scope::request_type, Scope::template path<&Scope::owner_type::Member>>(__VA_ARGS__)
#define MMLTK_CLI_OPTION(Scope, Member, ...) \
 MMLTK_CLI_NAMED(Scope, Member, Scope::template name<&Scope::owner_type::Member>(), __VA_ARGS__)
