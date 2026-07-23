

#ifndef CATCH_MATCHERS_QUANTIFIERS_HPP_INCLUDED
#define CATCH_MATCHERS_QUANTIFIERS_HPP_INCLUDED

#include <catch2/matchers/catch_matchers_templated.hpp>
#include <catch2/internal/catch_move_and_forward.hpp>

namespace Catch {
namespace Matchers {
template <typename Matcher>
class AllMatchMatcher final : public MatcherGenericBase {
    Matcher m_matcher;

   public:
    AllMatchMatcher(Matcher matcher) : m_matcher(CATCH_MOVE(matcher)) {}

    std::string describe() const override {
        return "all match " + m_matcher.describe();
    }

    template <typename RangeLike>
    bool match(RangeLike&& rng) const {
        for (auto&& elem : rng) {
            if (!m_matcher.match(elem)) {
                return false;
            }
        }
        return true;
    }
};

template <typename Matcher>
class NoneMatchMatcher final : public MatcherGenericBase {
    Matcher m_matcher;

   public:
    NoneMatchMatcher(Matcher matcher) : m_matcher(CATCH_MOVE(matcher)) {}

    std::string describe() const override {
        return "none match " + m_matcher.describe();
    }

    template <typename RangeLike>
    bool match(RangeLike&& rng) const {
        for (auto&& elem : rng) {
            if (m_matcher.match(elem)) {
                return false;
            }
        }
        return true;
    }
};

template <typename Matcher>
class AnyMatchMatcher final : public MatcherGenericBase {
    Matcher m_matcher;

   public:
    AnyMatchMatcher(Matcher matcher) : m_matcher(CATCH_MOVE(matcher)) {}

    std::string describe() const override {
        return "any match " + m_matcher.describe();
    }

    template <typename RangeLike>
    bool match(RangeLike&& rng) const {
        for (auto&& elem : rng) {
            if (m_matcher.match(elem)) {
                return true;
            }
        }
        return false;
    }
};

class AllTrueMatcher final : public MatcherGenericBase {
   public:
    std::string describe() const override;

    template <typename RangeLike>
    bool match(RangeLike&& rng) const {
        for (auto&& elem : rng) {
            if (!elem) {
                return false;
            }
        }
        return true;
    }
};

class NoneTrueMatcher final : public MatcherGenericBase {
   public:
    std::string describe() const override;

    template <typename RangeLike>
    bool match(RangeLike&& rng) const {
        for (auto&& elem : rng) {
            if (elem) {
                return false;
            }
        }
        return true;
    }
};

class AnyTrueMatcher final : public MatcherGenericBase {
   public:
    std::string describe() const override;

    template <typename RangeLike>
    bool match(RangeLike&& rng) const {
        for (auto&& elem : rng) {
            if (elem) {
                return true;
            }
        }
        return false;
    }
};

template <typename Matcher>
AllMatchMatcher<Matcher> AllMatch(Matcher&& matcher) {
    return {CATCH_FORWARD(matcher)};
}

template <typename Matcher>
NoneMatchMatcher<Matcher> NoneMatch(Matcher&& matcher) {
    return {CATCH_FORWARD(matcher)};
}

template <typename Matcher>
AnyMatchMatcher<Matcher> AnyMatch(Matcher&& matcher) {
    return {CATCH_FORWARD(matcher)};
}

AllTrueMatcher AllTrue();

NoneTrueMatcher NoneTrue();

AnyTrueMatcher AnyTrue();
}  
}  

#endif  // CATCH_MATCHERS_QUANTIFIERS_HPP_INCLUDED
