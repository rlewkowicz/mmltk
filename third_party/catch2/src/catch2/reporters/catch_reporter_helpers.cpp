
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0

#include <catch2/reporters/catch_reporter_helpers.hpp>
#include <catch2/interfaces/catch_interfaces_config.hpp>
#include <catch2/internal/catch_console_width.hpp>
#include <catch2/internal/catch_errno_guard.hpp>
#include <catch2/internal/catch_textflow.hpp>
#include <catch2/internal/catch_reusable_string_stream.hpp>
#include <catch2/internal/catch_string_manip.hpp>
#include <catch2/internal/catch_console_colour.hpp>
#include <catch2/catch_tostring.hpp>
#include <catch2/catch_test_case_info.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ostream>
#include <iomanip>

namespace Catch {

namespace {
template <typename Description, typename NameGetter>
std::size_t maxDescriptionNameLength(std::vector<Description> const& descriptions, NameGetter nameGetter) {
    if (descriptions.empty()) {
        return 0;
    }

    const auto iter = std::max_element(descriptions.begin(), descriptions.end(),
                                       [nameGetter](Description const& lhs, Description const& rhs) {
                                           return nameGetter(lhs).size() < nameGetter(rhs).size();
                                       });
    return nameGetter(*iter).size();
}

template <typename Description, typename NameGetter, typename DescriptionGetter>
void printDescriptionEntries(std::ostream& out, std::vector<Description> const& descriptions, NameGetter nameGetter,
                             DescriptionGetter descriptionGetter) {
    const auto maxNameLen = maxDescriptionNameLength(descriptions, nameGetter);
    for (auto const& desc : descriptions) {
        const std::string name = static_cast<std::string>(nameGetter(desc));
        out << TextFlow::Column(name + ':').indent(2).width(5 + maxNameLen) +
                   TextFlow::Column(descriptionGetter(desc))
                       .initialIndent(0)
                       .indent(2)
                       .width(CATCH_CONFIG_CONSOLE_WIDTH - maxNameLen - 8)
            << '\n';
    }
}

void listTestNamesOnly(std::ostream& out, std::vector<TestCaseHandle> const& tests) {
    for (auto const& test : tests) {
        auto const& testCaseInfo = test.getTestCaseInfo();

        if (startsWith(testCaseInfo.name, '#')) {
            out << '"' << testCaseInfo.name << '"';
        } else {
            out << testCaseInfo.name;
        }

        out << '\n';
    }
    out << std::flush;
}
}  // namespace

namespace Detail {

SharedAssertionPrinter::SharedAssertionPrinter(std::ostream& stream_, AssertionStats const& stats,
                                               bool printInfoMessages_, ColourImpl* colourImpl_,
                                               Colour::Code dimColour_)
    : stream(stream_),
      result(stats.assertionResult),
      messages(stats.infoMessages),
      itMessage(messages.begin()),
      printInfoMessages(printInfoMessages_),
      colourImpl(colourImpl_),
      dimColour(dimColour_) {}

void SharedAssertionPrinter::resetMessages() {
    itMessage = messages.begin();
}

void SharedAssertionPrinter::printExpressionWas() {
    if (result.hasExpression()) {
        stream << ';' << colourImpl->guardColour(dimColour) << " expression was:";
        printOriginalExpression();
    }
}

void SharedAssertionPrinter::printOriginalExpression() const {
    if (result.hasExpression()) {
        stream << ' ' << result.getExpression();
    }
}

void SharedAssertionPrinter::printReconstructedExpression() const {
    if (result.hasExpandedExpression()) {
        stream << colourImpl->guardColour(dimColour) << " for: ";
        stream << result.getExpandedExpression();
    }
}

void SharedAssertionPrinter::printReconstructedExpressionOneLine() const {
    if (result.hasExpandedExpression()) {
        stream << colourImpl->guardColour(dimColour) << " for: ";
        std::string expr = result.getExpandedExpression();
        std::replace(expr.begin(), expr.end(), '\n', ' ');
        stream << expr;
    }
}

void SharedAssertionPrinter::printExpressionAndMessages(bool reconstructedOnOneLine, Colour::Code expressionlessColour,
                                                        Colour::Code remainingColour) {
    printOriginalExpression();
    if (reconstructedOnOneLine) {
        printReconstructedExpressionOneLine();
    } else {
        printReconstructedExpression();
    }
    printRemainingMessages(result.hasExpression() ? remainingColour : expressionlessColour);
}

void SharedAssertionPrinter::printMessage() {
    if (itMessage != messages.end()) {
        stream << " '" << itMessage->message << '\'';
        ++itMessage;
    }
}

void SharedAssertionPrinter::printRemainingMessages(Colour::Code colour) {
    if (itMessage == messages.end()) {
        return;
    }

    const auto itEnd = messages.cend();
    const auto messageCount = static_cast<std::size_t>(itEnd - itMessage);
    stream << colourImpl->guardColour(colour) << " with " << pluralise(messageCount, "message"_sr) << ':';

    while (itMessage != itEnd) {
        if (printInfoMessages || itMessage->type != ResultWas::Info) {
            printMessage();
            if (itMessage != itEnd) {
                stream << colourImpl->guardColour(dimColour) << " and";
            }
            continue;
        }
        ++itMessage;
    }
}

}  // namespace Detail

std::string getFormattedDuration(double duration) {
    const std::size_t maxDoubleSize = DBL_MAX_10_EXP + 1 + 1 + 3 + 1;
    char buffer[maxDoubleSize];

    ErrnoGuard guard;
#ifdef _MSC_VER
    size_t printedLength = static_cast<size_t>(sprintf_s(buffer, "%.3f", duration));
#else
    size_t printedLength = static_cast<size_t>(std::snprintf(buffer, maxDoubleSize, "%.3f", duration));
#endif
    return std::string(buffer, printedLength);
}

bool shouldShowDuration(IConfig const& config, double duration) {
    if (config.showDurations() == ShowDurations::Always) {
        return true;
    }
    if (config.showDurations() == ShowDurations::Never) {
        return false;
    }
    const double min = config.minDuration();
    return min >= 0 && duration >= min;
}

std::string serializeFilters(std::vector<std::string> const& filters) {
    size_t serialized_size = filters.size() - 1;
    for (auto const& filter : filters) {
        serialized_size += filter.size();
    }

    std::string serialized;
    serialized.reserve(serialized_size);
    bool first = true;

    for (auto const& filter : filters) {
        if (!first) {
            serialized.push_back(' ');
        }
        first = false;
        serialized.append(filter);
    }

    return serialized;
}

bool shouldReportXmlAssertion(AssertionResult const& result) {
    return !result.isOk() || result.getResultType() == ResultWas::ExplicitSkip;
}

StringRef xmlAssertionElementName(ResultWas::OfType resultType, bool okToFailOverride) {
    if (okToFailOverride) {
        return "skipped"_sr;
    }

    switch (resultType) {
        case ResultWas::ThrewException:
        case ResultWas::FatalErrorCondition:
            return "error"_sr;
        case ResultWas::ExplicitFailure:
        case ResultWas::ExpressionFailed:
        case ResultWas::DidntThrowException:
            return "failure"_sr;
        case ResultWas::ExplicitSkip:
            return "skipped"_sr;
        case ResultWas::Info:
        case ResultWas::Warning:
        case ResultWas::Ok:
        case ResultWas::Unknown:
        case ResultWas::FailureBit:
        case ResultWas::Exception:
            return "internalError"_sr;
    }

    return "internalError"_sr;
}

std::string formatXmlAssertionBody(AssertionStats const& stats, XmlAssertionBodyStyle style) {
    AssertionResult const& result = stats.assertionResult;
    ReusableStringStream rss;

    if (result.getResultType() == ResultWas::ExplicitSkip) {
        rss << "SKIPPED\n";
    } else {
        rss << "FAILED:\n";
        if (result.hasExpression()) {
            if (style == XmlAssertionBodyStyle::TextFlowIndented) {
                rss << "  " << result.getExpressionInMacro() << '\n';
            } else {
                rss << '\t' << result.getExpressionInMacro() << '\n';
            }
        }
        if (result.hasExpandedExpression()) {
            rss << "with expansion:\n";
            if (style == XmlAssertionBodyStyle::TextFlowIndented) {
                rss << TextFlow::Column(result.getExpandedExpression()).indent(2) << '\n';
            } else {
                rss << '\t' << result.getExpandedExpression() << '\n';
            }
        }
    }

    if (result.hasMessage()) {
        rss << result.getMessage() << '\n';
    }
    for (auto const& msg : stats.infoMessages) {
        if (msg.type == ResultWas::Info) {
            rss << msg.message << '\n';
        }
    }

    rss << "at " << result.getSourceInfo();
    return rss.str();
}

std::ostream& operator<<(std::ostream& out, lineOfChars value) {
    for (size_t idx = 0; idx < CATCH_CONFIG_CONSOLE_WIDTH - 1; ++idx) {
        out.put(value.c);
    }
    return out;
}

void defaultListReporters(std::ostream& out, std::vector<ReporterDescription> const& descriptions,
                          Verbosity verbosity) {
    out << "Available reporters:\n";
    if (verbosity == Verbosity::Quiet) {
        const auto maxNameLen =
            maxDescriptionNameLength(descriptions, [](ReporterDescription const& desc) { return desc.name; });
        for (auto const& desc : descriptions) {
            out << TextFlow::Column(desc.name).indent(2).width(5 + maxNameLen) << '\n';
        }
    } else {
        printDescriptionEntries(
            out, descriptions, [](ReporterDescription const& desc) -> std::string const& { return desc.name; },
            [](ReporterDescription const& desc) -> std::string const& { return desc.description; });
    }
    out << '\n' << std::flush;
}

void defaultListListeners(std::ostream& out, std::vector<ListenerDescription> const& descriptions,
                          Verbosity verbosity) {
    if (verbosity == Verbosity::Quiet) {
        for (auto const& desc : descriptions) {
            out << desc.name << '\n';
        }
        return;
    }

    out << "Registered listeners:\n";

    if (descriptions.empty()) {
        return;
    }

    printDescriptionEntries(
        out, descriptions, [](ListenerDescription const& desc) { return desc.name; },
        [](ListenerDescription const& desc) -> std::string const& { return desc.description; });

    out << '\n' << std::flush;
}

void defaultListTags(std::ostream& out, std::vector<TagInfo> const& tags, bool isFiltered, Verbosity verbosity) {
    if (verbosity == Verbosity::Quiet) {
        for (auto const& tagCount : tags) {
            out << tagCount.all() << '\n';
        }
        return;
    }
    if (isFiltered) {
        out << "Tags for matching test cases:\n";
    } else {
        out << "All available tags:\n";
    }

    size_t maxTagCountLen = 2;

    if (!tags.empty()) {
        const auto maxTagCount = std::max_element(tags.begin(), tags.end(), [](auto const& lhs, auto const& rhs) {
                                     return lhs.count < rhs.count;
                                 })->count;

        if (maxTagCount >= 100) {
            auto numDigits = 1 + std::floor(std::log10(maxTagCount));
            maxTagCountLen = static_cast<size_t>(numDigits);
        }
    }

    for (auto const& tagCount : tags) {
        ReusableStringStream rss;
        rss << "  " << std::setw(maxTagCountLen) << tagCount.count << "  ";
        auto str = rss.str();
        auto wrapper =
            TextFlow::Column(tagCount.all()).initialIndent(0).indent(str.size()).width(CATCH_CONFIG_CONSOLE_WIDTH - 10);
        out << str << wrapper << '\n';
    }
    out << pluralise(tags.size(), "tag"_sr) << "\n\n" << std::flush;
}

void defaultListTests(std::ostream& out, ColourImpl* streamColour, std::vector<TestCaseHandle> const& tests,
                      bool isFiltered, Verbosity verbosity) {
    if (verbosity == Verbosity::Quiet) {
        listTestNamesOnly(out, tests);
        return;
    }

    if (isFiltered) {
        out << "Matching test cases:\n";
    } else {
        out << "All available test cases:\n";
    }

    for (auto const& test : tests) {
        auto const& testCaseInfo = test.getTestCaseInfo();
        Colour::Code colour = testCaseInfo.isHidden() ? Colour::SecondaryText : Colour::None;
        auto colourGuard = streamColour->guardColour(colour).engage(out);

        out << TextFlow::Column(testCaseInfo.name).indent(2) << '\n';
        if (verbosity >= Verbosity::High) {
            out << TextFlow::Column(Catch::Detail::stringify(testCaseInfo.lineInfo)).indent(4) << '\n';
        }
        if (!testCaseInfo.tags.empty() && verbosity > Verbosity::Quiet) {
            out << TextFlow::Column(testCaseInfo.tagsAsString()).indent(6) << '\n';
        }
    }

    if (isFiltered) {
        out << pluralise(tests.size(), "matching test case"_sr);
    } else {
        out << pluralise(tests.size(), "test case"_sr);
    }
    out << "\n\n" << std::flush;
}

namespace {
class SummaryColumn {
   public:
    SummaryColumn(std::string suffix, Colour::Code colour) : m_suffix(CATCH_MOVE(suffix)), m_colour(colour) {}

