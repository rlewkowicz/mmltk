

#ifndef CATCH_SAMPLE_ANALYSIS_HPP_INCLUDED
#define CATCH_SAMPLE_ANALYSIS_HPP_INCLUDED

#include <catch2/benchmark/catch_estimate.hpp>
#include <catch2/benchmark/catch_outlier_classification.hpp>
#include <catch2/benchmark/catch_clock.hpp>

#include <vector>

namespace Catch {
namespace Benchmark {
struct SampleAnalysis {
    std::vector<FDuration> samples;
    Estimate<FDuration> mean;
    Estimate<FDuration> standard_deviation;
    OutlierClassification outliers;
    double outlier_variance;
};
}  
}  

#endif  // CATCH_SAMPLE_ANALYSIS_HPP_INCLUDED
