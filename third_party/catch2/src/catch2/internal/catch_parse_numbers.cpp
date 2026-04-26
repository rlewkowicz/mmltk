
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0

#include <catch2/internal/catch_compiler_capabilities.hpp>
#include <catch2/internal/catch_parse_numbers.hpp>
#include <catch2/internal/catch_string_manip.hpp>

#include <limits>
#include <stdexcept>

namespace Catch {

Optional<unsigned int> parseUInt(std::string const& input, int base) {
    auto trimmed = trim(input);
    if (trimmed.empty() || trimmed[0] == '-') {
        return {};
    }

    CATCH_TRY {
        size_t pos = 0;
        const auto ret = std::stoull(trimmed, &pos, base);

        if (pos != trimmed.size()) {
            return {};
        }
        if (ret > std::numeric_limits<unsigned int>::max()) {
            return {};
        }
        return static_cast<unsigned int>(ret);
    }
    CATCH_CATCH_ANON(std::invalid_argument const&) {}
    CATCH_CATCH_ANON(std::out_of_range const&) {}
    return {};
}

}  // namespace Catch
