

#ifndef CATCH_TO_STRING_HPP_INCLUDED
#define CATCH_TO_STRING_HPP_INCLUDED

#include <string>

#include <catch2/internal/catch_compiler_capabilities.hpp>
#include <catch2/internal/catch_reusable_string_stream.hpp>

namespace Catch {
template <typename T>
std::string to_string(T const& t) {
#if defined(CATCH_CONFIG_CPP11_TO_STRING)
    return std::to_string(t);
#else
    ReusableStringStream rss;
    rss << t;
    return rss.str();
#endif
}
}  

#endif  // CATCH_TO_STRING_HPP_INCLUDED
