
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#include <catch2/reporters/catch_reporter_multi.hpp>

#include <catch2/catch_config.hpp>
#include <catch2/internal/catch_move_and_forward.hpp>
#include <catch2/internal/catch_stdstreams.hpp>

#include <ostream>

namespace Catch {
namespace {

template <typename Callback>
void for_each_reporter_like(std::vector<IEventListenerPtr>& reporterLikes, Callback&& callback) {
    for (auto& reporterish : reporterLikes) {
        callback(*reporterish);
    }
}

}  // namespace

void MultiReporter::updatePreferences(IEventListener const& reporterish) {
    m_preferences.shouldRedirectStdOut |= reporterish.getPreferences().shouldRedirectStdOut;
    m_preferences.shouldReportAllAssertions |= reporterish.getPreferences().shouldReportAllAssertions;
    m_preferences.shouldReportAllAssertionStarts |= reporterish.getPreferences().shouldReportAllAssertionStarts;
}

void MultiReporter::addListener(IEventListenerPtr&& listener) {
    updatePreferences(*listener);
    m_reporterLikes.insert(m_reporterLikes.begin() + m_insertedListeners, CATCH_MOVE(listener));
    ++m_insertedListeners;
}

void MultiReporter::addReporter(IEventListenerPtr&& reporter) {
    updatePreferences(*reporter);

    m_haveNoncapturingReporters |= !reporter->getPreferences().shouldRedirectStdOut;

    m_reporterLikes.push_back(CATCH_MOVE(reporter));
}

void MultiReporter::noMatchingTestCases(StringRef unmatchedSpec) {
    for_each_reporter_like(m_reporterLikes,
                           [&](IEventListener& reporterish) { reporterish.noMatchingTestCases(unmatchedSpec); });
}

void MultiReporter::fatalErrorEncountered(StringRef error) {
    for_each_reporter_like(m_reporterLikes,
                           [&](IEventListener& reporterish) { reporterish.fatalErrorEncountered(error); });
}

void MultiReporter::reportInvalidTestSpec(StringRef arg) {
    for_each_reporter_like(m_reporterLikes,
                           [&](IEventListener& reporterish) { reporterish.reportInvalidTestSpec(arg); });
}

void MultiReporter::benchmarkPreparing(StringRef name) {
    for_each_reporter_like(m_reporterLikes, [&](IEventListener& reporterish) { reporterish.benchmarkPreparing(name); });
}
void MultiReporter::benchmarkStarting(BenchmarkInfo const& benchmarkInfo) {
    for_each_reporter_like(m_reporterLikes,
                           [&](IEventListener& reporterish) { reporterish.benchmarkStarting(benchmarkInfo); });
}
void MultiReporter::benchmarkEnded(BenchmarkStats<> const& benchmarkStats) {
    for_each_reporter_like(m_reporterLikes,
                           [&](IEventListener& reporterish) { reporterish.benchmarkEnded(benchmarkStats); });
}

void MultiReporter::benchmarkFailed(StringRef error) {
    for_each_reporter_like(m_reporterLikes, [&](IEventListener& reporterish) { reporterish.benchmarkFailed(error); });
}

void MultiReporter::testRunStarting(TestRunInfo const& testRunInfo) {
    for_each_reporter_like(m_reporterLikes,
                           [&](IEventListener& reporterish) { reporterish.testRunStarting(testRunInfo); });
}

void MultiReporter::testCaseStarting(TestCaseInfo const& testInfo) {
    for_each_reporter_like(m_reporterLikes,
                           [&](IEventListener& reporterish) { reporterish.testCaseStarting(testInfo); });
}

void MultiReporter::testCasePartialStarting(TestCaseInfo const& testInfo, uint64_t partNumber) {
    for_each_reporter_like(m_reporterLikes, [&](IEventListener& reporterish) {
        reporterish.testCasePartialStarting(testInfo, partNumber);
    });
}

void MultiReporter::sectionStarting(SectionInfo const& sectionInfo) {
    for_each_reporter_like(m_reporterLikes,
                           [&](IEventListener& reporterish) { reporterish.sectionStarting(sectionInfo); });
}

void MultiReporter::assertionStarting(AssertionInfo const& assertionInfo) {
    for_each_reporter_like(m_reporterLikes,
                           [&](IEventListener& reporterish) { reporterish.assertionStarting(assertionInfo); });
}

void MultiReporter::assertionEnded(AssertionStats const& assertionStats) {
    const bool reportByDefault =
        assertionStats.assertionResult.getResultType() != ResultWas::Ok || m_config->includeSuccessfulResults();

    for_each_reporter_like(m_reporterLikes, [&](IEventListener& reporterish) {
        if (reportByDefault || reporterish.getPreferences().shouldReportAllAssertions) {
            reporterish.assertionEnded(assertionStats);
        }
    });
}

void MultiReporter::sectionEnded(SectionStats const& sectionStats) {
    for_each_reporter_like(m_reporterLikes,
                           [&](IEventListener& reporterish) { reporterish.sectionEnded(sectionStats); });
}

void MultiReporter::testCasePartialEnded(TestCaseStats const& testStats, uint64_t partNumber) {
    if (m_preferences.shouldRedirectStdOut && m_haveNoncapturingReporters) {
        if (!testStats.stdOut.empty()) {
            Catch::cout() << testStats.stdOut << std::flush;
        }
        if (!testStats.stdErr.empty()) {
            Catch::cerr() << testStats.stdErr << std::flush;
        }
    }

    for_each_reporter_like(
        m_reporterLikes, [&](IEventListener& reporterish) { reporterish.testCasePartialEnded(testStats, partNumber); });
}

void MultiReporter::testCaseEnded(TestCaseStats const& testCaseStats) {
    for_each_reporter_like(m_reporterLikes,
                           [&](IEventListener& reporterish) { reporterish.testCaseEnded(testCaseStats); });
}

void MultiReporter::testRunEnded(TestRunStats const& testRunStats) {
    for_each_reporter_like(m_reporterLikes,
                           [&](IEventListener& reporterish) { reporterish.testRunEnded(testRunStats); });
}

void MultiReporter::skipTest(TestCaseInfo const& testInfo) {
    for_each_reporter_like(m_reporterLikes, [&](IEventListener& reporterish) { reporterish.skipTest(testInfo); });
}

void MultiReporter::listReporters(std::vector<ReporterDescription> const& descriptions) {
    for_each_reporter_like(m_reporterLikes,
                           [&](IEventListener& reporterish) { reporterish.listReporters(descriptions); });
}

void MultiReporter::listListeners(std::vector<ListenerDescription> const& descriptions) {
    for_each_reporter_like(m_reporterLikes,
                           [&](IEventListener& reporterish) { reporterish.listListeners(descriptions); });
}

void MultiReporter::listTests(std::vector<TestCaseHandle> const& tests) {
    for_each_reporter_like(m_reporterLikes, [&](IEventListener& reporterish) { reporterish.listTests(tests); });
}

void MultiReporter::listTags(std::vector<TagInfo> const& tags) {
    for_each_reporter_like(m_reporterLikes, [&](IEventListener& reporterish) { reporterish.listTags(tags); });
}

}  // namespace Catch
