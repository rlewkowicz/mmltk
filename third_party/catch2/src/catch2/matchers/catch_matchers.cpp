

#include <catch2/matchers/catch_matchers.hpp>

namespace Catch {
namespace Matchers {

std::string MatcherUntypedBase::toString() const {
    if (m_cachedToString.empty()) {
        m_cachedToString = describe();
    }
    return m_cachedToString;
}

MatcherUntypedBase::~MatcherUntypedBase() = default;

}  
}  
