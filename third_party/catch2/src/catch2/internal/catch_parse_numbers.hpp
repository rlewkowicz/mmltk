

#ifndef CATCH_PARSE_NUMBERS_HPP_INCLUDED
#define CATCH_PARSE_NUMBERS_HPP_INCLUDED

#include <catch2/internal/catch_optional.hpp>

#include <string>

namespace Catch {

Optional<unsigned int> parseUInt(std::string const& input, int base = 10);
}

#endif  // CATCH_PARSE_NUMBERS_HPP_INCLUDED
