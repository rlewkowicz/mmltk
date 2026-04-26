
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#include <catch2/internal/catch_run_context.hpp>

#include <catch2/catch_user_config.hpp>
#include <catch2/generators/catch_generators_throw.hpp>
#include <catch2/interfaces/catch_interfaces_config.hpp>
#include <catch2/interfaces/catch_interfaces_generatortracker.hpp>
#include <catch2/interfaces/catch_interfaces_reporter.hpp>
#include <catch2/internal/catch_compiler_capabilities.hpp>
#include <catch2/internal/catch_context.hpp>
#include <catch2/internal/catch_enforce.hpp>
#include <catch2/internal/catch_fatal_condition_handler.hpp>
#include <catch2/internal/catch_random_number_generator.hpp>
#include <catch2/catch_timer.hpp>
#include <catch2/internal/catch_output_redirect.hpp>
#include <catch2/internal/catch_assertion_handler.hpp>
#include <catch2/internal/catch_path_filter.hpp>
#include <catch2/internal/catch_test_failure_exception.hpp>
#include <catch2/internal/catch_thread_local.hpp>
#include <catch2/internal/catch_unreachable.hpp>
#include <catch2/internal/catch_result_type.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cassert>
#include <algorithm>

namespace Catch {

namespace Generators {
namespace {
struct GeneratorTracker final : TestCaseTracking::TrackerBase, IGeneratorTracker {
    GeneratorBasePtr m_generator;
    bool m_isFiltered = false;

    GeneratorTracker(TestCaseTracking::NameAndLocation&& nameAndLocation, TrackerContext& ctx, ITracker* parent,
                     GeneratorBasePtr&& generator)
        : TrackerBase(CATCH_MOVE(nameAndLocation), ctx, parent), m_generator(CATCH_MOVE(generator)) {
        assert(m_generator && "Cannot create tracker without generator");

        if (m_newStyleFilters && m_allTrackerDepth < m_filterRef->size()) {
            auto const& filter = (*m_filterRef)[m_allTrackerDepth];
            if (filter.type == PathFilter::For::Section) {
                SKIP();
            }
            if (filter.filter != "*") {
                m_isFiltered = true;
                size_t targetIndex = std::stoul(filter.filter);
                m_generator->skipToNthElement(targetIndex);
            }
        }
    }

    static GeneratorTracker* acquire(TrackerContext& ctx, TestCaseTracking::NameAndLocationRef const& nameAndLocation) {
        GeneratorTracker* tracker;

        ITracker& currentTracker = ctx.currentTracker();
        if (currentTracker.nameAndLocation() == nameAndLocation) {
            auto thisTracker = currentTracker.parent()->findChild(nameAndLocation);
            assert(thisTracker);
            assert(thisTracker->isGeneratorTracker());
            tracker = static_cast<GeneratorTracker*>(thisTracker);
        } else if (ITracker* childTracker = currentTracker.findChild(nameAndLocation)) {
            assert(childTracker);
            assert(childTracker->isGeneratorTracker());
            tracker = static_cast<GeneratorTracker*>(childTracker);
        } else {
            return nullptr;
        }

        if (!tracker->isComplete()) {
            tracker->open();
        }

        return tracker;
    }

    bool isGeneratorTracker() const override {
        return true;
    }
    void close() override {
        TrackerBase::close();
        const bool should_wait_for_child = [&]() {
            if (m_children.empty()) {
                return false;
            }
            if (std::find_if(m_children.begin(), m_children.end(), [](TestCaseTracking::ITrackerPtr const& tracker) {
                    return tracker->hasStarted();
                }) != m_children.end()) {
                return false;
            }

            size_t childDepth = 1 + (m_newStyleFilters ? m_allTrackerDepth : m_sectionOnlyDepth);
            if (childDepth >= m_filterRef->size()) {
                return true;
            }

            if (m_newStyleFilters && (*m_filterRef)[childDepth].type != PathFilter::For::Section) {
                return false;
            }
            for (auto const& child : m_children) {
                if (child->isSectionTracker() && static_cast<SectionTracker const&>(*child).trimmedName() ==
                                                     StringRef((*m_filterRef)[childDepth].filter)) {
                    return true;
                }
            }
            return false;
        }();

        assert(m_generator && "Tracker without generator");
        if (should_wait_for_child ||
            (m_runState == CompletedSuccessfully && !m_isFiltered && m_generator->countedNext())) {
            m_children.clear();
            m_runState = Executing;
        }
    }

