#pragma once
#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::frameworks::reflection {
namespace detail {
template <class Value>
[[nodiscard]] consteval bool member_chain_is_applicable() {
 return false;
}
template <class Value, auto Member, auto... Rest>
[[nodiscard]] consteval bool member_chain_is_applicable() {
 if constexpr (!std::is_invocable_v<decltype(Member), Value&>) {
  return false;
 } else if constexpr (sizeof...(Rest) == 0U) {
  return true;
 } else {
  using Next = decltype(std::invoke(Member, std::declval<Value&>()));
  return member_chain_is_applicable<Next, Rest...>();
 }
}
}  // namespace detail
struct ReflectedMemberSegment final {
 std::string_view owner;
 std::string_view member;
 constexpr bool operator==(const ReflectedMemberSegment&) const noexcept = default;
};
class ReflectedMemberPath;
class ReflectedMemberIdentity final {
public:
 static constexpr std::size_t kMaximumDepth = 8U;
 constexpr ReflectedMemberIdentity() noexcept = default;
 template <class Root, auto... Members>
 [[nodiscard]] static consteval ReflectedMemberIdentity from_path() {
  using CanonicalRoot = std::remove_cvref_t<Root>;
  static_assert(sizeof...(Members) != 0U && sizeof...(Members) <= kMaximumDepth, "reflected member path exceeds its fixed identity capacity");
  if constexpr (sizeof...(Members) != 0U && sizeof...(Members) <= kMaximumDepth) {
   static_assert(detail::member_chain_is_applicable<CanonicalRoot, Members...>(), "reflected member path cannot be applied to its root type");
  }
  ReflectedMemberIdentity result;
  result.root_ = reflected_type_name<CanonicalRoot>();
  ((result.segments_[result.depth_++] = segment<Members>()), ...);
  return result;
 }
 [[nodiscard]] constexpr bool valid() const noexcept {
  if (root_.empty() || depth_ == 0U || depth_ > segments_.size()) return false;
  for (std::size_t index = 0U; index < depth_; ++index)
   if (segments_[index].owner.empty() || segments_[index].member.empty()) return false;
  return true;
 }
 [[nodiscard]] constexpr std::string_view name() const noexcept { return depth_ == 0U ? std::string_view{} : segments_[depth_ - 1U].member; }
 [[nodiscard]] constexpr bool operator==(const ReflectedMemberIdentity& other) const noexcept {
  if (root_ != other.root_ || depth_ != other.depth_) return false;
  for (std::size_t index = 0U; index < depth_; ++index) {
   if (segments_[index] != other.segments_[index]) return false;
  }
  return true;
 }

private:
 friend class ReflectedMemberPath;
 template <class T>
 [[nodiscard]] static consteval std::string_view reflected_type_name() {
  return type_name<T>();
 }
 template <auto Member>
 [[nodiscard]] static consteval ReflectedMemberSegment segment() {
  ReflectedMemberSegment result{};
  if constexpr (std::is_member_object_pointer_v<decltype(Member)>) {
   using Owner = typename MemberPointerOwner<std::remove_cvref_t<decltype(Member)>>::type;
   if constexpr (requires { materialized_field_policies(std::type_identity<Owner>{}); }) {
    visit_materialized_members<Owner>([&]<class Declaration>(const auto& fact) {
     if constexpr (std::same_as<std::remove_cvref_t<decltype(Declaration::pointer)>, std::remove_cvref_t<decltype(Member)>>) {
      if (Declaration::pointer == Member) result = {reflected_type_name<Owner>(), fact.member_name};
     }
    });
   }
  }
  return result;
 }
 std::string_view root_{};
 std::array<ReflectedMemberSegment, kMaximumDepth> segments_{};
 std::size_t depth_ = 0U;
};
template <auto Member, auto... Rest>
struct MemberPathAccessor final {
 static constexpr auto terminal_member = [] {
  constexpr auto path = std::tuple{Member, Rest...};
  return std::get<sizeof...(Rest)>(path);
 }();
 template <class Root>
 [[nodiscard]] static consteval ReflectedMemberIdentity identity() {
  return ReflectedMemberIdentity::from_path<Root, Member, Rest...>();
 }
 template <class Value>
 [[nodiscard]] static consteval bool applicable() {
  return detail::member_chain_is_applicable<Value, Member, Rest...>();
 }
 template <class Value>
  requires(MemberPathAccessor::template applicable<Value>())
 [[nodiscard]] constexpr decltype(auto) operator()(Value&& value) const noexcept {
  if constexpr (sizeof...(Rest) == 0U) {
   return std::invoke(Member, std::forward<Value>(value));
  } else {
   return MemberPathAccessor<Rest...>{}(std::invoke(Member, std::forward<Value>(value)));
  }
 }
};
template <auto... Members>
inline constexpr MemberPathAccessor<Members...> member_path{};
namespace detail {
template <class Root, class Destination, auto... Selector, auto... Relative>
[[nodiscard]] consteval auto rebase_member_path_impl(MemberPathAccessor<Selector...>, MemberPathAccessor<Relative...>) {
 static_assert(sizeof...(Selector) != 0U, "a provider selector cannot be empty");
 static_assert(sizeof...(Relative) != 0U, "a provider-relative destination cannot be empty");
 constexpr auto selector = std::tuple{Selector...};
 return [&]<std::size_t... Index>(std::index_sequence<Index...>) {
  if constexpr (sizeof...(Index) == 0U) {
   static_assert(std::same_as<std::remove_cvref_t<Root>, std::remove_cvref_t<Destination>>, "the provider selector parent must have the relation destination type");
  } else {
   using ParentAccessor = MemberPathAccessor<std::get<Index>(selector)...>;
   static_assert(
    std::same_as<std::remove_cvref_t<decltype(ParentAccessor{}(std::declval<Root&>()))>, std::remove_cvref_t<Destination>>, "the provider selector parent must have the relation destination type");
  }
  using Rebased = MemberPathAccessor<std::get<Index>(selector)..., Relative...>;
  static_assert(Rebased::template applicable<std::remove_cvref_t<Root>>(), "the rebased provider destination must apply to its canonical root");
  return Rebased{};
 }(std::make_index_sequence<sizeof...(Selector) - 1U>{});
}
}  // namespace detail
template <class Root, class Destination, auto... Selector, auto... Relative>
[[nodiscard]] consteval auto rebase_member_path(MemberPathAccessor<Selector...> selector, MemberPathAccessor<Relative...> relative) {
 return detail::rebase_member_path_impl<std::remove_cvref_t<Root>, std::remove_cvref_t<Destination>>(selector, relative);
}
template <class Request, auto Access>
 requires std::is_invocable_v<decltype(Access), Request&>
