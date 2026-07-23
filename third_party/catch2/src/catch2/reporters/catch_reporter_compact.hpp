

#ifndef CATCH_REPORTER_COMPACT_HPP_INCLUDED
#define CATCH_REPORTER_COMPACT_HPP_INCLUDED

#include <catch2/reporters/catch_reporter_streaming_base.hpp>

namespace Catch {

class CompactReporter final : public StreamingReporterBase {
   public:
    CompactReporter(ReporterConfig&& _config) : StreamingReporterBase(CATCH_MOVE(_config)) {
        m_preferences.shouldReportAllAssertionStarts = false;
    }

    ~CompactReporter() override;

    static std::string getDescription();

    void noMatchingTestCases(StringRef unmatchedSpec) override;

    void testRunStarting(TestRunInfo const& _testInfo) override;

    void assertionEnded(AssertionStats const& _assertionStats) override;

    void sectionEnded(SectionStats const& _sectionStats) override;

    void testRunEnded(TestRunStats const& _testRunStats) override;
};

}  

#endif  // CATCH_REPORTER_COMPACT_HPP_INCLUDED
