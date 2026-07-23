

#ifndef CATCH_PATH_FILTER_HPP_INCLUDED
#define CATCH_PATH_FILTER_HPP_INCLUDED

#include <catch2/internal/catch_move_and_forward.hpp>

#include <cstdint>
#include <string>

namespace Catch {

struct PathFilter {
    enum class For : std::uint8_t {
        Section,
        Generator,
    };
    // cppcheck-suppress passedByValue
    PathFilter(For type_, std::string filter_) : type(type_), filter(CATCH_MOVE(filter_)) {}

    For type;
    std::string filter;

    friend bool operator==(PathFilter const& lhs, PathFilter const& rhs);
};

}  

#endif  // CATCH_PATH_FILTER_HPP_INCLUDED