[[nodiscard]] constexpr decltype(auto) access(Request& request) noexcept {
 return std::invoke(Access, request);
}
template <class Request, auto Access>
using accessor_value_t = std::remove_cvref_t<decltype(access<Request, Access>(std::declval<Request&>()))>;
template <auto Access>
[[nodiscard]] consteval FieldConstraint accessor_policy() {
 using AccessType = std::remove_cvref_t<decltype(Access)>;
 if constexpr (std::is_member_object_pointer_v<AccessType>) {
  return policy_of_member<Access>();
 } else if constexpr (requires { AccessType::terminal_member; }) {
  return policy_of_member<AccessType::terminal_member>();
 } else {
  return {};
 }
}
template <class Root, auto Access>
[[nodiscard]] consteval bool accessor_is_applicable() {
 using AccessType = std::remove_cvref_t<decltype(Access)>;
 if constexpr (std::is_member_object_pointer_v<AccessType>) {
  return detail::member_chain_is_applicable<Root, Access>();
 } else if constexpr (requires { AccessType::template applicable<Root>(); }) {
  return AccessType::template applicable<Root>();
 } else {
  return std::is_invocable_v<AccessType, Root&>;
 }
}
template <class Request, auto Access>
[[nodiscard]] consteval ReflectedMemberIdentity accessor_member_identity() {
 static_assert(accessor_is_applicable<Request, Access>(), "reflected member accessor cannot be applied to its root type");
 // CPD-OFF: Identity and applicability dispatch over the same exhaustive accessor categories with different results.
 using AccessType = std::remove_cvref_t<decltype(Access)>;
 if constexpr (std::is_member_object_pointer_v<AccessType>) {
  return ReflectedMemberIdentity::from_path<Request, Access>();
 } else if constexpr (requires { AccessType::template identity<Request>(); }) {
  return AccessType::template identity<Request>();
 } else {
  return {};
 }
 // CPD-ON
}
class ReflectedMemberPath final {
public:
 static constexpr std::size_t kMaximumBytes = kMaximumPathBytes;
 [[nodiscard]] constexpr std::string_view view() const noexcept { return {text_.data(), size_}; }
 [[nodiscard]] constexpr bool operator==(const ReflectedMemberPath&) const noexcept = default;

private:
 template <class Root, auto Access>
 friend consteval ReflectedMemberPath reflected_member_path();
 [[nodiscard]] static consteval ReflectedMemberPath from_identity(const ReflectedMemberIdentity identity) {
  if (!identity.valid()) throw "cannot materialize an empty reflected member path";
  ReflectedMemberPath result;
  for (std::size_t index = 0U; index < identity.depth_; ++index) {
   const auto member = identity.segments_[index].member;
   const std::size_t separator = index == 0U ? 0U : 1U;
   if (result.size_ + separator + member.size() > result.text_.size()) throw "reflected member path exceeds its fixed text capacity";
   if (separator != 0U) result.text_[result.size_++] = '.';
   for (const char value : member) result.text_[result.size_++] = value;
  }
  return result;
 }
 constexpr ReflectedMemberPath() noexcept = default;
 std::array<char, kMaximumBytes> text_{};
 std::size_t size_ = 0U;
};
template <class Root, auto Access>
[[nodiscard]] consteval ReflectedMemberPath reflected_member_path() {
 return ReflectedMemberPath::from_identity(accessor_member_identity<Root, Access>());
}
}  // namespace mmltk::frameworks::reflection