    SummaryColumn&& addRow(std::uint64_t count) && {
        std::string row = std::to_string(count);
        auto const new_width = std::max(m_width, row.size());
        if (new_width > m_width) {
            for (auto& oldRow : m_rows) {
                oldRow.insert(0, new_width - m_width, ' ');
            }
        } else {
            row.insert(0, m_width - row.size(), ' ');
        }
        m_width = new_width;
        m_rows.push_back(row);
        return std::move(*this);
    }

    std::string const& getSuffix() const {
        return m_suffix;
    }
    Colour::Code getColour() const {
        return m_colour;
    }
    std::string const& getRow(std::size_t index) const {
        return m_rows[index];
    }

   private:
    std::string m_suffix;
    Colour::Code m_colour;
    std::size_t m_width = 0;
    std::vector<std::string> m_rows;
};

void printSummaryRow(std::ostream& stream, ColourImpl& colour, StringRef label, std::vector<SummaryColumn> const& cols,
                     std::size_t row) {
    for (auto const& col : cols) {
        auto const& value = col.getRow(row);
        auto const& suffix = col.getSuffix();
        if (suffix.empty()) {
            stream << label << ": ";
            if (value != "0") {
                stream << value;
            } else {
                stream << colour.guardColour(Colour::Warning) << "- none -";
            }
        } else if (value != "0") {
            stream << colour.guardColour(Colour::LightGrey) << " | " << colour.guardColour(col.getColour()) << value
                   << ' ' << suffix;
        }
    }
    stream << '\n';
}
}  // namespace

void printTestRunTotals(std::ostream& stream, ColourImpl& streamColour, Totals const& totals) {
    if (totals.testCases.total() == 0) {
        stream << streamColour.guardColour(Colour::Warning) << "No tests ran\n";
        return;
    }

    if (totals.assertions.total() > 0 && totals.testCases.allPassed()) {
        stream << streamColour.guardColour(Colour::ResultSuccess) << "All tests passed";
        stream << " (" << pluralise(totals.assertions.passed, "assertion"_sr) << " in "
               << pluralise(totals.testCases.passed, "test case"_sr) << ')' << '\n';
        return;
    }

    std::vector<SummaryColumn> columns;
    const auto totalAssertionCount = totals.assertions.total() - totals.assertions.skipped;
    columns.push_back(SummaryColumn("", Colour::None).addRow(totals.testCases.total()).addRow(totalAssertionCount));
    columns.push_back(
        SummaryColumn("passed", Colour::Success).addRow(totals.testCases.passed).addRow(totals.assertions.passed));
    columns.push_back(
        SummaryColumn("failed", Colour::ResultError).addRow(totals.testCases.failed).addRow(totals.assertions.failed));
    columns.push_back(SummaryColumn("skipped", Colour::Skip).addRow(totals.testCases.skipped).addRow(0));
    columns.push_back(SummaryColumn("failed as expected", Colour::ResultExpectedFailure)
                          .addRow(totals.testCases.failedButOk)
                          .addRow(totals.assertions.failedButOk));
    printSummaryRow(stream, streamColour, "test cases"_sr, columns, 0);
    printSummaryRow(stream, streamColour, "assertions"_sr, columns, 1);
}

}  // namespace Catch