    auto getGenerator() const -> GeneratorBasePtr const& override {
        return m_generator;
    }
};
}  // namespace
}  // namespace Generators

namespace Detail {

static CATCH_INTERNAL_THREAD_LOCAL bool g_lastAssertionPassed = false;

static CATCH_INTERNAL_THREAD_LOCAL SourceLineInfo g_lastKnownLineInfo("DummyLocation", static_cast<size_t>(-1));

static CATCH_INTERNAL_THREAD_LOCAL bool g_clearMessageScopes = false;

class MessageHolder {
    std::vector<MessageInfo> messages;
    std::vector<unsigned int> unscoped_ids;

   public:
    ~MessageHolder() = default;

    void addUnscopedMessage(MessageInfo&& info) {
        repairUnscopedMessageInvariant();
        unscoped_ids.push_back(info.sequence);
        messages.push_back(CATCH_MOVE(info));
    }

    void addUnscopedMessage(MessageBuilder&& builder) {
        MessageInfo info(CATCH_MOVE(builder.m_info));
        info.message = builder.m_stream.str();
        addUnscopedMessage(CATCH_MOVE(info));
    }

    void addScopedMessage(MessageInfo&& info) {
        messages.push_back(CATCH_MOVE(info));
    }

    std::vector<MessageInfo> const& getMessages() const {
        return messages;
    }

    void removeMessage(unsigned int messageId) {
        auto iter = std::find_if(messages.begin(), messages.end(),
                                 [messageId](MessageInfo const& msg) { return msg.sequence == messageId; });
        assert(iter != messages.end() && "Trying to remove non-existent message.");
        messages.erase(iter);
    }

    void removeUnscopedMessages() {
        for (const auto messageId : unscoped_ids) {
            removeMessage(messageId);
        }
        unscoped_ids.clear();
        g_clearMessageScopes = false;
    }

