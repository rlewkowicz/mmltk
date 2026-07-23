

#ifndef CATCH_MATCHERS_CONTAINER_PROPERTIES_HPP_INCLUDED
#define CATCH_MATCHERS_CONTAINER_PROPERTIES_HPP_INCLUDED

#include <catch2/matchers/catch_matchers_templated.hpp>
#include <catch2/internal/catch_container_nonmembers.hpp>
#include <catch2/internal/catch_move_and_forward.hpp>

namespace Catch {
namespace Matchers {
namespace Detail {
template <typename RangeLike>
auto rangeSize(RangeLike&& rng) -> decltype(size(rng)) {
#if defined(CATCH_CONFIG_POLYFILL_NONMEMBER_CONTAINER_ACCESS)
    using Catch::Detail::size;
#else
    using std::size;
#endif
    return size(rng);
}
}  

class IsEmptyMatcher final : public MatcherGenericBase {
   public:
    template <typename RangeLike>
    bool match(RangeLike&& rng) const {
#if defined(CATCH_CONFIG_POLYFILL_NONMEMBER_CONTAINER_ACCESS)
        using Catch::Detail::empty;
#else
        using std::empty;
#endif
        return empty(rng);
    }

    std::string describe() const override;
};

class HasSizeMatcher final : public MatcherGenericBase {
    std::size_t m_target_size;

   public:
    explicit HasSizeMatcher(std::size_t target_size) : m_target_size(target_size) {}

    template <typename RangeLike>
    bool match(RangeLike&& rng) const {
        return Detail::rangeSize(rng) == m_target_size;
    }

    std::string describe() const override;
};

template <typename Matcher>
class SizeMatchesMatcher final : public MatcherGenericBase {
    Matcher m_matcher;

   public:
    explicit SizeMatchesMatcher(Matcher m) : m_matcher(CATCH_MOVE(m)) {}

    template <typename RangeLike>
    bool match(RangeLike&& rng) const {
        return m_matcher.match(Detail::rangeSize(rng));
    }

    std::string describe() const override {
        return "size matches " + m_matcher.describe();
    }
};

IsEmptyMatcher IsEmpty();
HasSizeMatcher SizeIs(std::size_t sz);
template <typename Matcher>
std::enable_if_t<Detail::is_matcher_v<Matcher>, SizeMatchesMatcher<Matcher>> SizeIs(Matcher&& m) {
    return SizeMatchesMatcher<Matcher>{CATCH_FORWARD(m)};
}

}  
}  

#endif  // CATCH_MATCHERS_CONTAINER_PROPERTIES_HPP_INCLUDED
