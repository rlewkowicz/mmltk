
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_REPORTER_CUMULATIVE_BASE_HPP_INCLUDED
#define CATCH_REPORTER_CUMULATIVE_BASE_HPP_INCLUDED

#include <catch2/reporters/catch_reporter_common_base.hpp>
#include <catch2/internal/catch_move_and_forward.hpp>
#include <catch2/internal/catch_unique_ptr.hpp>
#include <catch2/internal/catch_optional.hpp>

#include <string>
#include <vector>

namespace Catch {

namespace Detail {

class AssertionOrBenchmarkResult {
    Optional<AssertionStats> m_assertion;
    Optional<BenchmarkStats<>> m_benchmark;

   public:
    AssertionOrBenchmarkResult(AssertionStats const& assertion);
    AssertionOrBenchmarkResult(BenchmarkStats<> const& benchmark);

    bool isAssertion() const;
    bool isBenchmark() const;

    AssertionStats const& asAssertion() const;
    BenchmarkStats<> const& asBenchmark() const;
};
}  // namespace Detail

class CumulativeReporterBase : public ReporterBase {
   public:
    template <typename T, typename ChildNodeT>
    struct Node {
        explicit Node(T const& _value) : value(_value) {}

        using ChildNodes = std::vector<Detail::unique_ptr<ChildNodeT>>;
        T value;
        ChildNodes children;
    };
    struct SectionNode {
        explicit SectionNode(SectionStats const& _stats) : stats(_stats) {}

        bool operator==(SectionNode const& other) const {
            return stats.sectionInfo.lineInfo == other.stats.sectionInfo.lineInfo;
        }

        bool hasAnyAssertions() const;

        SectionStats stats;
        std::vector<Detail::unique_ptr<SectionNode>> childSections;
        std::vector<Detail::AssertionOrBenchmarkResult> assertionsAndBenchmarks;
        std::string stdOut;
        std::string stdErr;
    };

    using TestCaseNode = Node<TestCaseStats, SectionNode>;
    using TestRunNode = Node<TestRunStats, TestCaseNode>;

    CumulativeReporterBase(ReporterConfig&& _config) : ReporterBase(CATCH_MOVE(_config)) {}
    ~CumulativeReporterBase() override;

    void benchmarkPreparing(StringRef) override {}
    void benchmarkStarting(BenchmarkInfo const&) override {}
    void benchmarkEnded(BenchmarkStats<> const& benchmarkStats) override;
    void benchmarkFailed(StringRef) override {}

    void noMatchingTestCases(StringRef) override {}
    void reportInvalidTestSpec(StringRef) override {}
    void fatalErrorEncountered(StringRef) override {}

    void testRunStarting(TestRunInfo const&) override {}

    void testCaseStarting(TestCaseInfo const&) override {}
    void testCasePartialStarting(TestCaseInfo const&, uint64_t) override {}
    void sectionStarting(SectionInfo const& sectionInfo) override;

    void assertionStarting(AssertionInfo const&) override {}

    void assertionEnded(AssertionStats const& assertionStats) override;
    void sectionEnded(SectionStats const& sectionStats) override;
    void testCasePartialEnded(TestCaseStats const&, uint64_t) override {}
    void testCaseEnded(TestCaseStats const& testCaseStats) override;
    void testRunEnded(TestRunStats const& testRunStats) override;
    virtual void testRunEndedCumulative() = 0;

    void skipTest(TestCaseInfo const&) override {}

   protected:
    bool m_shouldStoreSuccesfulAssertions = true;
    bool m_shouldStoreFailedAssertions = true;

    Detail::unique_ptr<TestRunNode> m_testRun;

   private:
    std::vector<Detail::unique_ptr<TestCaseNode>> m_testCases;
    Detail::unique_ptr<SectionNode> m_rootSection;
    SectionNode* m_deepestSection = nullptr;
    std::vector<SectionNode*> m_sectionStack;
};

}  // namespace Catch

#endif  // CATCH_REPORTER_CUMULATIVE_BASE_HPP_INCLUDED
