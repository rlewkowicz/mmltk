

#include <catch2/catch_config.hpp>
#include <catch2/internal/catch_move_and_forward.hpp>
#include <catch2/internal/catch_stdstreams.hpp>
#include <catch2/reporters/catch_reporter_multi.hpp>
#include <ostream>

namespace Catch {
namespace {

template <typename Callback>
void for_each_reporter_like(std::vector<IEventListenerPtr>& reporterLikes, Callback&& callback) {
    for (auto& reporterish : reporterLikes) {
        callback(*reporterish);
    }
}

}

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

#define CATCH_MULTI_FORWARD_EVENT1(name, Arg1Type, arg1)                                                       \
    void MultiReporter::name(Arg1Type arg1) {                                                                  \
        for_each_reporter_like(m_reporterLikes, [&](IEventListener& reporterish) { reporterish.name(arg1); }); \
    }
#define CATCH_MULTI_FORWARD_EVENT2(name, Arg1Type, arg1, Arg2Type, arg2)                                             \
    void MultiReporter::name(Arg1Type arg1, Arg2Type arg2) {                                                         \
        for_each_reporter_like(m_reporterLikes, [&](IEventListener& reporterish) { reporterish.name(arg1, arg2); }); \
    }

CATCH_REPORTER_FORWARDED_EVENTS(CATCH_MULTI_FORWARD_EVENT1, CATCH_MULTI_FORWARD_EVENT2)

#undef CATCH_MULTI_FORWARD_EVENT1
#undef CATCH_MULTI_FORWARD_EVENT2

void MultiReporter::assertionEnded(AssertionStats const& assertionStats) {
    const bool reportByDefault =
        assertionStats.assertionResult.getResultType() != ResultWas::Ok || m_config->includeSuccessfulResults();

    for_each_reporter_like(m_reporterLikes, [&](IEventListener& reporterish) {
        if (reportByDefault || reporterish.getPreferences().shouldReportAllAssertions) {
            reporterish.assertionEnded(assertionStats);
        }
    });
}

void MultiReporter::testCasePartialEnded(TestCaseStats const& testCaseStats, uint64_t partNumber) {
    if (m_preferences.shouldRedirectStdOut && m_haveNoncapturingReporters) {
        if (!testCaseStats.stdOut.empty()) {
            Catch::cout() << testCaseStats.stdOut << std::flush;
        }
        if (!testCaseStats.stdErr.empty()) {
            Catch::cerr() << testCaseStats.stdErr << std::flush;
        }
    }

    for_each_reporter_like(m_reporterLikes, [&](IEventListener& reporterish) {
        reporterish.testCasePartialEnded(testCaseStats, partNumber);
    });
}

}
