

#ifndef CATCH_MATCHERS_TEMPLATED_HPP_INCLUDED
#define CATCH_MATCHERS_TEMPLATED_HPP_INCLUDED

#include <algorithm>
#include <array>
#include <catch2/internal/catch_lifetimebound.hpp>
#include <catch2/internal/catch_logical_traits.hpp>
#include <catch2/internal/catch_move_and_forward.hpp>
#include <catch2/internal/catch_stringref.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <string>
#include <type_traits>

namespace Catch {
namespace Matchers {
class MatcherGenericBase : public MatcherUntypedBase {
   public:
    MatcherGenericBase() = default;
    ~MatcherGenericBase() override;

    MatcherGenericBase(MatcherGenericBase const&) = default;
    MatcherGenericBase(MatcherGenericBase&&) = default;

    MatcherGenericBase& operator=(MatcherGenericBase const&) = delete;
    MatcherGenericBase& operator=(MatcherGenericBase&&) = delete;
};

namespace Detail {
template <std::size_t N, std::size_t M>
std::array<void const*, N + M> array_cat(std::array<void const*, N>&& lhs, std::array<void const*, M>&& rhs) {
    std::array<void const*, N + M> arr{};
    std::copy_n(lhs.begin(), N, arr.begin());
    std::copy_n(rhs.begin(), M, arr.begin() + N);
    return arr;
}

template <std::size_t N>
std::array<void const*, N + 1> array_cat(std::array<void const*, N>&& lhs, void const* rhs) {
    std::array<void const*, N + 1> arr{};
    std::copy_n(lhs.begin(), N, arr.begin());
    arr[N] = rhs;
    return arr;
}

template <std::size_t N>
std::array<void const*, N + 1> array_cat(void const* lhs, std::array<void const*, N>&& rhs) {
    std::array<void const*, N + 1> arr{{lhs}};
    std::copy_n(rhs.begin(), N, arr.begin() + 1);
    return arr;
}

template <typename T>
static constexpr bool is_generic_matcher_v =
    std::is_base_of<Catch::Matchers::MatcherGenericBase, std::remove_cv_t<std::remove_reference_t<T>>>::value;

template <typename... Ts>
static constexpr bool are_generic_matchers_v =
    Catch::Detail::conjunction<std::integral_constant<bool, is_generic_matcher_v<Ts>>...>::value;

template <typename T>
static constexpr bool is_matcher_v =
    std::is_base_of<Catch::Matchers::MatcherUntypedBase, std::remove_cv_t<std::remove_reference_t<T>>>::value;

// Shared short-circuiting fold over a pack of type-erased matchers.
// IsConjunction == true behaves like &&-chaining, false like ||-chaining.
template <bool IsConjunction, std::size_t N, typename Arg>
bool match_multi_of(Arg&&, std::array<void const*, N> const&, std::index_sequence<>) {
    return IsConjunction;
}

template <bool IsConjunction, typename T, typename... MatcherTs, std::size_t N, typename Arg, std::size_t Idx,
          std::size_t... Indices>
bool match_multi_of(Arg&& arg, std::array<void const*, N> const& matchers, std::index_sequence<Idx, Indices...>) {
    const bool matched = static_cast<T const*>(matchers[Idx])->match(arg);
    if (matched != IsConjunction) {
        return matched;
    }
    return match_multi_of<IsConjunction, MatcherTs...>(arg, matchers, std::index_sequence<Indices...>{});
}

std::string describe_multi_matcher(StringRef combine, std::string const* descriptions_begin,
                                   std::string const* descriptions_end);

template <typename... MatcherTs, std::size_t... Idx>
std::string describe_multi_matcher(StringRef combine, std::array<void const*, sizeof...(MatcherTs)> const& matchers,
                                   std::index_sequence<Idx...>) {
    std::array<std::string, sizeof...(MatcherTs)> descriptions{
        {static_cast<MatcherTs const*>(matchers[Idx])->toString()...}};

    return describe_multi_matcher(combine, descriptions.data(), descriptions.data() + descriptions.size());
}

template <bool IsConjunction, typename... MatcherTs>
class MatchMultiOfGeneric final : public MatcherGenericBase {
   public:
    MatchMultiOfGeneric(MatchMultiOfGeneric const&) = delete;
    MatchMultiOfGeneric& operator=(MatchMultiOfGeneric const&) = delete;
    MatchMultiOfGeneric(MatchMultiOfGeneric&&) = default;
    MatchMultiOfGeneric& operator=(MatchMultiOfGeneric&&) = delete;

    MatchMultiOfGeneric(MatcherTs const&... matchers CATCH_ATTR_LIFETIMEBOUND)
        : m_matchers{{std::addressof(matchers)...}} {}
    explicit MatchMultiOfGeneric(std::array<void const*, sizeof...(MatcherTs)> matchers) : m_matchers{matchers} {}

    template <typename Arg>
    bool match(Arg&& arg) const {
        return match_multi_of<IsConjunction, MatcherTs...>(arg, m_matchers, std::index_sequence_for<MatcherTs...>{});
    }

