

#ifndef CATCH_MATCHERS_RANGE_EQUALS_HPP_INCLUDED
#define CATCH_MATCHERS_RANGE_EQUALS_HPP_INCLUDED

#include <catch2/internal/catch_is_permutation.hpp>
#include <catch2/matchers/catch_matchers_templated.hpp>
#include <functional>

namespace Catch {
namespace Matchers {

namespace Detail {

// Ordered comparison: elementwise equality in order, same length.
template <typename TargetRangeLike, typename RangeLike, typename Equality>
constexpr bool matchesRange(std::true_type, TargetRangeLike const& desired, RangeLike const& rng,
                            Equality const& predicate) {
    using std::begin;
    using std::end;
    auto rng_start = begin(rng);
    const auto rng_end = end(rng);
    auto target_start = begin(desired);
    const auto target_end = end(desired);

    while (rng_start != rng_end && target_start != target_end) {
        if (!predicate(*rng_start, *target_start)) {
            return false;
        }
        ++rng_start;
        ++target_start;
    }
    return rng_start == rng_end && target_start == target_end;
}

// Unordered comparison: the ranges are permutations of each other.
template <typename TargetRangeLike, typename RangeLike, typename Equality>
constexpr bool matchesRange(std::false_type, TargetRangeLike const& desired, RangeLike const& rng,
                            Equality const& predicate) {
    using std::begin;
    using std::end;
    return Catch::Detail::is_permutation(begin(desired), end(desired), begin(rng), end(rng), predicate);
}

}

// Shared implementation of RangeEqualsMatcher / UnorderedRangeEqualsMatcher;
// Ordered selects which comparison Detail::matchesRange performs.
template <bool Ordered, typename TargetRangeLike, typename Equality>
class RangeEqualsMatcherImpl final : public MatcherGenericBase {
    TargetRangeLike m_desired;
    Equality m_predicate;

   public:
    template <typename TargetRangeLike2, typename Equality2>
    constexpr RangeEqualsMatcherImpl(TargetRangeLike2&& range, Equality2&& predicate)
        : m_desired(CATCH_FORWARD(range)), m_predicate(CATCH_FORWARD(predicate)) {}

    template <typename RangeLike>
    constexpr bool match(RangeLike&& rng) const {
        return Detail::matchesRange(std::integral_constant<bool, Ordered>{}, m_desired, rng, m_predicate);
    }

    std::string describe() const override {
        return (Ordered ? "elements are " : "unordered elements are ") + ::Catch::Detail::stringify(m_desired);
    }
};

template <typename TargetRangeLike, typename Equality>
using RangeEqualsMatcher = RangeEqualsMatcherImpl<true, TargetRangeLike, Equality>;
template <typename TargetRangeLike, typename Equality>
using UnorderedRangeEqualsMatcher = RangeEqualsMatcherImpl<false, TargetRangeLike, Equality>;

// The ordered and unordered factories differ only in the matcher they return,
// so both overload pairs (deduced range, and braced initializer list) are
// generated from a single definition.
#define CATCH_MATCHERS_RANGE_EQUALS_FACTORY(factory, matcher)                                                     \
    template <typename RangeLike, typename Equality = decltype(std::equal_to<>{})>                                \
    constexpr matcher<RangeLike, Equality> factory(RangeLike&& range, Equality&& predicate = std::equal_to<>{}) { \
        return {CATCH_FORWARD(range), CATCH_FORWARD(predicate)};                                                  \
    }                                                                                                             \
                                                                                                                  \
    template <typename T, typename Equality = decltype(std::equal_to<>{})>                                        \
    constexpr matcher<std::initializer_list<T>, Equality> factory(std::initializer_list<T> range,                 \
                                                                  Equality&& predicate = std::equal_to<>{}) {     \
        return {range, CATCH_FORWARD(predicate)};                                                                 \
    }

CATCH_MATCHERS_RANGE_EQUALS_FACTORY(RangeEquals, RangeEqualsMatcher)
CATCH_MATCHERS_RANGE_EQUALS_FACTORY(UnorderedRangeEquals, UnorderedRangeEqualsMatcher)

#undef CATCH_MATCHERS_RANGE_EQUALS_FACTORY
}
}

#endif  // CATCH_MATCHERS_RANGE_EQUALS_HPP_INCLUDED
