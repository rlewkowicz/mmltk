

#ifndef CATCH_CASE_SENSITIVE_HPP_INCLUDED
#define CATCH_CASE_SENSITIVE_HPP_INCLUDED

#include <cstdint>

namespace Catch {

enum class CaseSensitive : std::uint8_t {
    Yes,
    No
};

}  

#endif  // CATCH_CASE_SENSITIVE_HPP_INCLUDED
