
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_REPORTER_HELPERS_HPP_INCLUDED
#define CATCH_REPORTER_HELPERS_HPP_INCLUDED

#include <iosfwd>
#include <string>
#include <vector>

#include <catch2/internal/catch_list.hpp>
#include <catch2/internal/catch_console_colour.hpp>
#include <catch2/interfaces/catch_interfaces_config.hpp>
#include <catch2/interfaces/catch_interfaces_reporter.hpp>
#include <catch2/catch_totals.hpp>

namespace Catch {

class IConfig;
class TestCaseHandle;
class ColourImpl;

std::string getFormattedDuration(double duration);

bool shouldShowDuration(IConfig const& config, double duration);

std::string serializeFilters(std::vector<std::string> const& filters);

enum class XmlAssertionBodyStyle {
    TextFlowIndented,
    PlainIndented,
};

bool shouldReportXmlAssertion(AssertionResult const& result);

StringRef xmlAssertionElementName(ResultWas::OfType resultType, bool okToFailOverride = false);

std::string formatXmlAssertionBody(AssertionStats const& stats, XmlAssertionBodyStyle style);

struct lineOfChars {
    char c;
    constexpr lineOfChars(char c_) : c(c_) {}

    friend std::ostream& operator<<(std::ostream& out, lineOfChars value);
};

namespace Detail {

class SharedAssertionPrinter {
   protected:
    SharedAssertionPrinter(std::ostream& stream, AssertionStats const& stats, bool printInfoMessages,
                           ColourImpl* colourImpl, Colour::Code dimColour);

    void resetMessages();
    void printExpressionWas();
    void printOriginalExpression() const;
    void printReconstructedExpression() const;
    void printReconstructedExpressionOneLine() const;
    void printExpressionAndMessages(bool reconstructedOnOneLine, Colour::Code expressionlessColour,
                                    Colour::Code remainingColour);
    void printMessage();
    void printRemainingMessages(Colour::Code colour);

    std::ostream& stream;
    AssertionResult const& result;
    std::vector<MessageInfo> const& messages;
    std::vector<MessageInfo>::const_iterator itMessage;
    bool printInfoMessages;
    ColourImpl* colourImpl;
    Colour::Code dimColour;
};

}  // namespace Detail

void defaultListReporters(std::ostream& out, std::vector<ReporterDescription> const& descriptions, Verbosity verbosity);

void defaultListListeners(std::ostream& out, std::vector<ListenerDescription> const& descriptions, Verbosity verbosity);

void defaultListTags(std::ostream& out, std::vector<TagInfo> const& tags, bool isFiltered, Verbosity verbosity);

void defaultListTests(std::ostream& out, ColourImpl* streamColour, std::vector<TestCaseHandle> const& tests,
                      bool isFiltered, Verbosity verbosity);

void printTestRunTotals(std::ostream& stream, ColourImpl& streamColour, Totals const& totals);

}  // namespace Catch

#endif  // CATCH_REPORTER_HELPERS_HPP_INCLUDED
