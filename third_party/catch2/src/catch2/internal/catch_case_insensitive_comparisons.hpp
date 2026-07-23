

#ifndef CATCH_CASE_INSENSITIVE_COMPARISONS_HPP_INCLUDED
#define CATCH_CASE_INSENSITIVE_COMPARISONS_HPP_INCLUDED

#include <catch2/internal/catch_stringref.hpp>

namespace Catch {
namespace Detail {
struct CaseInsensitiveLess {
    bool operator()(StringRef lhs, StringRef rhs) const;
};

struct CaseInsensitiveEqualTo {
    bool operator()(StringRef lhs, StringRef rhs) const;
};

}  
}  

#endif  // CATCH_CASE_INSENSITIVE_COMPARISONS_HPP_INCLUDED
