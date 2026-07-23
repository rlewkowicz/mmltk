

#ifndef CATCH_INTERFACES_REPORTER_HPP_INCLUDED
#define CATCH_INTERFACES_REPORTER_HPP_INCLUDED

#include <catch2/catch_section_info.hpp>
#include <catch2/catch_test_run_info.hpp>
#include <catch2/catch_totals.hpp>
#include <catch2/catch_assertion_result.hpp>
#include <catch2/internal/catch_message_info.hpp>
#include <catch2/internal/catch_stringref.hpp>
#include <catch2/internal/catch_unique_ptr.hpp>
#include <catch2/benchmark/detail/catch_benchmark_stats.hpp>

#include <map>
#include <string>
#include <vector>

namespace Catch {

struct ReporterDescription;
struct ListenerDescription;
struct TagInfo;
struct TestCaseInfo;
class TestCaseHandle;
class IConfig;
class IStream;
enum class ColourMode : std::uint8_t;

struct ReporterConfig {
    ReporterConfig(IConfig const* _fullConfig, Detail::unique_ptr<IStream> _stream, ColourMode colourMode,
                   std::map<std::string, std::string> customOptions);

    ReporterConfig(ReporterConfig&&) = default;
    ReporterConfig& operator=(ReporterConfig&&) = default;
    ~ReporterConfig();

    Detail::unique_ptr<IStream> takeStream() &&;
    IConfig const* fullConfig() const;
    ColourMode colourMode() const;
    std::map<std::string, std::string> const& customOptions() const;

   private:
    Detail::unique_ptr<IStream> m_stream;
    IConfig const* m_fullConfig;
    ColourMode m_colourMode;
    std::map<std::string, std::string> m_customOptions;
};

struct AssertionStats {
    AssertionStats(AssertionResult const& _assertionResult, std::vector<MessageInfo> const& _infoMessages,
                   Totals const& _totals);

    AssertionStats(AssertionStats const&) = default;
    AssertionStats(AssertionStats&&) = default;
    AssertionStats& operator=(AssertionStats const&) = delete;
    AssertionStats& operator=(AssertionStats&&) = delete;

    AssertionResult assertionResult;
    std::vector<MessageInfo> infoMessages;
    Totals totals;
};

struct SectionStats {
    SectionStats(SectionInfo&& _sectionInfo, Counts const& _assertions, double _durationInSeconds,
                 bool _missingAssertions);

    SectionInfo sectionInfo;
    Counts assertions;
    double durationInSeconds;
    bool missingAssertions;
};

struct TestCaseStats {
    TestCaseStats(TestCaseInfo const& _testInfo, Totals const& _totals, std::string&& _stdOut, std::string&& _stdErr,
                  bool _aborting);

    TestCaseInfo const* testInfo;
    Totals totals;
    std::string stdOut;
    std::string stdErr;
    bool aborting;
};

struct TestRunStats {
    TestRunStats(TestRunInfo const& _runInfo, Totals const& _totals, bool _aborting);

    TestRunInfo runInfo;
    Totals totals;
    bool aborting;
};

struct ReporterPreferences {
    bool shouldRedirectStdOut = false;
    bool shouldReportAllAssertions = false;
    bool shouldReportAllAssertionStarts = true;
};

class IEventListener {
   protected:
    ReporterPreferences m_preferences;
    IConfig const* m_config;

   public:
    IEventListener(IConfig const* config) : m_config(config) {}

    virtual ~IEventListener();

    ReporterPreferences const& getPreferences() const {
        return m_preferences;
    }

    virtual void noMatchingTestCases(StringRef unmatchedSpec) = 0;
    virtual void reportInvalidTestSpec(StringRef invalidArgument) = 0;

    virtual void testRunStarting(TestRunInfo const& testRunInfo) = 0;

    virtual void testCaseStarting(TestCaseInfo const& testInfo) = 0;
    virtual void testCasePartialStarting(TestCaseInfo const& testInfo, uint64_t partNumber) = 0;
    virtual void sectionStarting(SectionInfo const& sectionInfo) = 0;

    virtual void benchmarkPreparing(StringRef benchmarkName) = 0;
    virtual void benchmarkStarting(BenchmarkInfo const& benchmarkInfo) = 0;
    virtual void benchmarkEnded(BenchmarkStats<> const& benchmarkStats) = 0;
    virtual void benchmarkFailed(StringRef benchmarkName) = 0;

    virtual void assertionStarting(AssertionInfo const& assertionInfo) = 0;

    virtual void assertionEnded(AssertionStats const& assertionStats) = 0;

    virtual void sectionEnded(SectionStats const& sectionStats) = 0;
    virtual void testCasePartialEnded(TestCaseStats const& testCaseStats, uint64_t partNumber) = 0;
    virtual void testCaseEnded(TestCaseStats const& testCaseStats) = 0;
    virtual void testRunEnded(TestRunStats const& testRunStats) = 0;

    virtual void skipTest(TestCaseInfo const& testInfo) = 0;

    virtual void fatalErrorEncountered(StringRef error) = 0;

    virtual void listReporters(std::vector<ReporterDescription> const& descriptions) = 0;
    virtual void listListeners(std::vector<ListenerDescription> const& descriptions) = 0;
    virtual void listTests(std::vector<TestCaseHandle> const& tests) = 0;
    virtual void listTags(std::vector<TagInfo> const& tags) = 0;
};
using IEventListenerPtr = Detail::unique_ptr<IEventListener>;

}  

#endif  // CATCH_INTERFACES_REPORTER_HPP_INCLUDED
