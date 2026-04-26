
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_REPORTER_STREAMING_BASE_HPP_INCLUDED
#define CATCH_REPORTER_STREAMING_BASE_HPP_INCLUDED

#include <catch2/reporters/catch_reporter_common_base.hpp>
#include <catch2/internal/catch_move_and_forward.hpp>

#include <vector>

namespace Catch {

class StreamingReporterBase : public ReporterBase {
   public:
    StreamingReporterBase(ReporterConfig&& _config) : ReporterBase(CATCH_MOVE(_config)) {}
    ~StreamingReporterBase() override;

    void benchmarkPreparing(StringRef) override {}
    void benchmarkStarting(BenchmarkInfo const&) override {}
    void benchmarkEnded(BenchmarkStats<> const&) override {}
    void benchmarkFailed(StringRef) override {}

    void fatalErrorEncountered(StringRef) override {}
    void noMatchingTestCases(StringRef) override {}
    void reportInvalidTestSpec(StringRef) override {}

    void testRunStarting(TestRunInfo const& _testRunInfo) override;

    void testCaseStarting(TestCaseInfo const& _testInfo) override {
        currentTestCaseInfo = &_testInfo;
    }
    void testCasePartialStarting(TestCaseInfo const&, uint64_t) override {}
    void sectionStarting(SectionInfo const& _sectionInfo) override {
        m_sectionStack.push_back(_sectionInfo);
    }

    void assertionStarting(AssertionInfo const&) override {}
    void assertionEnded(AssertionStats const&) override {}

    void sectionEnded(SectionStats const&) override {
        m_sectionStack.pop_back();
    }
    void testCasePartialEnded(TestCaseStats const&, uint64_t) override {}
    void testCaseEnded(TestCaseStats const&) override {
        currentTestCaseInfo = nullptr;
    }
    void testRunEnded(TestRunStats const&) override;

    void skipTest(TestCaseInfo const&) override {}

   protected:
    TestRunInfo currentTestRunInfo{"test run has not started yet"_sr};
    TestCaseInfo const* currentTestCaseInfo = nullptr;

    std::vector<SectionInfo> m_sectionStack;
};

}  // namespace Catch

#endif  // CATCH_REPORTER_STREAMING_BASE_HPP_INCLUDED
