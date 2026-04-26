
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#include <catch2/reporters/catch_reporter_junit.hpp>

#include <catch2/reporters/catch_reporter_helpers.hpp>
#include <catch2/internal/catch_string_manip.hpp>
#include <catch2/interfaces/catch_interfaces_config.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/catch_test_spec.hpp>
#include <catch2/internal/catch_move_and_forward.hpp>

#include <cassert>
#include <ctime>
#include <algorithm>

namespace Catch {

namespace {
std::string getCurrentTimestamp() {
    time_t rawtime;
    std::time(&rawtime);

    std::tm timeInfo = {};
#if defined(_MSC_VER) || defined(__MINGW32__)
    gmtime_s(&timeInfo, &rawtime);
#elif defined(CATCH_PLATFORM_PLAYSTATION)
    gmtime_s(&rawtime, &timeInfo);
#elif defined(__IAR_SYSTEMS_ICC__)
    timeInfo = *std::gmtime(&rawtime);
#else
    gmtime_r(&rawtime, &timeInfo);
#endif

    auto const timeStampSize = sizeof("2017-01-16T17:06:45Z");
    char timeStamp[timeStampSize];
    const char* const fmt = "%Y-%m-%dT%H:%M:%SZ";

    std::strftime(timeStamp, timeStampSize, fmt, &timeInfo);

    return std::string(timeStamp, timeStampSize - 1);
}

std::string fileNameTag(std::vector<Tag> const& tags) {
    auto it = std::find_if(begin(tags), end(tags),
                           [](Tag const& tag) { return tag.original.size() > 0 && tag.original[0] == '#'; });
    if (it != tags.end()) {
        return static_cast<std::string>(it->original.substr(1, it->original.size() - 1));
    }
    return std::string();
}

static void normalizeNamespaceMarkers(std::string& str) {
    std::size_t pos = str.find("::");
    while (pos != std::string::npos) {
        str.replace(pos, 2, ".");
        pos += 1;
        pos = str.find("::", pos);
    }
}

}  // namespace

JunitReporter::JunitReporter(ReporterConfig&& _config) : CumulativeReporterBase(CATCH_MOVE(_config)), xml(m_stream) {
    m_preferences.shouldRedirectStdOut = true;
    m_preferences.shouldReportAllAssertions = false;
    m_preferences.shouldReportAllAssertionStarts = false;
    m_shouldStoreSuccesfulAssertions = false;
}

std::string JunitReporter::getDescription() {
    return "Reports test results in an XML format that looks like Ant's junitreport target";
}

void JunitReporter::testRunStarting(TestRunInfo const& runInfo) {
    CumulativeReporterBase::testRunStarting(runInfo);
    xml.startElement("testsuites");
    suiteTimer.start();
    stdOutForSuite.clear();
    stdErrForSuite.clear();
    unexpectedExceptions = 0;
}

void JunitReporter::testCaseStarting(TestCaseInfo const& testCaseInfo) {
    m_okToFail = testCaseInfo.okToFail();
}

void JunitReporter::assertionEnded(AssertionStats const& assertionStats) {
    if (assertionStats.assertionResult.getResultType() == ResultWas::ThrewException && !m_okToFail)
        unexpectedExceptions++;
    CumulativeReporterBase::assertionEnded(assertionStats);
}

void JunitReporter::testCaseEnded(TestCaseStats const& testCaseStats) {
    stdOutForSuite += testCaseStats.stdOut;
    stdErrForSuite += testCaseStats.stdErr;
    CumulativeReporterBase::testCaseEnded(testCaseStats);
}

void JunitReporter::testRunEndedCumulative() {
    const auto suiteTime = suiteTimer.getElapsedSeconds();
    writeRun(*m_testRun, suiteTime);
    xml.endElement();
}

void JunitReporter::writeRun(TestRunNode const& testRunNode, double suiteTime) {
    XmlWriter::ScopedElement e = xml.scopedElement("testsuite");

    TestRunStats const& stats = testRunNode.value;
    xml.writeAttribute("name"_sr, stats.runInfo.name);
    xml.writeAttribute("errors"_sr, unexpectedExceptions);
    xml.writeAttribute("failures"_sr, stats.totals.assertions.failed - unexpectedExceptions);
    xml.writeAttribute("skipped"_sr, stats.totals.assertions.skipped);
    xml.writeAttribute("tests"_sr, stats.totals.assertions.total());
    xml.writeAttribute("hostname"_sr, "tbd"_sr);
    if (m_config->showDurations() == ShowDurations::Never)
        xml.writeAttribute("time"_sr, ""_sr);
    else
        xml.writeAttribute("time"_sr, getFormattedDuration(suiteTime));
    xml.writeAttribute("timestamp"_sr, getCurrentTimestamp());

    {
        auto properties = xml.scopedElement("properties");
        xml.scopedElement("property")
            .writeAttribute("name"_sr, "random-seed"_sr)
            .writeAttribute("value"_sr, m_config->rngSeed());
        if (m_config->testSpec().hasFilters()) {
            xml.scopedElement("property")
                .writeAttribute("name"_sr, "filters"_sr)
                .writeAttribute("value"_sr, m_config->testSpec());
        }
    }

    for (auto const& child : testRunNode.children)
        writeTestCase(*child);

    xml.scopedElement("system-out").writeText(trim(stdOutForSuite), XmlFormatting::Newline);
    xml.scopedElement("system-err").writeText(trim(stdErrForSuite), XmlFormatting::Newline);
}

void JunitReporter::writeTestCase(TestCaseNode const& testCaseNode) {
    TestCaseStats const& stats = testCaseNode.value;

    assert(testCaseNode.children.size() == 1);
    SectionNode const& rootSection = *testCaseNode.children.front();

    std::string className = static_cast<std::string>(stats.testInfo->className);

    if (className.empty()) {
        className = fileNameTag(stats.testInfo->tags);
        if (className.empty()) {
            className = "global";
        }
    }

    if (!m_config->name().empty())
        className = static_cast<std::string>(m_config->name()) + '.' + className;

    normalizeNamespaceMarkers(className);

    writeSection(className, "", rootSection, stats.testInfo->okToFail());
}

void JunitReporter::writeSection(std::string const& className, std::string const& rootName,
                                 SectionNode const& sectionNode, bool testOkToFail) {
    std::string name = trim(sectionNode.stats.sectionInfo.name);
    if (!rootName.empty())
        name = rootName + '/' + name;

    if (sectionNode.stats.assertions.total() > 0 || !sectionNode.stdOut.empty() || !sectionNode.stdErr.empty()) {
        XmlWriter::ScopedElement e = xml.scopedElement("testcase");
        if (className.empty()) {
            xml.writeAttribute("classname"_sr, name);
            xml.writeAttribute("name"_sr, "root"_sr);
        } else {
            xml.writeAttribute("classname"_sr, className);
            xml.writeAttribute("name"_sr, name);
        }
        xml.writeAttribute("time"_sr, getFormattedDuration(sectionNode.stats.durationInSeconds));
        xml.writeAttribute("status"_sr, "run"_sr);

        if (sectionNode.stats.assertions.failedButOk) {
            xml.scopedElement("skipped").writeAttribute("message", "TEST_CASE tagged with !mayfail");
        }

        writeAssertions(sectionNode);

        if (!sectionNode.stdOut.empty())
            xml.scopedElement("system-out").writeText(trim(sectionNode.stdOut), XmlFormatting::Newline);
        if (!sectionNode.stdErr.empty())
            xml.scopedElement("system-err").writeText(trim(sectionNode.stdErr), XmlFormatting::Newline);
    }
    for (auto const& childNode : sectionNode.childSections)
        if (className.empty())
            writeSection(name, "", *childNode, testOkToFail);
        else
            writeSection(className, name, *childNode, testOkToFail);
}

void JunitReporter::writeAssertions(SectionNode const& sectionNode) {
    for (auto const& assertionOrBenchmark : sectionNode.assertionsAndBenchmarks) {
        if (assertionOrBenchmark.isAssertion()) {
            writeAssertion(assertionOrBenchmark.asAssertion());
        }
    }
}

void JunitReporter::writeAssertion(AssertionStats const& stats) {
    AssertionResult const& result = stats.assertionResult;
    if (!shouldReportXmlAssertion(result)) {
        return;
    }

    XmlWriter::ScopedElement e =
        xml.scopedElement(static_cast<std::string>(xmlAssertionElementName(result.getResultType())));
    xml.writeAttribute("message"_sr, result.getExpression());
    xml.writeAttribute("type"_sr, result.getTestMacroName());
    xml.writeText(formatXmlAssertionBody(stats, XmlAssertionBodyStyle::TextFlowIndented), XmlFormatting::Newline);
}

}  // namespace Catch
