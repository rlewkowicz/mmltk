

#ifndef CATCH_BENCHMARK_STATS_HPP_INCLUDED
#define CATCH_BENCHMARK_STATS_HPP_INCLUDED

#include <catch2/benchmark/catch_estimate.hpp>
#include <catch2/benchmark/catch_outlier_classification.hpp>
#include <catch2/benchmark/detail/catch_benchmark_stats_fwd.hpp>

#include <string>
#include <vector>

namespace Catch {

struct BenchmarkInfo {
    std::string name;
    double estimatedDuration;
    int iterations;
    unsigned int samples;
    unsigned int resamples;
    double clockResolution;
    double clockCost;
};

template <class Dummy>
struct BenchmarkStats {
    BenchmarkInfo info;

    std::vector<Benchmark::FDuration> samples;
    Benchmark::Estimate<Benchmark::FDuration> mean;
    Benchmark::Estimate<Benchmark::FDuration> standardDeviation;
    Benchmark::OutlierClassification outliers;
    double outlierVariance;
};

}  

#endif  // CATCH_BENCHMARK_STATS_HPP_INCLUDED
