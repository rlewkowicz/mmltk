
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#include <catch2/reporters/catch_reporter_sonarqube.hpp>

#include <catch2/internal/catch_string_manip.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/internal/catch_reusable_string_stream.hpp>
#include <catch2/interfaces/catch_interfaces_config.hpp>
#include <catch2/catch_test_spec.hpp>
#include <catch2/reporters/catch_reporter_helpers.hpp>

#include <map>

namespace Catch {

namespace {
std::string createMetadataString(IConfig const& config) {
    ReusableStringStream sstr;
    if (config.testSpec().hasFilters()) {
        sstr << "filters='" << config.testSpec() << "' ";
    }
    sstr << "rng-seed=" << config.rngSeed();
    return sstr.str();
}
}  // namespace

void SonarQubeReporter::testRunStarting(TestRunInfo const& testRunInfo) {
    CumulativeReporterBase::testRunStarting(testRunInfo);

    xml.writeComment(createMetadataString(*m_config));
    xml.startElement("testExecutions");
    xml.writeAttribute("version"_sr, '1');
}

void SonarQubeReporter::writeRun(TestRunNode const& runNode) {
    std::map<StringRef, std::vector<TestCaseNode const*>> testsPerFile;

    for (auto const& child : runNode.children) {
        testsPerFile[child->value.testInfo->lineInfo.file].push_back(child.get());
    }

    for (auto const& kv : testsPerFile) {
        writeTestFile(kv.first, kv.second);
    }
}

void SonarQubeReporter::writeTestFile(StringRef filename, std::vector<TestCaseNode const*> const& testCaseNodes) {
    XmlWriter::ScopedElement e = xml.scopedElement("file");
    xml.writeAttribute("path"_sr, filename);

    for (auto const& child : testCaseNodes)
        writeTestCase(*child);
}

void SonarQubeReporter::writeTestCase(TestCaseNode const& testCaseNode) {
    assert(testCaseNode.children.size() == 1);
    SectionNode const& rootSection = *testCaseNode.children.front();
    writeSection("", rootSection, testCaseNode.value.testInfo->okToFail());
}

void SonarQubeReporter::writeSection(std::string const& rootName, SectionNode const& sectionNode, bool okToFail) {
    std::string name = trim(sectionNode.stats.sectionInfo.name);
    if (!rootName.empty())
        name = rootName + '/' + name;

    if (sectionNode.stats.assertions.total() > 0 || !sectionNode.stdOut.empty() || !sectionNode.stdErr.empty()) {
        XmlWriter::ScopedElement e = xml.scopedElement("testCase");
        xml.writeAttribute("name"_sr, name);
        xml.writeAttribute("duration"_sr, static_cast<long>(sectionNode.stats.durationInSeconds * 1000));

        writeAssertions(sectionNode, okToFail);
    }

    for (auto const& childNode : sectionNode.childSections)
        writeSection(name, *childNode, okToFail);
}

void SonarQubeReporter::writeAssertions(SectionNode const& sectionNode, bool okToFail) {
    for (auto const& assertionOrBenchmark : sectionNode.assertionsAndBenchmarks) {
        if (assertionOrBenchmark.isAssertion()) {
            writeAssertion(assertionOrBenchmark.asAssertion(), okToFail);
        }
    }
}

void SonarQubeReporter::writeAssertion(AssertionStats const& stats, bool okToFail) {
    AssertionResult const& result = stats.assertionResult;
    if (!shouldReportXmlAssertion(result)) {
        return;
    }

    XmlWriter::ScopedElement e =
        xml.scopedElement(static_cast<std::string>(xmlAssertionElementName(result.getResultType(), okToFail)));

    ReusableStringStream messageRss;
    messageRss << result.getTestMacroName() << '(' << result.getExpression() << ')';
    xml.writeAttribute("message"_sr, messageRss.str());
    xml.writeText(formatXmlAssertionBody(stats, XmlAssertionBodyStyle::PlainIndented), XmlFormatting::Newline);
}

}  // namespace Catch
