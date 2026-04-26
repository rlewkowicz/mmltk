
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_REPORTER_SPEC_PARSER_HPP_INCLUDED
#define CATCH_REPORTER_SPEC_PARSER_HPP_INCLUDED

#include <catch2/interfaces/catch_interfaces_config.hpp>
#include <catch2/internal/catch_optional.hpp>
#include <catch2/internal/catch_stringref.hpp>

#include <map>
#include <string>
#include <vector>

namespace Catch {

enum class ColourMode : std::uint8_t;

namespace Detail {
std::vector<std::string> splitReporterSpec(StringRef reporterSpec);

Optional<ColourMode> stringToColourMode(StringRef colourMode);
}  // namespace Detail

class ReporterSpec {
    std::string m_name;
    Optional<std::string> m_outputFileName;
    Optional<ColourMode> m_colourMode;
    std::map<std::string, std::string> m_customOptions;

    friend bool operator==(ReporterSpec const& lhs, ReporterSpec const& rhs);
    friend bool operator!=(ReporterSpec const& lhs, ReporterSpec const& rhs) {
        return !(lhs == rhs);
    }

   public:
    ReporterSpec(std::string name, Optional<std::string> outputFileName, Optional<ColourMode> colourMode,
                 std::map<std::string, std::string> customOptions);

    std::string const& name() const {
        return m_name;
    }

    Optional<std::string> const& outputFile() const {
        return m_outputFileName;
    }

    Optional<ColourMode> const& colourMode() const {
        return m_colourMode;
    }

    std::map<std::string, std::string> const& customOptions() const {
        return m_customOptions;
    }
};

Optional<ReporterSpec> parseReporterSpec(StringRef reporterSpec);

}  // namespace Catch

#endif  // CATCH_REPORTER_SPEC_PARSER_HPP_INCLUDED
