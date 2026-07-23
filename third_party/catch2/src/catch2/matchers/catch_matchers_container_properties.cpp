

#include <catch2/matchers/catch_matchers_container_properties.hpp>
#include <catch2/internal/catch_reusable_string_stream.hpp>

namespace Catch {
namespace Matchers {

std::string IsEmptyMatcher::describe() const {
    return "is empty";
}

std::string HasSizeMatcher::describe() const {
    ReusableStringStream sstr;
    sstr << "has size == " << m_target_size;
    return sstr.str();
}

IsEmptyMatcher IsEmpty() {
    return {};
}

HasSizeMatcher SizeIs(std::size_t sz) {
    return HasSizeMatcher{sz};
}

}  
}  
