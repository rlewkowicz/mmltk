

#include <catch2/reporters/catch_reporter_streaming_base.hpp>

namespace Catch {

StreamingReporterBase::~StreamingReporterBase() = default;

void StreamingReporterBase::testRunStarting(TestRunInfo const& _testRunInfo) {
    currentTestRunInfo = _testRunInfo;
}

void StreamingReporterBase::testRunEnded(TestRunStats const&) {
    currentTestCaseInfo = nullptr;
}

}  
