

#include <catch2/reporters/catch_reporter_automake.hpp>
#include <catch2/catch_test_case_info.hpp>

#include <ostream>

namespace Catch {

AutomakeReporter::~AutomakeReporter() = default;

void AutomakeReporter::testCaseEnded(TestCaseStats const& _testCaseStats) {
    m_stream << ":test-result: ";
    if (_testCaseStats.totals.testCases.skipped > 0) {
        m_stream << "SKIP";
    } else if (_testCaseStats.totals.assertions.allPassed()) {
        m_stream << "PASS";
    } else if (_testCaseStats.totals.assertions.allOk()) {
        m_stream << "XFAIL";
    } else {
        m_stream << "FAIL";
    }
    m_stream << ' ' << _testCaseStats.testInfo->name << '\n';
    StreamingReporterBase::testCaseEnded(_testCaseStats);
}

void AutomakeReporter::skipTest(TestCaseInfo const& testInfo) {
    m_stream << ":test-result: SKIP " << testInfo.name << '\n';
}

}  
