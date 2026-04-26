
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_MATCHERS_RANGE_EQUALS_HPP_INCLUDED
#define CATCH_MATCHERS_RANGE_EQUALS_HPP_INCLUDED

#include <catch2/internal/catch_is_permutation.hpp>
#include <catch2/matchers/catch_matchers_templated.hpp>

#include <functional>

namespace Catch {
namespace Matchers {

template <typename TargetRangeLike, typename Equality>
class RangeEqualsMatcher final : public MatcherGenericBase {
    TargetRangeLike m_desired;
    Equality m_predicate;

   public:
    template <typename TargetRangeLike2, typename Equality2>
    constexpr RangeEqualsMatcher(TargetRangeLike2&& range, Equality2&& predicate)
        : m_desired(CATCH_FORWARD(range)), m_predicate(CATCH_FORWARD(predicate)) {}

    template <typename RangeLike>
    constexpr bool match(RangeLike&& rng) const {
        auto rng_start = begin(rng);
        const auto rng_end = end(rng);
        auto target_start = begin(m_desired);
        const auto target_end = end(m_desired);

        while (rng_start != rng_end && target_start != target_end) {
            if (!m_predicate(*rng_start, *target_start)) {
                return false;
            }
            ++rng_start;
            ++target_start;
        }
        return rng_start == rng_end && target_start == target_end;
    }

    std::string describe() const override {
        return "elements are " + Catch::Detail::stringify(m_desired);
    }
};

template <typename TargetRangeLike, typename Equality>
class UnorderedRangeEqualsMatcher final : public MatcherGenericBase {
    TargetRangeLike m_desired;
    Equality m_predicate;

   public:
    template <typename TargetRangeLike2, typename Equality2>
    constexpr UnorderedRangeEqualsMatcher(TargetRangeLike2&& range, Equality2&& predicate)
        : m_desired(CATCH_FORWARD(range)), m_predicate(CATCH_FORWARD(predicate)) {}

    template <typename RangeLike>
    constexpr bool match(RangeLike&& rng) const {
        using std::begin;
        using std::end;
        return Catch::Detail::is_permutation(begin(m_desired), end(m_desired), begin(rng), end(rng), m_predicate);
    }

    std::string describe() const override {
        return "unordered elements are " + ::Catch::Detail::stringify(m_desired);
    }
};

template <typename RangeLike, typename Equality = decltype(std::equal_to<>{})>
constexpr RangeEqualsMatcher<RangeLike, Equality> RangeEquals(RangeLike&& range,
                                                              Equality&& predicate = std::equal_to<>{}) {
    return {CATCH_FORWARD(range), CATCH_FORWARD(predicate)};
}

template <typename T, typename Equality = decltype(std::equal_to<>{})>
constexpr RangeEqualsMatcher<std::initializer_list<T>, Equality> RangeEquals(std::initializer_list<T> range,
                                                                             Equality&& predicate = std::equal_to<>{}) {
    return {range, CATCH_FORWARD(predicate)};
}

template <typename RangeLike, typename Equality = decltype(std::equal_to<>{})>
constexpr UnorderedRangeEqualsMatcher<RangeLike, Equality> UnorderedRangeEquals(
    RangeLike&& range, Equality&& predicate = std::equal_to<>{}) {
    return {CATCH_FORWARD(range), CATCH_FORWARD(predicate)};
}

template <typename T, typename Equality = decltype(std::equal_to<>{})>
constexpr UnorderedRangeEqualsMatcher<std::initializer_list<T>, Equality> UnorderedRangeEquals(
    std::initializer_list<T> range, Equality&& predicate = std::equal_to<>{}) {
    return {range, CATCH_FORWARD(predicate)};
}
}  // namespace Matchers
}  // namespace Catch

#endif  // CATCH_MATCHERS_RANGE_EQUALS_HPP_INCLUDED