    std::string describe() const override {
        return describe_multi_matcher<MatcherTs...>(IsConjunction ? " and "_sr : " or "_sr, m_matchers,
                                                    std::index_sequence_for<MatcherTs...>{});
    }

    std::array<void const*, sizeof...(MatcherTs)> m_matchers;
};

template <typename... MatcherTs>
using MatchAllOfGeneric = MatchMultiOfGeneric<true, MatcherTs...>;
template <typename... MatcherTs>
using MatchAnyOfGeneric = MatchMultiOfGeneric<false, MatcherTs...>;

// Shared combiners behind operator&& / operator||: merge two multi-matchers of
// the same kind, or append/prepend a single matcher to a multi-matcher.
template <bool IsConjunction, typename... MatcherTs, typename... MatchersRHS>
MatchMultiOfGeneric<IsConjunction, MatcherTs..., MatchersRHS...> combine_multi_matchers(
    MatchMultiOfGeneric<IsConjunction, MatcherTs...>&& lhs, MatchMultiOfGeneric<IsConjunction, MatchersRHS...>&& rhs) {
    return MatchMultiOfGeneric<IsConjunction, MatcherTs..., MatchersRHS...>{
        array_cat(CATCH_MOVE(lhs.m_matchers), CATCH_MOVE(rhs.m_matchers))};
}

template <bool IsConjunction, typename MatcherRHS, typename... MatcherTs>
MatchMultiOfGeneric<IsConjunction, MatcherTs..., MatcherRHS> append_multi_matcher(
    MatchMultiOfGeneric<IsConjunction, MatcherTs...>&& lhs, MatcherRHS const& rhs) {
    return MatchMultiOfGeneric<IsConjunction, MatcherTs..., MatcherRHS>{
        array_cat(CATCH_MOVE(lhs.m_matchers), static_cast<void const*>(std::addressof(rhs)))};
}

template <bool IsConjunction, typename MatcherLHS, typename... MatcherTs>
MatchMultiOfGeneric<IsConjunction, MatcherLHS, MatcherTs...> prepend_multi_matcher(
    MatcherLHS const& lhs, MatchMultiOfGeneric<IsConjunction, MatcherTs...>&& rhs) {
    return MatchMultiOfGeneric<IsConjunction, MatcherLHS, MatcherTs...>{
        array_cat(static_cast<void const*>(std::addressof(lhs)), CATCH_MOVE(rhs.m_matchers))};
}

template <typename... MatcherTs, typename... MatchersRHS>
MatchAllOfGeneric<MatcherTs..., MatchersRHS...> operator&&(
    MatchAllOfGeneric<MatcherTs...>&& lhs CATCH_ATTR_LIFETIMEBOUND,
    MatchAllOfGeneric<MatchersRHS...>&& rhs CATCH_ATTR_LIFETIMEBOUND) {
    return combine_multi_matchers(CATCH_MOVE(lhs), CATCH_MOVE(rhs));
}

template <typename MatcherRHS, typename... MatcherTs>
std::enable_if_t<is_matcher_v<MatcherRHS>, MatchAllOfGeneric<MatcherTs..., MatcherRHS>> operator&&(
    MatchAllOfGeneric<MatcherTs...>&& lhs CATCH_ATTR_LIFETIMEBOUND, MatcherRHS const& rhs CATCH_ATTR_LIFETIMEBOUND) {
    return append_multi_matcher(CATCH_MOVE(lhs), rhs);
}

template <typename MatcherLHS, typename... MatcherTs>
std::enable_if_t<is_matcher_v<MatcherLHS>, MatchAllOfGeneric<MatcherLHS, MatcherTs...>> operator&&(
    MatcherLHS const& lhs CATCH_ATTR_LIFETIMEBOUND, MatchAllOfGeneric<MatcherTs...>&& rhs CATCH_ATTR_LIFETIMEBOUND) {
    return prepend_multi_matcher(lhs, CATCH_MOVE(rhs));
}

template <typename... MatcherTs, typename... MatchersRHS>
MatchAnyOfGeneric<MatcherTs..., MatchersRHS...> operator||(
    MatchAnyOfGeneric<MatcherTs...>&& lhs CATCH_ATTR_LIFETIMEBOUND,
    MatchAnyOfGeneric<MatchersRHS...>&& rhs CATCH_ATTR_LIFETIMEBOUND) {
    return combine_multi_matchers(CATCH_MOVE(lhs), CATCH_MOVE(rhs));
}

template <typename MatcherRHS, typename... MatcherTs>
std::enable_if_t<is_matcher_v<MatcherRHS>, MatchAnyOfGeneric<MatcherTs..., MatcherRHS>> operator||(
    MatchAnyOfGeneric<MatcherTs...>&& lhs CATCH_ATTR_LIFETIMEBOUND, MatcherRHS const& rhs CATCH_ATTR_LIFETIMEBOUND) {
    return append_multi_matcher(CATCH_MOVE(lhs), rhs);
}

template <typename MatcherLHS, typename... MatcherTs>
std::enable_if_t<is_matcher_v<MatcherLHS>, MatchAnyOfGeneric<MatcherLHS, MatcherTs...>> operator||(
    MatcherLHS const& lhs CATCH_ATTR_LIFETIMEBOUND, MatchAnyOfGeneric<MatcherTs...>&& rhs CATCH_ATTR_LIFETIMEBOUND) {
    return prepend_multi_matcher(lhs, CATCH_MOVE(rhs));
}

template <typename MatcherT>
class MatchNotOfGeneric final : public MatcherGenericBase {
    MatcherT const& m_matcher;