    void repairUnscopedMessageInvariant() {
        if (g_clearMessageScopes) {
            removeUnscopedMessages();
        }
        g_clearMessageScopes = false;
    }
};

CATCH_INTERNAL_START_WARNINGS_SUPPRESSION
CATCH_INTERNAL_SUPPRESS_GLOBALS_WARNINGS
static MessageHolder& g_messageHolder() {
    static CATCH_INTERNAL_THREAD_LOCAL MessageHolder value;
    return value;
}
CATCH_INTERNAL_STOP_WARNINGS_SUPPRESSION

}  // namespace Detail

RunContext::RunContext(IConfig const* _config, IEventListenerPtr&& reporter)
    : m_runInfo(_config->name()),
      m_config(_config),
      m_reporter(CATCH_MOVE(reporter)),
      m_outputRedirect(makeOutputRedirect(m_reporter->getPreferences().shouldRedirectStdOut)),
      m_abortAfterXFailedAssertions(m_config->abortAfter()),
      m_reportAssertionStarting(m_reporter->getPreferences().shouldReportAllAssertionStarts),
      m_includeSuccessfulResults(m_config->includeSuccessfulResults() ||
                                 m_reporter->getPreferences().shouldReportAllAssertions),
      m_shouldDebugBreak(m_config->shouldDebugBreak()) {
    getCurrentMutableContext().setResultCapture(this);
    m_reporter->testRunStarting(m_runInfo);

    // TODO: HACK!
    ReusableStringStream rss;
    (void)rss;
}

RunContext::~RunContext() {
    updateTotalsFromAtomics();
    m_reporter->testRunEnded(TestRunStats(m_runInfo, m_totals, aborting()));
}

Totals RunContext::runTest(TestCaseHandle const& testCase) {
    updateTotalsFromAtomics();
    const Totals prevTotals = m_totals;

    auto const& testInfo = testCase.getTestCaseInfo();
    m_reporter->testCaseStarting(testInfo);
    testCase.prepareTestCase();
    m_activeTestCase = &testCase;

    ITracker& rootTracker = m_trackerContext.startRun();
    assert(rootTracker.isSectionTracker());
    rootTracker.setFilters(&m_config->getPathFilters(), m_config->useNewFilterBehaviour());

    seedRng(*m_config);

    uint64_t testRuns = 0;
    std::string redirectedCout;
    std::string redirectedCerr;
    do {
        m_trackerContext.startCycle();
        m_testCaseTracker = &SectionTracker::acquire(
            m_trackerContext, TestCaseTracking::NameAndLocationRef(testInfo.name, testInfo.lineInfo));

        m_reporter->testCasePartialStarting(testInfo, testRuns);

        updateTotalsFromAtomics();
        const auto beforeRunTotals = m_totals;
        runCurrentTest();
        std::string oneRunCout = m_outputRedirect->getStdout();
        std::string oneRunCerr = m_outputRedirect->getStderr();
        m_outputRedirect->clearBuffers();
        redirectedCout += oneRunCout;
        redirectedCerr += oneRunCerr;

        updateTotalsFromAtomics();
        const auto singleRunTotals = m_totals.delta(beforeRunTotals);
        auto statsForOneRun =
            TestCaseStats(testInfo, singleRunTotals, CATCH_MOVE(oneRunCout), CATCH_MOVE(oneRunCerr), aborting());
        m_reporter->testCasePartialEnded(statsForOneRun, testRuns);

        ++testRuns;
    } while (!m_testCaseTracker->isSuccessfullyCompleted() && !aborting());

    Totals deltaTotals = m_totals.delta(prevTotals);
    if (testInfo.expectedToFail() && deltaTotals.testCases.passed > 0) {
        deltaTotals.assertions.failed++;
        deltaTotals.testCases.passed--;
        deltaTotals.testCases.failed++;
    }
    m_totals.testCases += deltaTotals.testCases;
    testCase.tearDownTestCase();
    m_reporter->testCaseEnded(
        TestCaseStats(testInfo, deltaTotals, CATCH_MOVE(redirectedCout), CATCH_MOVE(redirectedCerr), aborting()));

    m_activeTestCase = nullptr;
    m_testCaseTracker = nullptr;

    return deltaTotals;
}

void RunContext::assertionEnded(AssertionResult&& result) {
    Detail::g_lastKnownLineInfo = result.m_info.lineInfo;
    if (result.getResultType() == ResultWas::Ok) {
        m_atomicAssertionCount.passed++;
        Detail::g_lastAssertionPassed = true;
    } else if (result.getResultType() == ResultWas::ExplicitSkip) {
        m_atomicAssertionCount.skipped++;
        Detail::g_lastAssertionPassed = true;
    } else if (!result.succeeded()) {
        Detail::g_lastAssertionPassed = false;
        if (result.isOk()) {
        } else if (m_activeTestCase->getTestCaseInfo().okToFail())
            m_atomicAssertionCount.failedButOk++;
        else
            m_atomicAssertionCount.failed++;
    } else {
        Detail::g_lastAssertionPassed = true;
    }

    auto& msgHolder = Detail::g_messageHolder();
    msgHolder.repairUnscopedMessageInvariant();

    Detail::LockGuard lock(m_assertionMutex);
    {
        auto _ = scopedDeactivate(*m_outputRedirect);
        updateTotalsFromAtomics();
        m_reporter->assertionEnded(AssertionStats(result, msgHolder.getMessages(), m_totals));
    }

    if (result.getResultType() != ResultWas::Warning) {
        msgHolder.removeUnscopedMessages();
    }

    m_lastResult = CATCH_MOVE(result);
}

void RunContext::notifyAssertionStarted(AssertionInfo const& info) {
    if (m_reportAssertionStarting) {
        Detail::LockGuard lock(m_assertionMutex);
        auto _ = scopedDeactivate(*m_outputRedirect);
        m_reporter->assertionStarting(info);
    }
}

bool RunContext::sectionStarted(StringRef sectionName, SourceLineInfo const& sectionLineInfo, Counts& assertions) {
    ITracker& sectionTracker =
        SectionTracker::acquire(m_trackerContext, TestCaseTracking::NameAndLocationRef(sectionName, sectionLineInfo));

    if (!sectionTracker.isOpen())
        return false;
    m_activeSections.push_back(&sectionTracker);

    SectionInfo sectionInfo(sectionLineInfo, static_cast<std::string>(sectionName));
    Detail::g_lastKnownLineInfo = sectionLineInfo;

    {
        auto _ = scopedDeactivate(*m_outputRedirect);
        m_reporter->sectionStarting(sectionInfo);
    }

    updateTotalsFromAtomics();
    assertions = m_totals.assertions;

    return true;
}
IGeneratorTracker* RunContext::acquireGeneratorTracker(StringRef generatorName, SourceLineInfo const& lineInfo) {
    auto* tracker = Generators::GeneratorTracker::acquire(
        m_trackerContext, TestCaseTracking::NameAndLocationRef(generatorName, lineInfo));
    Detail::g_lastKnownLineInfo = lineInfo;
    return tracker;
}

IGeneratorTracker* RunContext::createGeneratorTracker(StringRef generatorName, SourceLineInfo lineInfo,
                                                      Generators::GeneratorBasePtr&& generator) {
    if (m_config->warnAboutInfiniteGenerators() && !generator->isFinite()) {
        FAIL("GENERATE() would run infinitely");
    }

    auto nameAndLoc = TestCaseTracking::NameAndLocation(static_cast<std::string>(generatorName), lineInfo);
    auto& currentTracker = m_trackerContext.currentTracker();
    assert(currentTracker.nameAndLocation() != nameAndLoc &&
           "Trying to create tracker for a generator that already has one");

    auto newTracker = Catch::Detail::make_unique<Generators::GeneratorTracker>(CATCH_MOVE(nameAndLoc), m_trackerContext,
                                                                               &currentTracker, CATCH_MOVE(generator));
    auto ret = newTracker.get();
    currentTracker.addChild(CATCH_MOVE(newTracker));

    ret->open();
    return ret;
}

bool RunContext::testForMissingAssertions(Counts& assertions) {
    if (assertions.total() != 0)
        return false;
    if (!m_config->warnAboutMissingAssertions())
        return false;
    if (m_trackerContext.currentTracker().hasChildren())
        return false;
    m_atomicAssertionCount.failed++;
    assertions.failed++;
    return true;
}

void RunContext::sectionEnded(SectionEndInfo&& endInfo) {
    updateTotalsFromAtomics();
    Counts assertions = m_totals.assertions - endInfo.prevAssertions;
    bool missingAssertions = testForMissingAssertions(assertions);

    if (!m_activeSections.empty()) {
        m_activeSections.back()->close();
        m_activeSections.pop_back();
    }

    {
        auto _ = scopedDeactivate(*m_outputRedirect);
        m_reporter->sectionEnded(
            SectionStats(CATCH_MOVE(endInfo.sectionInfo), assertions, endInfo.durationInSeconds, missingAssertions));
    }
}

void RunContext::sectionEndedEarly(SectionEndInfo&& endInfo) {
    if (m_unfinishedSections.empty()) {
        m_activeSections.back()->fail();
    } else {
        m_activeSections.back()->close();
    }
    m_activeSections.pop_back();

    m_unfinishedSections.push_back(CATCH_MOVE(endInfo));
}

void RunContext::benchmarkPreparing(StringRef name) {
    auto _ = scopedDeactivate(*m_outputRedirect);
    m_reporter->benchmarkPreparing(name);
}
void RunContext::benchmarkStarting(BenchmarkInfo const& info) {
    auto _ = scopedDeactivate(*m_outputRedirect);
    m_reporter->benchmarkStarting(info);
}
void RunContext::benchmarkEnded(BenchmarkStats<> const& stats) {
    auto _ = scopedDeactivate(*m_outputRedirect);
    m_reporter->benchmarkEnded(stats);
}
void RunContext::benchmarkFailed(StringRef error) {
    auto _ = scopedDeactivate(*m_outputRedirect);
    m_reporter->benchmarkFailed(error);
}

std::string RunContext::getCurrentTestName() const {
    return m_activeTestCase ? m_activeTestCase->getTestCaseInfo().name : std::string();
}

const AssertionResult* RunContext::getLastResult() const {
    Detail::LockGuard _(m_assertionMutex);
    return &(*m_lastResult);
}

void RunContext::exceptionEarlyReported() {
    m_shouldReportUnexpected = false;
}

void RunContext::handleFatalErrorCondition(StringRef message) {
    {
        Detail::LockGuard lock(m_assertionMutex);
        // TODO: scoped deactivate here? Just give up and do best effort?
        auto _ = scopedDeactivate(*m_outputRedirect);

        m_reporter->fatalErrorEncountered(message);
    }

    AssertionResultData tempResult(ResultWas::FatalErrorCondition, {false});
    tempResult.message = static_cast<std::string>(message);
    AssertionResult result(makeDummyAssertionInfo(), CATCH_MOVE(tempResult));

    assertionEnded(CATCH_MOVE(result));

    Detail::LockGuard lock(m_assertionMutex);

    while (!m_activeSections.empty()) {
        auto const& nl = m_activeSections.back()->nameAndLocation();
        SectionEndInfo endInfo{SectionInfo(nl.location, nl.name), {}, 0.0};
        sectionEndedEarly(CATCH_MOVE(endInfo));
    }
    handleUnfinishedSections();

    auto const& testCaseInfo = m_activeTestCase->getTestCaseInfo();
    SectionInfo testCaseSection(testCaseInfo.lineInfo, testCaseInfo.name);

    Counts assertions;
    assertions.failed = 1;
    SectionStats testCaseSectionStats(CATCH_MOVE(testCaseSection), assertions, 0, false);
    m_reporter->sectionEnded(testCaseSectionStats);

    auto const& testInfo = m_activeTestCase->getTestCaseInfo();

    Totals deltaTotals;
    deltaTotals.testCases.failed = 1;
    deltaTotals.assertions.failed = 1;
    m_reporter->testCaseEnded(TestCaseStats(testInfo, deltaTotals, std::string(), std::string(), false));
    m_totals.testCases.failed++;
    updateTotalsFromAtomics();
    m_reporter->testRunEnded(TestRunStats(m_runInfo, m_totals, false));
}

bool RunContext::lastAssertionPassed() {
    return Detail::g_lastAssertionPassed;
}

void RunContext::assertionPassedFastPath(SourceLineInfo lineInfo) {
    Detail::g_lastKnownLineInfo = lineInfo;
    ++m_atomicAssertionCount.passed;
    Detail::g_lastAssertionPassed = true;
    Detail::g_clearMessageScopes = true;
}

void RunContext::updateTotalsFromAtomics() {
    m_totals.assertions = Counts{
        m_atomicAssertionCount.passed,
        m_atomicAssertionCount.failed,
        m_atomicAssertionCount.failedButOk,
        m_atomicAssertionCount.skipped,
    };
}

bool RunContext::aborting() const {
    return m_atomicAssertionCount.failed >= m_abortAfterXFailedAssertions;
}

void RunContext::runCurrentTest() {
    auto const& testCaseInfo = m_activeTestCase->getTestCaseInfo();
    SectionInfo testCaseSection(testCaseInfo.lineInfo, testCaseInfo.name);
    m_reporter->sectionStarting(testCaseSection);
    updateTotalsFromAtomics();
    Counts prevAssertions = m_totals.assertions;
    double duration = 0;
    m_shouldReportUnexpected = true;
    Detail::g_lastKnownLineInfo = testCaseInfo.lineInfo;

    Timer timer;
    CATCH_TRY {
        {
            auto _ = scopedActivate(*m_outputRedirect);
            timer.start();
            invokeActiveTestCase();
        }
        duration = timer.getElapsedSeconds();
    }
    CATCH_CATCH_ANON(TestFailureException&) {}
    CATCH_CATCH_ANON(TestSkipException&) {}
    CATCH_CATCH_ALL {
        if (m_shouldReportUnexpected) {
            AssertionReaction dummyReaction;
            handleUnexpectedInflightException(makeDummyAssertionInfo(), translateActiveException(), dummyReaction);
        }
    }
    updateTotalsFromAtomics();
    Counts assertions = m_totals.assertions - prevAssertions;
    bool missingAssertions = testForMissingAssertions(assertions);

    m_testCaseTracker->close();
    handleUnfinishedSections();
    auto& msgHolder = Detail::g_messageHolder();
    msgHolder.removeUnscopedMessages();
    assert(msgHolder.getMessages().empty() && "There should be no leftover messages after the test ends");

    SectionStats testCaseSectionStats(CATCH_MOVE(testCaseSection), assertions, duration, missingAssertions);
    m_reporter->sectionEnded(testCaseSectionStats);
}

void RunContext::invokeActiveTestCase() {
    FatalConditionHandlerGuard _(&m_fatalConditionhandler);
    (void)_;

    m_activeTestCase->invoke();
}

void RunContext::handleUnfinishedSections() {
    for (auto it = m_unfinishedSections.rbegin(), itEnd = m_unfinishedSections.rend(); it != itEnd; ++it) {
        sectionEnded(CATCH_MOVE(*it));
    }
    m_unfinishedSections.clear();
}

void RunContext::handleExpr(AssertionInfo const& info, ITransientExpression const& expr, AssertionReaction& reaction) {
    bool negated = isFalseTest(info.resultDisposition);
    bool result = expr.getResult() != negated;

    if (result) {
        if (!m_includeSuccessfulResults) {
            assertionPassedFastPath(info.lineInfo);
        } else {
            reportExpr(info, ResultWas::Ok, &expr, negated);
        }
    } else {
        reportExpr(info, ResultWas::ExpressionFailed, &expr, negated);
        populateReaction(reaction, info.resultDisposition & ResultDisposition::Normal);
    }
}
void RunContext::reportExpr(AssertionInfo const& info, ResultWas::OfType resultType, ITransientExpression const* expr,
                            bool negated) {
    Detail::g_lastKnownLineInfo = info.lineInfo;
    AssertionResultData data(resultType, LazyExpression(negated));

    AssertionResult assertionResult{info, CATCH_MOVE(data)};
    assertionResult.m_resultData.lazyExpression.m_transientExpression = expr;

    assertionEnded(CATCH_MOVE(assertionResult));
}

void RunContext::handleMessage(AssertionInfo const& info, ResultWas::OfType resultType, std::string&& message,
                               AssertionReaction& reaction) {
    Detail::g_lastKnownLineInfo = info.lineInfo;

    AssertionResultData data(resultType, LazyExpression(false));
    data.message = CATCH_MOVE(message);
    AssertionResult assertionResult{info, CATCH_MOVE(data)};

    const auto isOk = assertionResult.isOk();
    assertionEnded(CATCH_MOVE(assertionResult));
    if (!isOk) {
        populateReaction(reaction, info.resultDisposition & ResultDisposition::Normal);
    } else if (resultType == ResultWas::ExplicitSkip) {
        // TODO: Need to handle this explicitly, as ExplicitSkip is
        reaction.shouldSkip = true;
    }
}

void RunContext::handleUnexpectedExceptionNotThrown(AssertionInfo const& info, AssertionReaction& reaction) {
    handleNonExpr(info, Catch::ResultWas::DidntThrowException, reaction);
}

void RunContext::handleUnexpectedInflightException(AssertionInfo const& info, std::string&& message,
                                                   AssertionReaction& reaction) {
    Detail::g_lastKnownLineInfo = info.lineInfo;

    AssertionResultData data(ResultWas::ThrewException, LazyExpression(false));
    data.message = CATCH_MOVE(message);
    AssertionResult assertionResult{info, CATCH_MOVE(data)};
    assertionEnded(CATCH_MOVE(assertionResult));
    populateReaction(reaction, info.resultDisposition & ResultDisposition::Normal);
}

void RunContext::populateReaction(AssertionReaction& reaction, bool has_normal_disposition) {
    reaction.shouldDebugBreak = m_shouldDebugBreak;
    reaction.shouldThrow = aborting() || has_normal_disposition;
}

AssertionInfo RunContext::makeDummyAssertionInfo() {
    const bool testCaseJustStarted = Detail::g_lastKnownLineInfo == m_activeTestCase->getTestCaseInfo().lineInfo;

    return AssertionInfo{testCaseJustStarted ? "TEST_CASE"_sr : StringRef(), Detail::g_lastKnownLineInfo,
                         testCaseJustStarted ? StringRef() : "{Unknown expression after the reported line}"_sr,
                         ResultDisposition::Normal};
}

void RunContext::handleIncomplete(AssertionInfo const& info) {
    using namespace std::string_literals;
    Detail::g_lastKnownLineInfo = info.lineInfo;

    AssertionResultData data(ResultWas::ThrewException, LazyExpression(false));
    data.message = "Exception translation was disabled by CATCH_CONFIG_FAST_COMPILE"s;
    AssertionResult assertionResult{info, CATCH_MOVE(data)};
    assertionEnded(CATCH_MOVE(assertionResult));
}

void RunContext::handleNonExpr(AssertionInfo const& info, ResultWas::OfType resultType, AssertionReaction& reaction) {
    AssertionResultData data(resultType, LazyExpression(false));
    AssertionResult assertionResult{info, CATCH_MOVE(data)};

    const auto isOk = assertionResult.isOk();
    if (isOk && !m_includeSuccessfulResults) {
        assertionPassedFastPath(info.lineInfo);
        return;
    }

    assertionEnded(CATCH_MOVE(assertionResult));
    if (!isOk) {
        populateReaction(reaction, info.resultDisposition & ResultDisposition::Normal);
    }
}

void IResultCapture::pushScopedMessage(MessageInfo&& message) {
    Detail::g_messageHolder().addScopedMessage(CATCH_MOVE(message));
}

void IResultCapture::popScopedMessage(unsigned int messageId) {
    Detail::g_messageHolder().removeMessage(messageId);
}

void IResultCapture::emplaceUnscopedMessage(MessageBuilder&& builder) {
    Detail::g_messageHolder().addUnscopedMessage(CATCH_MOVE(builder));
}

void IResultCapture::addUnscopedMessage(MessageInfo&& message) {
    Detail::g_messageHolder().addUnscopedMessage(CATCH_MOVE(message));
}

void seedRng(IConfig const& config) {
    sharedRng().seed(config.rngSeed());
}

unsigned int rngSeed() {
    return getCurrentContext().getConfig()->rngSeed();
}

}  // namespace Catch
