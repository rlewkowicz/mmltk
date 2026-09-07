

#ifndef CATCH_REPORTER_EVENT_LIST_HPP_INCLUDED
#define CATCH_REPORTER_EVENT_LIST_HPP_INCLUDED

// X-macro tables over the IEventListener event methods, used to generate the
// per-event boilerplate in EventListenerBase and MultiReporter.
//
// F1(name, Arg1Type, arg1) expands one-argument events,
// F2(name, Arg1Type, arg1, Arg2Type, arg2) expands two-argument events.

// Events that MultiReporter forwards verbatim to every reporter-like.
#define CATCH_REPORTER_FORWARDED_EVENTS(F1, F2)                                      \
    F1(noMatchingTestCases, StringRef, unmatchedSpec)                                \
    F1(fatalErrorEncountered, StringRef, error)                                      \
    F1(reportInvalidTestSpec, StringRef, arg)                                        \
    F1(benchmarkPreparing, StringRef, name)                                          \
    F1(benchmarkStarting, BenchmarkInfo const&, benchmarkInfo)                       \
    F1(benchmarkEnded, BenchmarkStats<> const&, benchmarkStats)                      \
    F1(benchmarkFailed, StringRef, error)                                            \
    F1(testRunStarting, TestRunInfo const&, testRunInfo)                             \
    F1(testCaseStarting, TestCaseInfo const&, testInfo)                              \
    F2(testCasePartialStarting, TestCaseInfo const&, testInfo, uint64_t, partNumber) \
    F1(sectionStarting, SectionInfo const&, sectionInfo)                             \
    F1(assertionStarting, AssertionInfo const&, assertionInfo)                       \
    F1(sectionEnded, SectionStats const&, sectionStats)                              \
    F1(testCaseEnded, TestCaseStats const&, testCaseStats)                           \
    F1(testRunEnded, TestRunStats const&, testRunStats)                              \
    F1(skipTest, TestCaseInfo const&, testInfo)                                      \
    F1(listReporters, std::vector<ReporterDescription> const&, descriptions)         \
    F1(listListeners, std::vector<ListenerDescription> const&, descriptions)         \
    F1(listTests, std::vector<TestCaseHandle> const&, tests)                         \
    F1(listTags, std::vector<TagInfo> const&, tags)

// Events for which MultiReporter has bespoke handling.
#define CATCH_REPORTER_CUSTOM_EVENTS(F1, F2)                  \
    F1(assertionEnded, AssertionStats const&, assertionStats) \
    F2(testCasePartialEnded, TestCaseStats const&, testCaseStats, uint64_t, partNumber)

// Every IEventListener event method.
#define CATCH_REPORTER_ALL_EVENTS(F1, F2)   \
    CATCH_REPORTER_FORWARDED_EVENTS(F1, F2) \
    CATCH_REPORTER_CUSTOM_EVENTS(F1, F2)

// Common expansions: overriding declarations for reporter classes.
#define CATCH_REPORTER_DECLARE_EVENT1(name, Arg1Type, arg1) void name(Arg1Type arg1) override;
#define CATCH_REPORTER_DECLARE_EVENT2(name, Arg1Type, arg1, Arg2Type, arg2) \
    void name(Arg1Type arg1, Arg2Type arg2) override;

#endif  // CATCH_REPORTER_EVENT_LIST_HPP_INCLUDED
