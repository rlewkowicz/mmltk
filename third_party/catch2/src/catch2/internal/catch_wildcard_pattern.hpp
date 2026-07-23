

#ifndef CATCH_WILDCARD_PATTERN_HPP_INCLUDED
#define CATCH_WILDCARD_PATTERN_HPP_INCLUDED

#include <catch2/catch_case_sensitive.hpp>

#include <cstdint>
#include <string>

namespace Catch {
class WildcardPattern {
    enum WildcardPosition : std::uint8_t {
        NoWildcard = 0,
        WildcardAtStart = 1,
        WildcardAtEnd = 2,
        WildcardAtBothEnds = WildcardAtStart | WildcardAtEnd
    };

   public:
    WildcardPattern(std::string const& pattern, CaseSensitive caseSensitivity);
    bool matches(std::string const& str) const;

   private:
    std::string normaliseString(std::string const& str) const;
    CaseSensitive m_caseSensitivity;
    WildcardPosition m_wildcard = NoWildcard;
    std::string m_pattern;
};
}  

#endif  // CATCH_WILDCARD_PATTERN_HPP_INCLUDED
