

#ifndef CATCH_UNREACHABLE_HPP_INCLUDED
#define CATCH_UNREACHABLE_HPP_INCLUDED

#include <exception>

#if defined(__cpp_lib_unreachable) && __cpp_lib_unreachable > 202202L
#include <utility>
namespace Catch {
namespace Detail {
using Unreachable = std::unreachable;
}
}  

#else  // vv If we do not have std::unreachable, we implement something similar

namespace Catch {
namespace Detail {

[[noreturn]]
inline void Unreachable() noexcept {
#if defined(NDEBUG)
#if defined(_MSC_VER) && !defined(__clang__)
    __assume(false);
#elif defined(__GNUC__)
    __builtin_unreachable();
#else  // vv platform without known optimization hint
    std::terminate();
#endif
#else   // ^^ NDEBUG
    std::terminate();
#endif  //
}

}  
}  

#endif

#endif  // CATCH_UNREACHABLE_HPP_INCLUDED
