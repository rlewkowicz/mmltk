

#ifndef CATCH_MATCHERS_CONTAINS_HPP_INCLUDED
#define CATCH_MATCHERS_CONTAINS_HPP_INCLUDED

#include <catch2/matchers/catch_matchers_templated.hpp>
#include <catch2/internal/catch_move_and_forward.hpp>

#include <functional>
#include <type_traits>

namespace Catch {
namespace Matchers {
template <typename T, typename Equality>
class ContainsElementMatcher final : public MatcherGenericBase {
    T m_desired;
    Equality m_eq;

   public:
    template <typename T2, typename Equality2>
    ContainsElementMatcher(T2&& target, Equality2&& predicate)
        : m_desired(CATCH_FORWARD(target)), m_eq(CATCH_FORWARD(predicate)) {}

    std::string describe() const override {
        return "contains element " + Catch::Detail::stringify(m_desired);
    }

    template <typename RangeLike>
    bool match(RangeLike&& rng) const {
        for (auto&& elem : rng) {
            if (m_eq(elem, m_desired)) {
                return true;
            }
        }
        return false;
    }
};

template <typename Matcher>
class ContainsMatcherMatcher final : public MatcherGenericBase {
    Matcher m_matcher;

   public:
    ContainsMatcherMatcher(Matcher matcher) : m_matcher(CATCH_MOVE(matcher)) {}

    template <typename RangeLike>
    bool match(RangeLike&& rng) const {
        for (auto&& elem : rng) {
            if (m_matcher.match(elem)) {
                return true;
            }
        }
        return false;
    }

    std::string describe() const override {
        return "contains element matching " + m_matcher.describe();
    }
};

template <typename T>
std::enable_if_t<!Detail::is_matcher_v<T>, ContainsElementMatcher<T, std::equal_to<>>> Contains(T&& elem) {
    return {CATCH_FORWARD(elem), std::equal_to<>{}};
}

template <typename Matcher>
std::enable_if_t<Detail::is_matcher_v<Matcher>, ContainsMatcherMatcher<Matcher>> Contains(Matcher&& matcher) {
    return {CATCH_FORWARD(matcher)};
}

template <typename T, typename Equality>
ContainsElementMatcher<T, Equality> Contains(T&& elem, Equality&& eq) {
    return {CATCH_FORWARD(elem), CATCH_FORWARD(eq)};
}

}  
}  

#endif  // CATCH_MATCHERS_CONTAINS_HPP_INCLUDED