   public:
    MatchNotOfGeneric(MatchNotOfGeneric const&) = delete;
    MatchNotOfGeneric& operator=(MatchNotOfGeneric const&) = delete;
    MatchNotOfGeneric(MatchNotOfGeneric&&) = default;
    MatchNotOfGeneric& operator=(MatchNotOfGeneric&&) = delete;

    explicit MatchNotOfGeneric(MatcherT const& matcher CATCH_ATTR_LIFETIMEBOUND) : m_matcher{matcher} {}

    template <typename Arg>
    bool match(Arg&& arg) const {
        return !m_matcher.match(arg);
    }

    std::string describe() const override {
        return "not " + m_matcher.toString();
    }

    friend MatcherT const& operator!(MatchNotOfGeneric<MatcherT> const& matcher CATCH_ATTR_LIFETIMEBOUND) {
        return matcher.m_matcher;
    }
};
}

template <typename MatcherLHS, typename MatcherRHS>
std::enable_if_t<Detail::are_generic_matchers_v<MatcherLHS, MatcherRHS>,
                 Detail::MatchAllOfGeneric<MatcherLHS, MatcherRHS>>
operator&&(MatcherLHS const& lhs CATCH_ATTR_LIFETIMEBOUND, MatcherRHS const& rhs CATCH_ATTR_LIFETIMEBOUND) {
    return {lhs, rhs};
}

template <typename MatcherLHS, typename MatcherRHS>
std::enable_if_t<Detail::are_generic_matchers_v<MatcherLHS, MatcherRHS>,
                 Detail::MatchAnyOfGeneric<MatcherLHS, MatcherRHS>>
operator||(MatcherLHS const& lhs CATCH_ATTR_LIFETIMEBOUND, MatcherRHS const& rhs CATCH_ATTR_LIFETIMEBOUND) {
    return {lhs, rhs};
}

template <typename MatcherT>
std::enable_if_t<Detail::is_generic_matcher_v<MatcherT>, Detail::MatchNotOfGeneric<MatcherT>> operator!(
    MatcherT const& matcher CATCH_ATTR_LIFETIMEBOUND) {
    return Detail::MatchNotOfGeneric<MatcherT>{matcher};
}

template <typename MatcherLHS, typename ArgRHS>
std::enable_if_t<Detail::is_generic_matcher_v<MatcherLHS>, Detail::MatchAllOfGeneric<MatcherLHS, MatcherBase<ArgRHS>>>
operator&&(MatcherLHS const& lhs CATCH_ATTR_LIFETIMEBOUND, MatcherBase<ArgRHS> const& rhs CATCH_ATTR_LIFETIMEBOUND) {
    return {lhs, rhs};
}

template <typename ArgLHS, typename MatcherRHS>
std::enable_if_t<Detail::is_generic_matcher_v<MatcherRHS>, Detail::MatchAllOfGeneric<MatcherBase<ArgLHS>, MatcherRHS>>
operator&&(MatcherBase<ArgLHS> const& lhs CATCH_ATTR_LIFETIMEBOUND, MatcherRHS const& rhs CATCH_ATTR_LIFETIMEBOUND) {
    return {lhs, rhs};
}

template <typename MatcherLHS, typename ArgRHS>
std::enable_if_t<Detail::is_generic_matcher_v<MatcherLHS>, Detail::MatchAnyOfGeneric<MatcherLHS, MatcherBase<ArgRHS>>>
operator||(MatcherLHS const& lhs CATCH_ATTR_LIFETIMEBOUND, MatcherBase<ArgRHS> const& rhs CATCH_ATTR_LIFETIMEBOUND) {
    return {lhs, rhs};
}

template <typename ArgLHS, typename MatcherRHS>
std::enable_if_t<Detail::is_generic_matcher_v<MatcherRHS>, Detail::MatchAnyOfGeneric<MatcherBase<ArgLHS>, MatcherRHS>>
operator||(MatcherBase<ArgLHS> const& lhs CATCH_ATTR_LIFETIMEBOUND, MatcherRHS const& rhs CATCH_ATTR_LIFETIMEBOUND) {
    return {lhs, rhs};
}

}
}

#endif  // CATCH_MATCHERS_TEMPLATED_HPP_INCLUDED
