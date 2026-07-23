

#include <catch2/matchers/catch_matchers_quantifiers.hpp>

namespace Catch {
namespace Matchers {
std::string AllTrueMatcher::describe() const {
    return "contains only true";
}

AllTrueMatcher AllTrue() {
    return AllTrueMatcher{};
}

std::string NoneTrueMatcher::describe() const {
    return "contains no true";
}

NoneTrueMatcher NoneTrue() {
    return NoneTrueMatcher{};
}

std::string AnyTrueMatcher::describe() const {
    return "contains at least one true";
}

AnyTrueMatcher AnyTrue() {
    return AnyTrueMatcher{};
}
}  
}  
